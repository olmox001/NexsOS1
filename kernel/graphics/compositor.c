/*
 * kernel/graphics/compositor.c
 * Window Compositor
 *
 * Manages windows and composites them to the screen.
 */
#include <drivers/gpu/gpu.h>
#include <drivers/virtio_input.h>
#include <graphics/gl.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/graphics.h>
#include <kernel/kmalloc.h>

/* Disable optimizations to ensure stack safety/debugging */

#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <stdint.h>

#define MAX_WINDOWS 32

struct window {
  int id;
  int x, y;
  int width, height;
  int z_order;
  int visible;
  int pid;
  int protected;          /* If true, cannot be closed */
  int top_most;           /* If true, always on top and no decorations */
  uint32_t *buffer;       /* Window's pixel buffer */
  uint32_t bg_color;      /* Default background color */
  uint32_t curr_bg_color; /* Current ANSI background color */
  char title[64];

  /* Terminal State */
  int cursor_x, cursor_y;
  int cursor_visible;  /* VT100 DECTCEM (\x1b[?25h/l); 1 = draw the caret */
  int caret_px, caret_py; /* cell where the caret was last painted */
  int caret_shown;        /* 1 = a caret is currently baked at caret_px/py */
  uint8_t *text_grid;  /* Character grid */
  uint32_t *attr_grid; /* Attribute grid (colors) */
  int grid_cols, grid_rows;
  uint32_t fg_color;
  int escape_state;
  char escape_buf[32];
  int escape_len;

  /* Compositor flags */
  int has_alpha; /* Se 1, contiene trasparenze e non occlude i layer inferiori
                  */
};

/* Global State */
static struct window windows[MAX_WINDOWS];
static int window_count = 0;
static int next_window_id = 100;
static volatile int compositor_dirty = 1;
static DEFINE_SPINLOCK(compositor_lock);

/* Damage rect: tracks the bounding box of pixels that need GPU upload */
static int damage_x1 = 0, damage_y1 = 0;
static int damage_x2 = 0, damage_y2 = 0;

/* Helper to expand damage region */
static void expand_damage(int x, int y, int w, int h) {
  if (x < damage_x1)
    damage_x1 = x;
  if (y < damage_y1)
    damage_y1 = y;
  if (x + w > damage_x2)
    damage_x2 = x + w;
  if (y + h > damage_y2)
    damage_y2 = y + h;
  compositor_dirty = 1;
}

/* Pre-allocated buffers for rendering to avoid stack usage and kmalloc in IRQ
 */
static struct window *sorted_windows[MAX_WINDOWS];
static struct region *visible_regions_store[MAX_WINDOWS];

/* Mouse State */
static int mouse_x = 400;
static int mouse_y = 300;
// static uint32_t mouse_color = 0xFFFFFFFF;

/* Dragging State */
static int dragging_window_id = -1;
static int drag_off_x = 0;
static int drag_off_y = 0;

/* Title bar dimensions */
#define TITLE_BAR_HEIGHT 20
#define CLOSE_BUTTON_SIZE 16

/* Global backbuffer - pre-allocated to avoid IRQ malloc */
static uint32_t *compositor_backbuffer = NULL;
static int bb_width = 0;
static int bb_height = 0;

/*
 * Initialize Compositor
 */
void compositor_init(void) {
  memset(windows, 0, sizeof(windows));
  window_count = 0;
  next_window_id = 100;

  /* Pre-allocate backbuffer for 720x1280 (can resize later) */
  bb_width = 720;
  bb_height = 1280;
  compositor_backbuffer = kmalloc(bb_width * bb_height * 4);

  /* Initialize damage rect to full screen so the first frame is fully uploaded
   */
  damage_x1 = 0;
  damage_y1 = 0;
  damage_x2 = bb_width;
  damage_y2 = bb_height;
  if (!compositor_backbuffer) {
    pr_err("%s", "Compositor: Failed to allocate backbuffer!\n");
  }

  pr_info("%s", "Compositor: Initialized\n");
}

/* Forward Declarations */
static void compositor_render_internal(void);
static void draw_rect_internal(int window_id, int x, int y, int w, int h,
                               uint32_t color, int caller_pid);

/*
 * Create Window
 */
/*
 * Interrupt Locking Helpers
 * Prevent nested interrupts by saving/restoring PSTATE.DAIF
 */
/* Interrupt Locking Helpers from cpu.h */

int compositor_create_window(int x, int y, int w, int h, const char *title,
                             int pid) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);

  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
    pr_err("Compositor: Invalid window dimensions %dx%d\n", w, h);
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }

  if (window_count >= MAX_WINDOWS) {
    pr_err("%s", "Compositor: Max windows reached\n");
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }

  /* Find free slot */
  int slot = -1;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == 0) {
      slot = i;
      break;
    }
  }

  if (slot < 0) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }

  /* Allocate window buffer */
  size_t buffer_size = w * h * sizeof(uint32_t);
  uint32_t *buffer = (uint32_t *)kmalloc(buffer_size);
  if (!buffer) {
    pr_err("%s", "Compositor: Failed to allocate window buffer\n");
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }
  /* Initialize clear background - use a consistent dark theme */
  uint32_t default_bg = 0xFF1a1a2e;
  for (int i = 0; i < w * h; i++)
    buffer[i] = default_bg;

  /* Initialize window */
  windows[slot].id = next_window_id++;
  windows[slot].x = x;
  windows[slot].y = y;
  windows[slot].width = w;
  windows[slot].height = h;
  windows[slot].z_order = window_count;
  windows[slot].visible = 1;
  windows[slot].pid = pid;
  windows[slot].buffer = buffer;
  windows[slot].bg_color = default_bg;
  windows[slot].curr_bg_color = default_bg;

  /* Initialize text grids using dynamic font metrics */
  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  windows[slot].grid_cols = w / char_w;
  windows[slot].grid_rows = h / char_h;
  size_t grid_size = windows[slot].grid_cols * windows[slot].grid_rows;
  windows[slot].text_grid = (uint8_t *)kmalloc(grid_size);
  windows[slot].attr_grid = (uint32_t *)kmalloc(grid_size * 4);
  if (windows[slot].text_grid && windows[slot].attr_grid) {
    memset(windows[slot].text_grid, ' ', grid_size);
    for (size_t i = 0; i < grid_size; i++)
      windows[slot].attr_grid[i] = 0xFFFFFFFF;
  } else {
    /* Handle failure of grid allocation */
    if (windows[slot].text_grid)
      kfree(windows[slot].text_grid);
    if (windows[slot].attr_grid)
      kfree(windows[slot].attr_grid);
    kfree(buffer);
    windows[slot].id = 0;
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }
  /* Copy title */
  int len = 0;
  while (title[len] && len < 63) {
    windows[slot].title[len] = title[len];
    len++;
  }
  windows[slot].title[len] = '\0';

  /* Initialize terminal state */
  windows[slot].cursor_x = 0;
  windows[slot].cursor_y = 0;
  windows[slot].cursor_visible = 1;
  windows[slot].caret_shown = 0;
  windows[slot].caret_px = 0;
  windows[slot].caret_py = 0;
  windows[slot].fg_color = 0xFFFFFFFF;
  windows[slot].escape_state = 0;
  windows[slot].escape_len = 0;

  /* Attiva il supporto alpha blending per default */
  windows[slot].has_alpha = 1;

  /* Clear buffer to background */
  for (int i = 0; i < w * h; i++) {
    buffer[i] = windows[slot].bg_color;
  }

  /* Mark main shell (PID 2) as protected */
  windows[slot].protected = (pid == 2) ? 1 : 0;
  windows[slot].top_most = 0;

  window_count++;

  pr_info("Compositor: Created window '%s' (%dx%d) at (%d,%d)\n", title, w, h,
          x, y);
  spin_unlock_irqrestore(&compositor_lock, flags);
  return windows[slot].id;
}

/*
 * __focus_topmost_locked - re-point keyboard focus after a window goes away.
 *
 * Picks the top-most remaining visible window (highest z_order) and gives it
 * keyboard focus; falls back to the shell default (PID 7) when no window is
 * left.  Replaces the old hardcoded 'keyboard_focus_pid = 7' reset, which
 * sent input to a stale/wrong PID whenever the shell was not PID 7 (PID
 * numbering depends on boot service order) and ignored Z-order entirely.
 *
 * Caller MUST hold compositor_lock; destroyed slots are already zeroed and
 * thus excluded by the id != 0 check.
 */
/* Erase the caret on every window not owned by keep_pid (caller holds
 * compositor_lock).  Keeps the caret on the input window only. */
static void __clear_other_carets_locked(int keep_pid);

static void __focus_topmost_locked(void) {
  int max_z = -1;
  int pid = 7; /* shell default when no window remains */
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].visible && windows[i].z_order > max_z) {
      max_z = windows[i].z_order;
      pid = windows[i].pid;
    }
  }
  keyboard_focus_pid = pid;
  __clear_other_carets_locked(pid);
  pr_debug("Compositor: Focus reset to PID %d\n", pid);
}

/*
 * compositor_window_owner - return the owning PID of a window, or -1 if the
 * window id does not exist.  Used by the syscall layer for the ABI-04
 * ownership check on SYS_DESTROY_WINDOW (kernel-internal callers like the
 * close button or process teardown bypass the check by design).
 */
int compositor_window_owner(int window_id) {
  uint64_t flags;
  int owner = -1;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      owner = windows[i].pid;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return owner;
}

/*
 * compositor_window_grid - report a window's terminal grid (cols x rows).
 *
 * The terminal cell size derives from the active font (proportional fonts
 * give a cell == max glyph advance), so a windowed TTY app cannot assume a
 * fixed 80x25.  Returns 0 and fills cols/rows on success, -1 if the window
 * id does not exist.  Backs SYS_WINDOW_GRID.
 */
int compositor_window_grid(int window_id, int *cols, int *rows) {
  uint64_t flags;
  int found = -1;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      if (cols)
        *cols = windows[i].grid_cols;
      if (rows)
        *rows = windows[i].grid_rows;
      found = 0;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return found;
}

/*
 * Destroy Window
 */
void compositor_destroy_window(int window_id) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      int refocus = (windows[i].pid == keyboard_focus_pid);
      if (windows[i].buffer) {
        kfree(windows[i].buffer);
      }
      if (windows[i].text_grid)
        kfree(windows[i].text_grid);
      if (windows[i].attr_grid)
        kfree(windows[i].attr_grid);
      memset(&windows[i], 0, sizeof(struct window));
      window_count--;
      if (refocus) {
        /* The focused window is gone: hand focus to the next in Z-order. */
        __focus_topmost_locked();
      }
      spin_unlock_irqrestore(&compositor_lock, flags);
      return;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/*
 * Destroy all windows owned by a specific PID
 */
void compositor_destroy_windows_by_pid(int pid) {
  uint64_t flags;
  int refocus = 0;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].pid == pid) {
      if (windows[i].pid == keyboard_focus_pid) {
        refocus = 1;
      }
      if (windows[i].buffer) {
        kfree(windows[i].buffer);
      }
      if (windows[i].text_grid) {
        kfree(windows[i].text_grid);
      }
      if (windows[i].attr_grid) {
        kfree(windows[i].attr_grid);
      }
      memset(&windows[i], 0, sizeof(struct window));
      window_count--;
    }
  }
  if (refocus) {
    /* All of the dying PID's windows are zeroed by now, so the scan picks
     * the top-most SURVIVING window (or the shell default). */
    __focus_topmost_locked();
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/*
 * Get Window Buffer (for direct drawing)
 */
uint32_t *compositor_get_buffer(int window_id) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      return windows[i].buffer;
    }
  }
  return NULL;
}

/*
 * Find window by PID
 */
int compositor_get_window_by_pid(int pid) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].pid == pid) {
      int id = windows[i].id;
      spin_unlock_irqrestore(&compositor_lock, flags);
      return id;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return -1;
}

/*
 * Get PID of the focused window (top-most Z-order)
 */
int compositor_get_focus_pid(void) {
  uint64_t flags;
  /* Use trylock to avoid blocking in timer IRQ context */
  if (!spin_trylock_irqsave(&compositor_lock, &flags))
    return -1;
  int max_z = -1;
  int pid = -1;

  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].visible) {
      if (windows[i].z_order > max_z) {
        max_z = windows[i].z_order;
        pid = windows[i].pid;
      }
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return pid;
}

/*
 * Move Window
 */
void compositor_move_window(int window_id, int x, int y) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      windows[i].x = x;
      windows[i].y = y;
      return;
    }
  }
}

/*
 * Alpha blend two colors
 */
static inline uint32_t blend_pixel(uint32_t fg, uint32_t bg) {
  uint32_t alpha = (fg >> 24) & 0xFF;
  if (alpha == 255)
    return fg;
  if (alpha == 0)
    return bg;

  uint32_t inv_alpha = 255 - alpha;
  uint32_t r =
      (((fg >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * inv_alpha) >> 8;
  uint32_t g =
      (((fg >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * inv_alpha) >> 8;
  uint32_t b = ((fg & 0xFF) * alpha + (bg & 0xFF) * inv_alpha) >> 8;

  return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/*
 * Draw Window Decorations (Title Bar)
 */

/*
 * Process ANSI SGR (Select Graphic Rendition) parameters
 */
static void handle_sgr(struct window *win) {
  if (win->escape_len == 0) {
    win->fg_color = 0xFFFFFFFF;
    return;
  }

  int val = 0;
  for (int i = 0; i < win->escape_len; i++) {
    if (win->escape_buf[i] >= '0' && win->escape_buf[i] <= '9') {
      val = val * 10 + (win->escape_buf[i] - '0');
    }
  }

  if (val == 0) {
    win->fg_color = 0xFFFFFFFF;
    win->curr_bg_color = win->bg_color;
  } else if (val == 39) {
    win->fg_color = 0xFFFFFFFF; /* default foreground */
  } else if (val == 49) {
    win->curr_bg_color = win->bg_color; /* default background */
  } else if (val >= 30 && val <= 37) {
    uint32_t colors[] = {0xFF000000, 0xFFBB0000, 0xFF00BB00, 0xFFBBBB00,
                         0xFF0000BB, 0xFFBB00BB, 0xFF00BBBB, 0xFFBBBBBB};
    win->fg_color = colors[val - 30];
  } else if (val >= 40 && val <= 47) {
    uint32_t colors[] = {0xFF000000, 0xFFBB0000, 0xFF00BB00, 0xFFBBBB00,
                         0xFF0000BB, 0xFFBB00BB, 0xFF00BBBB, 0xFFBBBBBB};
    win->curr_bg_color = colors[val - 40];
  } else if (val >= 90 && val <= 97) {
    uint32_t colors[] = {0xFF555555, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
                         0xFF5555FF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF};
    win->fg_color = colors[val - 90];
  } else if (val >= 100 && val <= 107) {
    uint32_t colors[] = {0xFF555555, 0xFFFF5555, 0xFF55FF55, 0xFFFFFF55,
                         0xFF5555FF, 0xFFFF55FF, 0xFF55FFFF, 0xFFFFFFFF};
    win->curr_bg_color = colors[val - 100];
  }
}

/* Parse up to two decimal CSI parameters separated by ';' from buf[0..len).
 * Returns how many parameters were present (0, 1 or 2); a and b receive their
 * values (0 when omitted). */
static int csi_params(const char *buf, int len, int *a, int *b) {
  int vals[2] = {0, 0};
  int which = 0, have = 0;
  for (int i = 0; i < len; i++) {
    char c = buf[i];
    if (c >= '0' && c <= '9') {
      vals[which] = vals[which] * 10 + (c - '0');
      have = 1;
    } else if (c == ';' && which < 1) {
      which++;
    }
  }
  *a = vals[0];
  *b = vals[1];
  return have ? which + 1 : 0;
}

/* Clear one terminal cell to the current background (pixels + grids). */
static void term_clear_cell(int win_id, struct window *win, int cx, int cy,
                            int char_w, int char_h) {
  if (cx < 0 || cy < 0 || cx >= win->grid_cols || cy >= win->grid_rows)
    return;
  draw_rect_internal(win_id, cx * char_w, cy * char_h, char_w, char_h,
                     win->curr_bg_color, 1);
  if (win->text_grid && win->attr_grid) {
    int idx = cy * win->grid_cols + cx;
    win->text_grid[idx] = ' ';
    win->attr_grid[idx] = win->curr_bg_color;
  }
}

/* Erase a previously painted caret by repainting its cell from the text/attr
 * grid (background + glyph).  Idempotent; clears caret_shown.  This is what
 * prevents stray green blocks after the cursor moves (newline/scroll): the old
 * caret cell is always restored before a new one is drawn. */
static void term_erase_caret(int win_id, struct window *win,
                             struct gl_surface *surf, int char_w, int char_h) {
  if (!win->caret_shown)
    return;
  int px = win->caret_px, py = win->caret_py;
  win->caret_shown = 0;
  if (px < 0 || py < 0 || px >= win->grid_cols || py >= win->grid_rows)
    return;
  draw_rect_internal(win_id, px * char_w, py * char_h, char_w, char_h,
                     win->bg_color, 1);
  if (win->text_grid && win->attr_grid) {
    int idx = py * win->grid_cols + px;
    char ch = (char)win->text_grid[idx];
    if (ch >= 32 && ch < 127)
      gl_draw_char(surf, px * char_w, py * char_h, ch, win->attr_grid[idx]);
  }
}

/* Draw the text caret as a 25%-transparent green block (shell accent 0x00FF88)
 * at the cursor cell, blended over the cell so the glyph stays readable.  Only
 * the window that owns keyboard focus — i.e. the one the user is typing into —
 * gets a caret; a notification or a print-only window never shows one.  The
 * previous caret (this window's) is erased first so movement leaves no trail. */
static void term_draw_cursor(int win_id, struct window *win,
                             struct gl_surface *surf, int char_w, int char_h) {
  extern int keyboard_focus_pid;

  term_erase_caret(win_id, win, surf, char_w, char_h);

  if (!win->cursor_visible || win->pid != keyboard_focus_pid)
    return;
  int cx = win->cursor_x, cy = win->cursor_y;
  if (cx < 0 || cy < 0 || cx >= win->grid_cols || cy >= win->grid_rows)
    return;

  const uint32_t caret = 0xC000FF88; /* ARGB: 75% alpha (25% transparent) */
  int px0 = cx * char_w, py0 = cy * char_h;
  for (int y = 0; y < char_h; y++) {
    int sy = py0 + y;
    if (sy < 0 || sy >= win->height)
      continue;
    for (int x = 0; x < char_w; x++) {
      int sx = px0 + x;
      if (sx < 0 || sx >= win->width)
        continue;
      uint32_t *p = &surf->buffer[sy * surf->stride + sx];
      *p = blend_pixel(caret, *p);
    }
  }
  win->caret_px = cx;
  win->caret_py = cy;
  win->caret_shown = 1;
}

/* Erase carets on every window not owned by keep_pid; marks damage so they
 * repaint.  Caller holds compositor_lock.  Used on focus changes so the caret
 * follows the input window instead of lingering on the one that lost focus. */
static void __clear_other_carets_locked(int keep_pid) {
  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  for (int i = 0; i < MAX_WINDOWS; i++) {
    struct window *win = &windows[i];
    if (win->id == 0 || !win->caret_shown || win->pid == keep_pid)
      continue;
    struct gl_surface s = {.width = win->width,
                           .height = win->height,
                           .stride = win->width,
                           .buffer = win->buffer};
    term_erase_caret(win->id, win, &s, char_w, char_h);
    expand_damage(win->x, win->y - TITLE_BAR_HEIGHT, win->width,
                  win->height + TITLE_BAR_HEIGHT);
    compositor_dirty = 1;
  }
}

/* compositor_focus_changed - public hook for SYS_SET_FOCUS: wipe the caret off
 * windows that just lost keyboard focus. */
void compositor_focus_changed(int new_pid) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  __clear_other_carets_locked(new_pid);
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/*
 * handle_csi - dispatch a complete CSI sequence (ESC [ ... <final>).
 *
 * Supports the subset a full-screen TTY app (kilo) needs: H/f (cursor
 * position, 1-based), K (erase in line), J (erase screen), m (SGR colours),
 * and the private DECTCEM ?25 h/l (cursor visibility).  Unknown finals are
 * ignored.  win->escape_buf holds the bytes between '[' and <final>.
 */
static void handle_csi(int win_id, struct window *win, char final, int char_w,
                       int char_h) {
  const char *eb = win->escape_buf;
  int len = win->escape_len;

  /* Private mode: ESC [ ? Pn h/l — we honour 25 (show/hide caret). */
  if (len > 0 && eb[0] == '?') {
    int a, b;
    csi_params(eb + 1, len - 1, &a, &b);
    if (a == 25)
      win->cursor_visible = (final == 'h');
    return;
  }

  if (final == 'm') {
    handle_sgr(win);
    return;
  }

  int a, b;
  int n = csi_params(eb, len, &a, &b);

  if (final == 'H' || final == 'f') {
    int row = (n >= 1 && a > 0) ? a - 1 : 0;
    int col = (n >= 2 && b > 0) ? b - 1 : 0;
    if (row >= win->grid_rows)
      row = win->grid_rows - 1;
    if (col >= win->grid_cols)
      col = win->grid_cols - 1;
    win->cursor_y = row < 0 ? 0 : row;
    win->cursor_x = col < 0 ? 0 : col;
  } else if (final == 'K') {
    int mode = (n >= 1) ? a : 0;
    int x0 = (mode == 1) ? 0 : win->cursor_x;
    int x1 = (mode == 0) ? win->grid_cols - 1 : win->cursor_x;
    if (mode == 2) {
      x0 = 0;
      x1 = win->grid_cols - 1;
    }
    for (int x = x0; x <= x1; x++)
      term_clear_cell(win_id, win, x, win->cursor_y, char_w, char_h);
  } else if (final == 'J') {
    for (int p = 0; p < win->width * win->height; p++)
      win->buffer[p] = win->curr_bg_color;
    if (win->text_grid && win->attr_grid) {
      memset(win->text_grid, ' ', win->grid_cols * win->grid_rows);
      for (int p = 0; p < win->grid_cols * win->grid_rows; p++)
        win->attr_grid[p] = win->curr_bg_color;
    }
    win->cursor_x = 0;
    win->cursor_y = 0;
  }
}

/*
 * Write text to a window (Terminal Emulator) - Uses GL
 */
void compositor_window_write(int win_id, const char *buf, size_t count) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  struct window *win = NULL;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == win_id) {
      win = &windows[i];
      break;
    }
  }
  if (win == NULL || win->buffer == NULL) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return;
  }

  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  int cols = win->grid_cols; /* Use pre-calculated grid size */
  int rows = win->grid_rows;

  /* Create GL Surface wrappers for window buffer */
  struct gl_surface win_surf = {.width = win->width,
                                .height = win->height,
                                .stride = win->width,
                                .buffer = win->buffer};

  for (size_t i = 0; i < count; i++) {
    char c = buf[i];

    if (win->escape_state == 0) {
      if (c == '\033') {
        win->escape_state = 1;
        win->escape_len = 0;
      } else if (c == '\n') {
        win->cursor_x = 0;
        win->cursor_y++;
      } else if (c == '\r') {
        win->cursor_x = 0;
      } else if (c == '\b' || c == 127) {
        if (win->cursor_x > 0)
          win->cursor_x--;
      } else if (c >= 32 && c < 127) {
        /* Check bounds and wrap BEFORE writing */
        if (win->cursor_x < 0)
          win->cursor_x = 0;
        if (win->cursor_y < 0)
          win->cursor_y = 0;
        if (win->cursor_x >= cols) {
          win->cursor_x = 0;
          win->cursor_y++;
        }
        if (win->cursor_y >= rows) {
          /* Scroll Pixels buffer up by one line */
          if (win->height > char_h) {
            size_t line_size = win->width * char_h;
            memmove(win->buffer, win->buffer + line_size,
                    win->width * (win->height - char_h) * 4);
          }
          /* Clear last line in pixel buffer */
          for (int p = win->width * (win->height - char_h);
               p < win->width * win->height; p++) {
            win->buffer[p] = win->bg_color;
          }

          /* Scroll Text Grids */
          if (win->text_grid && win->attr_grid) {
            memmove(win->text_grid, win->text_grid + win->grid_cols,
                    win->grid_cols * (win->grid_rows - 1));
            memmove(win->attr_grid, win->attr_grid + win->grid_cols,
                    win->grid_cols * (win->grid_rows - 1) * 4);
            /* Clear last row in grids */
            int last_row_start = win->grid_cols * (win->grid_rows - 1);
            memset(win->text_grid + last_row_start, ' ', win->grid_cols);
            for (int p = 0; p < win->grid_cols; p++)
              win->attr_grid[last_row_start + p] = 0xFFFFFFFF;
          }
          win->cursor_y = rows - 1;
        }

        /* Clear char background */
        /* Use internal unsafe draw (which writes to buffer) */
        /* Pass caller_pid = 1 to bypass security check for kernel-internal
         * write
         */
        draw_rect_internal(win_id, win->cursor_x * char_w,
                           win->cursor_y * char_h, char_w, char_h,
                           win->curr_bg_color, 1);

        gl_draw_char(&win_surf, win->cursor_x * char_w, win->cursor_y * char_h,
                     c, win->fg_color);
        /* Update grids for persistence */
        if (win->text_grid && win->attr_grid) {
          int idx = win->cursor_y * win->grid_cols + win->cursor_x;
          if (idx < win->grid_cols * win->grid_rows) {
            win->text_grid[idx] = c;
            win->attr_grid[idx] = win->fg_color;
          }
        }

        win->cursor_x++;
      }
    } else if (win->escape_state == 1) {
      if (c == '[')
        win->escape_state = 2;
      else
        win->escape_state = 0;
    } else if (win->escape_state == 2) {
      /* Final byte is a letter (CSI dispatch) or '@'; '?' and digits/';' are
       * parameter bytes accumulated in escape_buf. */
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        handle_csi(win_id, win, c, char_w, char_h);
        win->escape_state = 0;
      } else if (win->escape_len < 31) {
        win->escape_buf[win->escape_len++] = c;
      } else {
        win->escape_state = 0;
      }
    }
  }

  /* Draw the text caret at its final position for this batch. */
  term_draw_cursor(win_id, win, &win_surf, char_w, char_h);

  /* Mark compositor as needing redraw (window area including title bar) */
  expand_damage(win->x, win->y - TITLE_BAR_HEIGHT, win->width,
                win->height + TITLE_BAR_HEIGHT);
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/*
 * Draw simple mouse cursor
 */

/*
 * Handle Mouse Click
 */
void compositor_handle_click(int button, int state) {
  (void)button;

  if (state == 0) {
    dragging_window_id = -1;
    return;
  }

  if (state != 1)
    return;

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);

  struct window *hit = NULL;
  int max_z = -1;

  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].visible) {
      int title_top = windows[i].y - TITLE_BAR_HEIGHT;
      if (mouse_x >= windows[i].x &&
          mouse_x < windows[i].x + windows[i].width && mouse_y >= title_top &&
          mouse_y < windows[i].y + windows[i].height) {
        if (windows[i].z_order > max_z) {
          max_z = windows[i].z_order;
          hit = &windows[i];
        }
      }
    }
  }

  if (!hit) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return;
  }

  /* Bring to front */
  int top_z = 0;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].z_order > top_z)
      top_z = windows[i].z_order;
  }
  hit->z_order = top_z + 1;

  /* Update keyboard focus to this process */
  if (keyboard_focus_pid != hit->pid) {
    pr_info("Compositor: Focus changed to PID %d (Window '%s')\n", hit->pid,
            hit->title);
    keyboard_focus_pid = hit->pid;
  }

  /*
   * FIX(GFX-COMP-03): never call kernel_ipc_send() or process_terminate() while
   * holding compositor_lock.  compositor_handle_click runs in mouse-IRQ context;
   * kernel_ipc_send() takes sched_lock, and process_terminate() takes sched_lock
   * then re-enters the compositor (compositor_destroy_windows_by_pid ->
   * compositor_lock).  Holding compositor_lock across either is the reverse of
   * process_terminate's own sched_lock->compositor_lock order — an SMP AB-BA
   * deadlock against a concurrent kill on another CPU (the observed "freeze on
   * window-close/kill").  So we capture the work into locals under the lock and
   * perform it AFTER the single unlock below.
   */

  /* Capture the mouse event to deliver to the focused process. */
  int send_pid = -1;
  struct ipc_message msg = {0};
  if (keyboard_focus_pid > 0) {
    msg.from = 0; /* Kernel */
    msg.type = IPC_TYPE_MOUSE;
    msg.data1 = (uint64_t)button;
    msg.data2 = (uint64_t)state;
    /* Store relative coordinates in payload */
    int rel_x = mouse_x - hit->x;
    int rel_y = mouse_y - hit->y;
    memcpy(msg.payload, &rel_x, 4);
    memcpy(msg.payload + 4, &rel_y, 4);
    send_pid = keyboard_focus_pid;
  }

  /* Capture a close-button hit; the terminate is deferred until after unlock. */
  int do_close = 0;
  int close_pid = 0;
  if (!hit->protected) {
    int btn_x = hit->x + hit->width - CLOSE_BUTTON_SIZE - 2;
    int btn_y = hit->y - TITLE_BAR_HEIGHT + 2;
    if (mouse_x >= btn_x && mouse_x < btn_x + CLOSE_BUTTON_SIZE &&
        mouse_y >= btn_y && mouse_y < btn_y + CLOSE_BUTTON_SIZE) {
      do_close = 1;
      close_pid = hit->pid;
    }
  }

  /* Check for drag start (skipped when closing, matching the old early-return). */
  if (!do_close && mouse_y >= hit->y - TITLE_BAR_HEIGHT && mouse_y < hit->y) {
    dragging_window_id = hit->id;
    drag_off_x = mouse_x - hit->x;
    drag_off_y = mouse_y - hit->y;
  }

  expand_damage(0, 0, bb_width, bb_height);
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);

  /*
   * Cross-subsystem calls, now strictly OUTSIDE compositor_lock (FIX(GFX-COMP-03)).
   * Both validate their target pid internally, so a window/process that changed
   * between the unlock and here is handled gracefully (returns an error).
   * NOTE: process_terminate still runs in mouse-IRQ context — this removes the
   * freeze, but the zombie/no-reap behaviour for an IRQ-time kill is a separate
   * follow-up (process_terminate must not run from IRQ; see SCHED-03).
   */
  if (send_pid > 0)
    kernel_ipc_send(send_pid, &msg);
  if (do_close) {
    pr_info("Compositor: Close button -> terminate PID %d\n", close_pid);
    extern int process_terminate(int pid);
    process_terminate(close_pid);
  }
}

/*
 * Update Mouse Position
 */

void compositor_update_mouse(int dx, int dy, int absolute) {
  struct gpu_device *dev = gpu_get_primary();
  int width = 800; /* Fallback */
  int height = 600;

  if (dev) {
    width = dev->width;
    height = dev->height;
  }

  int old_mx = mouse_x, old_my = mouse_y;

  if (absolute) {
    /* Absolute pointer: events carry one axis at a time, so a negative component
     * means "leave this axis unchanged". Values are normalized to
     * [0, INPUT_ABS_MAX]; scale to framebuffer pixels. This is what makes the
     * cursor track 1:1 under absolute hosts like UTM (DRV-INPUT-01 #125), instead
     * of a relative device saturating at a screen edge. */
    if (dx >= 0)
      mouse_x = (int)(((long)dx * (width - 1)) / INPUT_ABS_MAX);
    if (dy >= 0)
      mouse_y = (int)(((long)dy * (height - 1)) / INPUT_ABS_MAX);
  } else {
    mouse_x += dx;
    mouse_y += dy;
  }

  /* Clamp to screen */
  if (mouse_x < 0)
    mouse_x = 0;
  if (mouse_x >= width)
    mouse_x = width - 1;
  if (mouse_y < 0)
    mouse_y = 0;
  if (mouse_y >= height)
    mouse_y = height - 1;

  /* Handle Dragging */
  if (dragging_window_id != -1) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
      if (windows[i].id == dragging_window_id) {
        windows[i].x = mouse_x - drag_off_x;
        windows[i].y = mouse_y - drag_off_y;
        /* Enforce screen boundaries */
        if (windows[i].x < 0)
          windows[i].x = 0;
        if (windows[i].x + windows[i].width > width)
          windows[i].x = width - windows[i].width;
        if (windows[i].y < TITLE_BAR_HEIGHT)
          windows[i].y = TITLE_BAR_HEIGHT;
        if (windows[i].y + windows[i].height > height)
          windows[i].y = height - windows[i].height;
        break;
      }
    }
  }

  /* Mark compositor as needing redraw - don't render from IRQ! */
  if (dragging_window_id != -1) {
    expand_damage(0, 0, bb_width, bb_height);
  } else {
    /* Only the old and new cursor areas (12x16 + 1px border) */
    expand_damage(old_mx - 1, old_my - 1, 14, 18);
    expand_damage(mouse_x - 1, mouse_y - 1, 14, 18);
  }
  compositor_dirty = 1;
}

/*
 * Composite All Windows to Screen
 */
/*
 * Compositor Render (HAL + GL)
 */
/*
 * Compositor Render (Region-based / Front-to-Back with Occlusion Culling)
 */
#include <kernel/region.h>

static volatile int in_render = 0;
static void compositor_render_internal(void) {
  /* Atomic guard against concurrent rendering (multi-CPU or IRQ re-entrancy) */
  if (__sync_lock_test_and_set(&in_render, 1))
    return;

  struct gpu_device *dev = gpu_get_primary();
  if (!dev || !compositor_backbuffer) {
    __sync_lock_release(&in_render);
    return;
  }

  /* Use current buffer dimensions */
  int bb_w = bb_width;
  int bb_h = bb_height;
  uint32_t *backbuffer = compositor_backbuffer;

  /* Wrap backbuffer in GL Surface */
  struct gl_surface screen = {
      .width = bb_w, .height = bb_h, .stride = bb_w, .buffer = backbuffer};

  /* Use static buffers to avoid stack pressure/smashing */
  struct window **sorted = sorted_windows;
  struct region **visible_regions = visible_regions_store;

  memset(visible_regions, 0, sizeof(struct region *) * MAX_WINDOWS);

  int count = 0;
  for (int i = 0; i < MAX_WINDOWS && count < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].visible) {
      /* Skip off-screen */
      if (windows[i].x >= bb_w || windows[i].y >= bb_h ||
          windows[i].x + windows[i].width <= 0 ||
          windows[i].y + windows[i].height <= 0)
        continue;
      sorted[count++] = &windows[i];
    }
  }

  /* Bubble Sort */
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - i - 1; j++) {
      if (sorted[j]->z_order > sorted[j + 1]->z_order) {
        struct window *tmp = sorted[j];
        sorted[j] = sorted[j + 1];
        sorted[j + 1] = tmp;
      }
    }
  }

  /* Top Most handling */
  /* Top Most handling: move all top-most windows to the end of the sorted list
   */
  int current_count = count;
  for (int i = 0; i < current_count; i++) {
    if (sorted[i]->top_most) {
      struct window *tmp = sorted[i];
      /* Shift remaining windows left */
      for (int k = i; k < current_count - 1; k++) {
        sorted[k] = sorted[k + 1];
      }
      sorted[current_count - 1] = tmp;
      /* Decrement current_count so we don't re-process the window we just moved
       */
      current_count--;
      /* Decrement i to process the window that was shifted into the current
       * slot */
      i--;
    }
  }

  /*
   * Two-Pass Rendering Algorithm
   * Pass 1: Visibility Calculation (Top-Down)
   * computes what part of each window is visible.
   */
  struct region *occluded = region_create();
  if (!occluded) {
    __sync_lock_release(&in_render);
    return;
  }

  /* Iterate Top-to-Bottom for Occlusion */
  for (int i = count - 1; i >= 0 && i < MAX_WINDOWS; i--) {
    struct window *win = sorted[i];

    /* Calculate Full Window Bounds (Content + Decorations) */
    int win_y = win->top_most ? win->y : win->y - TITLE_BAR_HEIGHT;
    int win_h = win->top_most ? win->height : win->height + TITLE_BAR_HEIGHT;

    struct region *vis = region_create();
    if (vis) {
      region_add_rect(vis, win->x, win_y, win->width, win_h);

      /* Subtract currently occluded area */
      for (int r = 0; r < occluded->count; r++) {
        struct rect *or = &occluded->rects[r];
        region_subtract(vis, or->x, or->y, or->w, or->h);
      }

      /* Clip to screen bounds */
      region_intersect_rect(vis, 0, 0, bb_w, bb_h);
    }

    visible_regions[i] = vis;

    /* Aggiungi a Occluded (Solo se la finestra non contiene trasparenze) */
    if (!win->has_alpha) {
      region_add_rect(occluded, win->x, win_y, win->width, win_h);
    }
  }

  /* Calculate Background Region (Screen - Occluded) */
  struct region *bg_region = region_create();
  if (bg_region) {
    region_add_rect(bg_region, 0, 0, bb_w, bb_h);
    for (int r = 0; r < occluded->count; r++) {
      struct rect *or = &occluded->rects[r];
      region_subtract(bg_region, or->x, or->y, or->w, or->h);
    }

    /* Draw Background */
    for (int r = 0; r < bg_region->count; r++) {
      struct rect *bg = &bg_region->rects[r];
      for (int y = 0; y < bg->h; y++) {
        for (int x = 0; x < bg->w; x++) {
          int sy = bg->y + y;
          int sx = bg->x + x;

          /* Final backbuffer bounds safety check */
          if (sx >= 0 && sx < bb_w && sy >= 0 && sy < bb_h) {
            /* Proper Gradient Background */
            uint32_t r_chk = 20;
            uint32_t g_chk = 40 + (sy * 40 / bb_h);
            uint32_t b_chk = 80 + (sy * 80 / bb_h);
            backbuffer[sy * bb_w + sx] =
                0xFF000000 | (r_chk << 16) | (g_chk << 8) | b_chk;
          }
        }
      }
    }
    region_destroy(bg_region);
  }
  region_destroy(occluded);
  occluded = NULL; /* prevent double-free: cleanup at end of function also calls region_destroy(occluded) */

  /* Pass 2: Rendering (Bottom-Up) - Painter's Algorithm with Clipping */
  for (int i = 0; i < count && i < MAX_WINDOWS; i++) {
    struct window *win = sorted[i];
    struct region *vis = visible_regions[i];

    /* Decoration Params */
    int title_h = win->top_most ? 0 : TITLE_BAR_HEIGHT;
    int content_y = win->y;
    int decor_y = win->y - title_h;

    if (vis) {
      /* Iterate Visible Rects */
      for (int r = 0; r < vis->count; r++) {
        struct rect *vr = &vis->rects[r];

        /* Draw pixels for this visible rect */
        for (int dy = 0; dy < vr->h; dy++) {
          for (int dx = 0; dx < vr->w; dx++) {
            int screen_x = vr->x + dx;
            int screen_y = vr->y + dy;
            int screen_idx = screen_y * bb_w + screen_x;

            /* Determine if we are in Decoration or Content */
            if (screen_y < content_y) {
              /* Decoration Area (Title Bar) */
              if (screen_y >= decor_y) {
                /* In Title Bar */
                /* Check for Close Button */
                int btn_start_x = win->x + win->width - CLOSE_BUTTON_SIZE - 2;
                if (screen_x >= btn_start_x &&
                    screen_x < btn_start_x + CLOSE_BUTTON_SIZE &&
                    screen_y >= decor_y + 2 &&
                    screen_y < decor_y + 2 + CLOSE_BUTTON_SIZE) {
                  backbuffer[screen_idx] = 0xFFCC4444; /* Red Button */
                } else {
                  backbuffer[screen_idx] = 0xFF18181B; /* Dark Title Bar */
                }
              }
            } else {
              /* Content Area */
              /* Map to Window Buffer Coords */
              int win_local_x = screen_x - win->x;
              int win_local_y = screen_y - win->y;

              if (win_local_x >= 0 && win_local_x < win->width &&
                  win_local_y >= 0 && win_local_y < win->height) {

                if (win->buffer) {
                  uint32_t pixel =
                      win->buffer[win_local_y * win->width + win_local_x];
                  backbuffer[screen_idx] =
                      blend_pixel(pixel, backbuffer[screen_idx]);
                } else {
                  backbuffer[screen_idx] =
                      blend_pixel(win->bg_color, backbuffer[screen_idx]);
                }
              }
            }
          } // dx
        } // dy
      }
      region_destroy(vis);
      visible_regions[i] = NULL;
    }

    /* Draw Title Text - Naive Unclipped (Relies on Painter's Algo overwriting)
     */
    if (!win->top_most) {
      int title_len = 0;
      while (win->title[title_len] && title_len < 63)
        title_len++;

      int char_h = graphics_font_height();
      int text_w = graphics_string_width(win->title);
      int start_x = win->x + (win->width - text_w) / 2;
      int start_y = decor_y + (20 - char_h) / 2; /* Center vertically in title bar */

      gl_draw_string(&screen, start_x, start_y, win->title, 0xFFFFFFFF);
    }
  }

  /* Cleanup any remaining regions in store */
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (visible_regions_store[i]) {
      region_destroy(visible_regions_store[i]);
      visible_regions_store[i] = NULL;
    }
  }

  /* Mouse Cursor (Always on top) */
  static const char *cursor_bits[] = {
      "X           ", "XX          ", "X.X         ", "X..X        ",
      "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
      "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
      "X.X X..X    ", "XX  X..X    ", "X    XX     ", "     XX     "};
  int c_h = 16;
  int c_w = 12;
  for (int y = 0; y < c_h; y++) {
    for (int x = 0; x < c_w; x++) {
      int px = mouse_x + x;
      int py = mouse_y + y;
      if (px >= 0 && px < bb_w && py >= 0 && py < bb_h) {
        char p = cursor_bits[y][x];
        if (p == 'X')
          backbuffer[py * bb_w + px] = 0xFFFFFFFF; // Border White
        else if (p == '.')
          backbuffer[py * bb_w + px] = 0xFF000000; // Fill Black
      }
    }
  }

  /* Flush — only upload the damage bounding box instead of the full framebuffer
   */
  if (dev->ops && dev->ops->flush && dev->ops->get_framebuffer) {
    void *fb_va = dev->ops->get_framebuffer(dev, NULL);
    if (fb_va) {
      int dx1 = damage_x1 < 0 ? 0 : damage_x1;
      int dy1 = damage_y1 < 0 ? 0 : damage_y1;
      int dx2 = damage_x2 > bb_w ? bb_w : damage_x2;
      int dy2 = damage_y2 > bb_h ? bb_h : damage_y2;
      if (dx1 < dx2 && dy1 < dy2) {
        int row_bytes = (dx2 - dx1) * 4;
        uint8_t *dst = (uint8_t *)fb_va;
        const uint8_t *src = (const uint8_t *)backbuffer;
        for (int row = dy1; row < dy2; row++) {
          memcpy(dst + ((size_t)row * bb_w + dx1) * 4,
                 src + ((size_t)row * bb_w + dx1) * 4, row_bytes);
        }
        dev->ops->flush(dev, dx1, dy1, dx2 - dx1, dy2 - dy1);
      }
      /* Reset damage: invalid state (x1>x2) means nothing to flush */
      damage_x1 = bb_w;
      damage_y1 = bb_h;
      damage_x2 = 0;
      damage_y2 = 0;
    }
  }
  /* Cleanup regions */
  region_destroy(occluded);
  for (int i = 0; i < count; i++) {
    if (visible_regions[i]) {
      region_destroy(visible_regions[i]);
      visible_regions[i] = NULL;
    }
  }

  __sync_lock_release(&in_render);
}

/*
 * Composite All Windows to Screen (Public - Locks)
 */
void compositor_render(void) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  compositor_render_internal();
  compositor_dirty = 0;
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/*
 * Compositor Tick - Called from timer interrupt
 * Renders if dirty flag is set (avoids re-render on every event)
 */
void compositor_tick(void) {
  uint64_t flags;
  /* Use trylock to avoid blocking timer IRQ if compositor is busy */
  if (spin_trylock_irqsave(&compositor_lock, &flags)) {
    if (compositor_dirty) {
      compositor_dirty = 0;
      compositor_render_internal();
    }
    spin_unlock_irqrestore(&compositor_lock, flags);
  }
}

/*
 * Draw to Window
 */
/*
 * Draw to Window (Internal - No Locking)
 */
static void draw_rect_internal(int window_id, int x, int y, int w, int h,
                               uint32_t color, int caller_pid) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id && windows[i].buffer) {
      /* Process Isolation: Verify Ownership */
      if (windows[i].pid != caller_pid &&
          caller_pid != 1) { /* PID 1 is root/init */
        pr_warn(
            "Compositor: Process %d tried to draw to window %d (owned by %d)\n",
            caller_pid, window_id, windows[i].pid);
        return;
      }

      for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
          int px = x + dx;
          int py = y + dy;
          /* Strict bounds checking using window dimensions */
          if (px >= 0 && px < windows[i].width && py >= 0 &&
              py < windows[i].height) {
            /* Final safety check: ensure window buffer is non-null */
            if (windows[i].buffer) {
              windows[i].buffer[py * windows[i].width + px] = color;
            }
          }
        }
      }
      /* Update damage region: Window relative -> Screen relative */
      int win_y = windows[i].y + (windows[i].top_most ? 0 : TITLE_BAR_HEIGHT);
      expand_damage(windows[i].x + x, win_y + y, w, h);
      return;
    }
  }
}

/*
 * Draw to Window (Public - Locks)
 */
void compositor_draw_rect(int window_id, int x, int y, int w, int h,
                          uint32_t color, int caller_pid) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  draw_rect_internal(window_id, x, y, w, h, color, caller_pid);
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/*
 * Blit user buffer to window
 */
void compositor_blit(int window_id, int x, int y, int w, int h,
                     const uint32_t *user_buf, int caller_pid) {
  // pr_info("BLIT: win=%d pid=%d buf=%p %dx%d\n", window_id, caller_pid,
  // user_buf, w, h);
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id && windows[i].buffer) {
      /* Process Isolation: Verify Ownership */
      if (windows[i].pid != caller_pid && caller_pid != 1) {
        spin_unlock_irqrestore(&compositor_lock, flags);
        return;
      }

      /* Copy Logic: Row by Row for speed */
      for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        /* Clip Y */
        if (py < 0 || py >= windows[i].height)
          continue;

        /* Calculate source and dest pointers for the row */
        /* We assume x=0 for full width blit usually, but handle x offset */

        /* Clip X roughly: we support full width blit mainly */
        /* If x < 0, we need to skip source pixels?
           For this syscall, let's assume valid bounds or simple clipping. */

        /* Destination X start */
        int dest_x = x;
        int src_x = 0;
        int copy_w = w;

        if (dest_x < 0) {
          src_x += -dest_x;
          copy_w -= -dest_x;
          dest_x = 0;
        }

        if (dest_x + copy_w > windows[i].width) {
          copy_w = windows[i].width - dest_x;
        }

        if (copy_w <= 0)
          continue;

        /* Use copy_from_user instead of raw memcpy for security */
        void *dst_ptr = &windows[i].buffer[py * windows[i].width + dest_x];
        const void *src_ptr = &user_buf[dy * w + src_x];

        if (vmm_copy_from_user(dst_ptr, src_ptr, copy_w * sizeof(uint32_t)) !=
            0) {
          /* Page fault or invalid access: abort blit */
          spin_unlock_irqrestore(&compositor_lock, flags);
          return;
        }
      }

      /* Update damage region: Window relative -> Screen relative */
      int win_y = windows[i].y + (windows[i].top_most ? 0 : TITLE_BAR_HEIGHT);
      expand_damage(windows[i].x + x, win_y + y, w, h);

      spin_unlock_irqrestore(&compositor_lock, flags);
      return;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
}

void compositor_set_window_flags(int window_id, int flags_val) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      windows[i].top_most = (flags_val & 1) ? 1 : 0;
      if (flags_val & 4)
        windows[i].visible = 0; /* bit 2: hide window */
      else if (flags_val & 2)
        windows[i].visible = 1; /* bit 1: show window */
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
}