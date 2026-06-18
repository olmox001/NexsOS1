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

#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/term.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <stdint.h>

#define MAX_WINDOWS 32

/* Desktop */
/* ========================================================================= */
/* Desktop background                                                        */
/* ========================================================================= */

#define COLOR_BG_TOP 0xFFF2F2F7
#define COLOR_BG_BOTTOM 0xFFECEFF1

/* ========================================================================= */
/* Window                                                                    */
/* ========================================================================= */

#define COLOR_WIN_BG 0xFFFCFCFD

/* ========================================================================= */
/* Title bar                                                                 */
/* ========================================================================= */

#define COLOR_TITLE_ACTIVE 0xFFEFEFF4
#define COLOR_TITLE_INACTIVE 0xFFE5E5EA

#define COLOR_TITLE_TEXT_ACTIVE 0xFF000000
#define COLOR_TITLE_TEXT_INACTIVE 0xFF8E8E93

/* macOS close button */
#define COLOR_CLOSE_BTN 0xFFFF5F57

/* ========================================================================= */
/* Text                                                                       */
/* ========================================================================= */

#define COLOR_FG 0xFF212121
#define COLOR_FG_SECONDARY 0xFF757575
#define COLOR_FG_DISABLED 0xFFBDBDBD

/* ========================================================================= */
/* Caret / selection                                                          */
/* ========================================================================= */

#define COLOR_CARET 0x40007AFF
#define COLOR_SELECTION 0x40007AFF
#define COLOR_SELECTION_ACTIVE 0xFF007AFF

/* ========================================================================= */
/* Borders                                                                    */
/* ========================================================================= */

#define COLOR_BORDER 0xFFD1D1D6
#define COLOR_BORDER_LIGHT 0xFFE5E5EA
#define COLOR_BORDER_DARK 0xFFC7C7CC

/* ========================================================================= */
/* Buttons                                                                    */
/* ========================================================================= */

#define COLOR_BUTTON_BG 0xFFFFFFFF
#define COLOR_BUTTON_HOVER 0xFFF5F5F5
#define COLOR_BUTTON_PRESSED 0xFFE0E0E0

#define COLOR_BUTTON_TEXT 0xFF000000
#define COLOR_BUTTON_DISABLED 0xFFFAFAFA

/* ========================================================================= */
/* Input fields                                                               */
/* ========================================================================= */

#define COLOR_INPUT_BG 0xFFFFFFFF
#define COLOR_INPUT_BORDER 0xFFD1D1D6
#define COLOR_INPUT_BORDER_ACTIVE 0xFF007AFF

/* ========================================================================= */
/* Menus */
/* ========================================================================= */

#define COLOR_MENU_BG 0xFFFFFFFF
#define COLOR_MENU_HOVER 0xFFF5F5F5
#define COLOR_MENU_SELECTED 0xFFE3F2FD

/* ========================================================================= */
/* Scrollbars */
/* ========================================================================= */

#define COLOR_SCROLL_TRACK 0xFFF2F2F7
#define COLOR_SCROLL_THUMB 0xFFC7C7CC
#define COLOR_SCROLL_THUMB_HOVER 0xFF8E8E93

/* ========================================================================= */
/* Tooltip */
/* ========================================================================= */

#define COLOR_TOOLTIP_BG 0xFF212121
#define COLOR_TOOLTIP_TEXT 0xFFFFFFFF

/* ========================================================================= */
/* Shadows */
/* ========================================================================= */

#define COLOR_SHADOW 0x20000000
#define COLOR_SHADOW_STRONG 0x40000000

/* ========================================================================= */
/* Status colors */
/* ========================================================================= */

#define COLOR_SUCCESS 0xFF34C759
#define COLOR_WARNING 0xFFFF9500
#define COLOR_ERROR 0xFFFF3B30
#define COLOR_INFO 0xFF007AFF

/* ========================================================================= */
/* Terminal colors */
/* ========================================================================= */

#define COLOR_TERM_BLACK 0xFF1C1C1E
#define COLOR_TERM_RED 0xFFFF3B30
#define COLOR_TERM_GREEN 0xFF34C759
#define COLOR_TERM_YELLOW 0xFFFFCC00
#define COLOR_TERM_BLUE 0xFF007AFF
#define COLOR_TERM_MAGENTA 0xFFAF52DE
#define COLOR_TERM_CYAN 0xFF5AC8FA
#define COLOR_TERM_WHITE 0xFFF2F2F7

#define COLOR_TERM_BRIGHT_BLACK 0xFF8E8E93
#define COLOR_TERM_BRIGHT_RED 0xFFFF6961
#define COLOR_TERM_BRIGHT_GREEN 0xFF30D158
#define COLOR_TERM_BRIGHT_YELLOW 0xFFFFD60A
#define COLOR_TERM_BRIGHT_BLUE 0xFF409CFF
#define COLOR_TERM_BRIGHT_MAGENTA 0xFFBF5AF2
#define COLOR_TERM_BRIGHT_CYAN 0xFF64D2FF
#define COLOR_TERM_BRIGHT_WHITE 0xFFFFFFFF

struct window {
  int id;
  int x, y;
  int width, height; /* LOGICAL surface size (the app's pixel buffer). */
  /* ON-SCREEN size the compositor draws the surface at (GFX-DYN-01 surface
   * model).  When draw_w/draw_h differ from width/height the compositor scales
   * the logical surface to the draw rect (the app may keep drawing at its old
   * size while it reallocates → fluid resize).  0 means "same as logical". */
  int draw_w, draw_h;
  int z_order;
  int visible;
  int pid;
  int protected;          /* If true, cannot be closed */
  int top_most;           /* If true, always on top and no decorations */
  int passive;            /* If true, click-through: never focused, never hit-tested
                             for input (system popups e.g. notifications). */
  uint32_t *buffer;       /* Window's pixel buffer */
  uint32_t bg_color;      /* Default background color */
  char title[64];
  int radius;
  int has_rounded_corners;

  /* Terminal State — VT/ANSI emulator extracted to term.c (#123). */
  struct terminal term;

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

/* Compositor backbuffer == the "compositor FB" / desktop-virtual surface
 * (GFX-DYN-01).  Its size follows the GPU, no longer a hard-coded 720x1280.
 * It is pre-allocated once to a CAPACITY that covers the current mode plus
 * reasonable runtime growth, so compositor_resize() never allocates and is
 * therefore safe to call from the timer-IRQ tick (host display-change path). */
#define COMPOSITOR_FALLBACK_W 1024
#define COMPOSITOR_FALLBACK_H 768
#define COMPOSITOR_MAX_W 1920
#define COMPOSITOR_MAX_H 1080

static uint32_t *compositor_backbuffer = NULL;
static int bb_width = 0;
static int bb_height = 0;
static size_t bb_capacity_px = 0; /* pixels actually allocated */

/*
 * Initialize Compositor
 */
void compositor_init(void) {
  memset(windows, 0, sizeof(windows));
  window_count = 0;
  next_window_id = 100;

  /* Backbuffer dimensions follow the primary GPU (no hard-coded size). */
  struct gpu_device *dev = gpu_get_primary();
  bb_width = dev ? dev->width : COMPOSITOR_FALLBACK_W;
  bb_height = dev ? dev->height : COMPOSITOR_FALLBACK_H;

  /* Allocate to a capacity covering the current mode and up to MAX_WxMAX_H so
   * runtime resizes (host display-change / set_mode) need no reallocation. */
  size_t cap = (size_t)bb_width * bb_height;
  size_t want = (size_t)COMPOSITOR_MAX_W * COMPOSITOR_MAX_H;
  if (want > cap)
    cap = want;
  int pages = (int)((cap * 4 + 4095) / 4096);
  compositor_backbuffer = pmm_alloc_pages(pages);
  if (!compositor_backbuffer) {
    pr_err("%s", "Compositor: Failed to allocate backbuffer!\n");
    bb_capacity_px = 0;
    bb_width = 0;
    bb_height = 0;
    return;
  }
  bb_capacity_px = cap;

  /* First frame is a full upload. */
  damage_x1 = 0;
  damage_y1 = 0;
  damage_x2 = bb_width;
  damage_y2 = bb_height;

  pr_info("Compositor: Initialized (%dx%d, capacity %d px)\n", bb_width,
          bb_height, (int)bb_capacity_px);
}

/* Report the current desktop (compositor backbuffer) size.  Backs
 * SYS_DISPLAY_INFO. */
void compositor_get_size(int *w, int *h) {
  if (w)
    *w = bb_width;
  if (h)
    *h = bb_height;
}

/*
 * compositor_resize - retarget the desktop/backbuffer to a new size.
 *
 * Capacity-bounded and allocation-free, so it is safe from any context
 * (including the timer IRQ on a host display-change).  The caller is
 * responsible for having already reconfigured the GPU scanout (gpu_set_mode)
 * to the same size so the next flush's strides match.  Existing windows are
 * clamped so their title bars stay reachable; the whole desktop is repainted.
 */
void compositor_resize(int w, int h) {
  if (w <= 0 || h <= 0)
    return;
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  if (!compositor_backbuffer || bb_capacity_px == 0) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return;
  }
  if ((size_t)w * h > bb_capacity_px) {
    /* Larger than the pre-allocation: clamp height to fit the buffer rather
     * than allocate in a possibly-IRQ context. */
    int max_h = (int)(bb_capacity_px / (size_t)w);
    if (max_h <= 0) {
      spin_unlock_irqrestore(&compositor_lock, flags);
      pr_warn("Compositor: resize %dx%d exceeds capacity, ignored\n", w, h);
      return;
    }
    pr_warn("Compositor: resize %dx%d clamped to %dx%d (capacity)\n", w, h, w,
            max_h);
    h = max_h;
  }

  bb_width = w;
  bb_height = h;

  /* Keep every window's title bar / close button on-screen. */
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == 0)
      continue;
    if (windows[i].x > bb_width - 40)
      windows[i].x = bb_width - 40;
    if (windows[i].x < 0)
      windows[i].x = 0;
    if (windows[i].y > bb_height - 20)
      windows[i].y = bb_height - 20;
    if (windows[i].y < TITLE_BAR_HEIGHT)
      windows[i].y = TITLE_BAR_HEIGHT;
  }

  /* Full repaint at the new size. */
  damage_x1 = 0;
  damage_y1 = 0;
  damage_x2 = bb_width;
  damage_y2 = bb_height;
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);

  pr_info("Compositor: resized to %dx%d\n", bb_width, bb_height);
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

  /*
   * FIX(GFX-COMP-NEWWIN-01): clamp the initial position so a newly created
   * window (including its title bar / close button) lands fully on-screen.
   * compositor_update_mouse() already enforces these same bounds while
   * dragging a window; without this check here, a window created near a
   * screen edge (or larger than the screen) would have its title bar and
   * close button off-screen until the user dragged it at least once.
   * Mirrors the clamp order used in compositor_update_mouse().
   */
  {
    struct gpu_device *screen_dev = gpu_get_primary();
    int screen_w = screen_dev ? screen_dev->width : 800;
    int screen_h = screen_dev ? screen_dev->height : 600;

    if (x + w > screen_w)
      x = screen_w - w;
    if (x < 0)
      x = 0;

    if (y + h > screen_h)
      y = screen_h - h;
    if (y < TITLE_BAR_HEIGHT)
      y = TITLE_BAR_HEIGHT;
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
  uint32_t default_bg = COLOR_WIN_BG;
  for (int i = 0; i < w * h; i++)
    buffer[i] = default_bg;

  /* Initialize window */
  windows[slot].id = next_window_id++;
  windows[slot].x = x;
  windows[slot].y = y;
  windows[slot].width = w;
  windows[slot].height = h;
  windows[slot].draw_w = w; /* on-screen == logical until a resize scales it */
  windows[slot].draw_h = h;
  windows[slot].z_order = window_count;
  windows[slot].visible = 1;
  windows[slot].pid = pid;
  windows[slot].buffer = buffer;
  windows[slot].bg_color = default_bg;

  /* Initialize the embedded terminal emulator (cell grid from font metrics). */
  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  if (term_init(&windows[slot].term, w / char_w, h / char_h, COLOR_FG,
                default_bg) != 0) {
    pr_err("%s", "Compositor: Failed to allocate terminal grids\n");
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

  /* Attiva il supporto alpha blending per default */
  windows[slot].has_alpha = 1;

  /* Clear buffer to background */
  for (int i = 0; i < w * h; i++) {
    buffer[i] = windows[slot].bg_color;
  }

  /* Mark main shell (PID 2) as protected */
  windows[slot].protected = (pid == 2) ? 1 : 0;
  windows[slot].top_most = 0;
  windows[slot].passive = 0;

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
    if (windows[i].id != 0 && windows[i].visible &&
        windows[i].z_order > max_z) {
      max_z = windows[i].z_order;
      pid = windows[i].pid;
    }
  }
  sched_set_focus_pid(pid); /* push the focus hint down (#67); never write the
                               scheduler's global directly */
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
        *cols = windows[i].term.cols;
      if (rows)
        *rows = windows[i].term.rows;
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
      term_free(&windows[i].term);
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
      term_free(&windows[i].term);
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

/* compositor_get_focus_pid() removed (DIR-02 / SCHED-01 #83): it was dead after
 * the scheduler→compositor inversion.  Focus is now read from the scheduler's
 * published keyboard_focus_pid hint, never queried back from the compositor. */

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
 * compositor_resize_window - resize a window's LOGICAL surface (GFX-DYN-01).
 *
 * Reallocates the window pixel buffer to w x h, reflows the embedded terminal
 * to the new cell grid, and resets the on-screen draw size to match (crisp,
 * non-scaled).  Allocation happens here, so this must be called from process
 * context (it backs SYS_WINDOW_RESIZE), never from an IRQ.  Returns 0 on
 * success, -1 on failure (the old surface is kept on failure).
 */
int compositor_resize_window(int window_id, int w, int h) {
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return -1;

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  struct window *win = NULL;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      win = &windows[i];
      break;
    }
  }
  if (!win) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }

  /* Allocate the new surface before touching the live one. */
  uint32_t *nbuf = (uint32_t *)kmalloc((size_t)w * h * 4);
  if (!nbuf) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }
  for (int p = 0; p < w * h; p++)
    nbuf[p] = win->bg_color;

  /* Damage the OLD on-screen footprint so the vacated area is repainted. */
  int old_dw = win->draw_w > 0 ? win->draw_w : win->width;
  int old_dh = win->draw_h > 0 ? win->draw_h : win->height;
  expand_damage(win->x, win->y - TITLE_BAR_HEIGHT, old_dw,
                old_dh + TITLE_BAR_HEIGHT);

  uint32_t *obuf = win->buffer;
  win->buffer = nbuf;
  win->width = w;
  win->height = h;
  win->draw_w = w; /* crisp: on-screen == new logical */
  win->draw_h = h;

  /* Reflow the terminal to the new cell grid. */
  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  if (char_w > 0 && char_h > 0)
    term_resize(&win->term, w / char_w, h / char_h);

  /* Damage the NEW footprint and repaint. */
  expand_damage(win->x, win->y - TITLE_BAR_HEIGHT, w, h + TITLE_BAR_HEIGHT);
  compositor_dirty = 1;
  int owner = win->pid;
  spin_unlock_irqrestore(&compositor_lock, flags);

  if (obuf)
    kfree(obuf);

  /* Notify the owner of its new logical size (outside the lock: kernel_ipc_send
   * takes sched_lock — never nest it under compositor_lock, cf. GFX-COMP-03). */
  if (owner > 0) {
    struct ipc_message msg = {0};
    msg.type = IPC_TYPE_RESIZE;
    msg.data1 = (uint64_t)w;
    msg.data2 = (uint64_t)h;
    kernel_ipc_send(owner, &msg);
  }
  return 0;
}

/*
 * The VT/ANSI terminal emulator (blend_pixel, SGR, CSI, caret, scroll) was
 * extracted to kernel/graphics/term.c (GFX-DYN-01, #123).  Pixel blending now
 * uses gl_blend_pixel() from <graphics/gl.h>.
 */

/* Erase carets on every window not owned by keep_pid; marks damage so they
 * repaint.  Caller holds compositor_lock.  Used on focus changes so the caret
 * follows the input window instead of lingering on the one that lost focus. */
static void __clear_other_carets_locked(int keep_pid) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    struct window *win = &windows[i];
    if (win->id == 0 || !win->term.caret_shown || win->pid == keep_pid)
      continue;
    struct gl_surface s = {.width = win->width,
                           .height = win->height,
                           .stride = win->width,
                           .buffer = win->buffer};
    term_clear_caret(&win->term, &s);
    win->term.focused = 0;
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
 * Write text to a window's terminal emulator.  Thin compositor seam over the
 * extracted VT engine (term.c, #123): look up the window, hand the terminal the
 * window's content surface, mark damage.  ANSI parsing, caret, scroll and SGR
 * all live in term_write().
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

  struct gl_surface win_surf = {.width = win->width,
                                .height = win->height,
                                .stride = win->width,
                                .buffer = win->buffer};

  /* The caret is drawn only on the window that currently owns keyboard input. */
  win->term.focused = (win->pid == keyboard_focus_pid);
  term_write(&win->term, &win_surf, buf, count);

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
    /* Passive windows (system notifications) are click-through: never hit-tested,
     * so a click on the popup passes to whatever is beneath it and the popup
     * neither steals focus/caret nor receives an IPC_TYPE_MOUSE event. */
    if (windows[i].id != 0 && windows[i].visible && !windows[i].passive) {
      int dw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
      int dh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
      int title_top = windows[i].y - TITLE_BAR_HEIGHT;
      if (mouse_x >= windows[i].x && mouse_x < windows[i].x + dw &&
          mouse_y >= title_top && mouse_y < windows[i].y + dh) {
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

  /* Update keyboard focus to this process — push the hint down (#67). */
  if (keyboard_focus_pid != hit->pid) {
    pr_info("Compositor: Focus changed to PID %d (Window '%s')\n", hit->pid,
            hit->title);
    sched_set_focus_pid(hit->pid);
  }

  /*
   * FIX(GFX-COMP-03): never call kernel_ipc_send() or process_terminate() while
   * holding compositor_lock.  compositor_handle_click runs in mouse-IRQ
   * context; kernel_ipc_send() takes sched_lock, and process_terminate() takes
   * sched_lock then re-enters the compositor (compositor_destroy_windows_by_pid
   * -> compositor_lock).  Holding compositor_lock across either is the reverse
   * of process_terminate's own sched_lock->compositor_lock order — an SMP AB-BA
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
    /* Store relative coordinates in payload, mapped from the on-screen draw
     * rect back to the app's LOGICAL surface so the app sees its own coords. */
    int hdw = hit->draw_w > 0 ? hit->draw_w : hit->width;
    int hdh = hit->draw_h > 0 ? hit->draw_h : hit->height;
    int rel_x = mouse_x - hit->x;
    int rel_y = mouse_y - hit->y;
    if (hdw > 0 && hdw != hit->width)
      rel_x = (int)((int64_t)rel_x * hit->width / hdw);
    if (hdh > 0 && hdh != hit->height)
      rel_y = (int)((int64_t)rel_y * hit->height / hdh);
    memcpy(msg.payload, &rel_x, 4);
    memcpy(msg.payload + 4, &rel_y, 4);
    send_pid = keyboard_focus_pid;
  }

  /* Capture a close-button hit; the terminate is deferred until after unlock.
   */
  int do_close = 0;
  int close_pid = 0;
  if (!hit->protected) {
    int hdw = hit->draw_w > 0 ? hit->draw_w : hit->width;
    int btn_x = hit->x + hdw - CLOSE_BUTTON_SIZE - 2;
    int btn_y = hit->y - TITLE_BAR_HEIGHT + 2;
    if (mouse_x >= btn_x && mouse_x < btn_x + CLOSE_BUTTON_SIZE &&
        mouse_y >= btn_y && mouse_y < btn_y + CLOSE_BUTTON_SIZE) {
      do_close = 1;
      close_pid = hit->pid;
    }
  }

  /* Check for drag start (skipped when closing, matching the old early-return).
   */
  if (!do_close && mouse_y >= hit->y - TITLE_BAR_HEIGHT && mouse_y < hit->y) {
    dragging_window_id = hit->id;
    drag_off_x = mouse_x - hit->x;
    drag_off_y = mouse_y - hit->y;
  }

  expand_damage(0, 0, bb_width, bb_height);
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);

  /*
   * Cross-subsystem calls, now strictly OUTSIDE compositor_lock
   * (FIX(GFX-COMP-03)). Both validate their target pid internally, so a
   * window/process that changed between the unlock and here is handled
   * gracefully. Input delivery uses the same kernel_ipc_send transport the
   * keyboard driver uses. Window close goes through the process-layer intent
   * seam window_request_close() (#69) — the compositor no longer references
   * process_terminate, so graphics does not drive process lifecycle directly.
   * NOTE: the close still force-terminates in mouse-IRQ context; deferring it to
   * a safe context is the separate SCHED-03 follow-up, now localised behind the
   * seam. */
  if (send_pid > 0)
    kernel_ipc_send(send_pid, &msg);
  if (do_close) {
    pr_info("Compositor: Close button -> request close of PID %d\n", close_pid);
    window_request_close(close_pid);
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
    /* Absolute pointer: events carry one axis at a time, so a negative
     * component means "leave this axis unchanged". Values are normalized to [0,
     * INPUT_ABS_MAX]; scale to framebuffer pixels. This is what makes the
     * cursor track 1:1 under absolute hosts like UTM (DRV-INPUT-01 #125),
     * instead of a relative device saturating at a screen edge. */
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
        int dw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
        int dh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
        windows[i].x = mouse_x - drag_off_x;
        windows[i].y = mouse_y - drag_off_y;
        /* Enforce screen boundaries (use on-screen draw size) */
        if (windows[i].x < 0)
          windows[i].x = 0;
        if (windows[i].x + dw > width)
          windows[i].x = width - dw;
        if (windows[i].y < TITLE_BAR_HEIGHT)
          windows[i].y = TITLE_BAR_HEIGHT;
        if (windows[i].y + dh > height)
          windows[i].y = height - dh;
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
      int dw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
      int dh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
      /* Skip off-screen (use on-screen draw size) */
      if (windows[i].x >= bb_w || windows[i].y >= bb_h ||
          windows[i].x + dw <= 0 || windows[i].y + dh <= 0)
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

    /* On-screen draw size (== logical unless the surface is being scaled). */
    int dw = win->draw_w > 0 ? win->draw_w : win->width;
    int dh = win->draw_h > 0 ? win->draw_h : win->height;

    /* Calculate Full Window Bounds (Content + Decorations) */
    int win_y = win->top_most ? win->y : win->y - TITLE_BAR_HEIGHT;
    int win_h = win->top_most ? dh : dh + TITLE_BAR_HEIGHT;

    struct region *vis = region_create();
    if (vis) {
      region_add_rect(vis, win->x, win_y, dw, win_h);

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
      region_add_rect(occluded, win->x, win_y, dw, win_h);
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

    /* Draw Background — gradiente verticale macOS-style, calcolato una volta
     * per riga (COLOR_BG_TOP -> COLOR_BG_BOTTOM) invece che per pixel. */
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
  occluded = NULL; /* prevent double-free: cleanup at end of function also calls
                      region_destroy(occluded) */

  /* Pass 2: Rendering (Bottom-Up) - Painter's Algorithm with Clipping */
  for (int i = 0; i < count && i < MAX_WINDOWS; i++) {
    struct window *win = sorted[i];
    struct region *vis = visible_regions[i];

    /* On-screen draw size + whether the logical surface needs scaling. */
    int dw = win->draw_w > 0 ? win->draw_w : win->width;
    int dh = win->draw_h > 0 ? win->draw_h : win->height;
    int scaled = (dw != win->width || dh != win->height);

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
                /* In Title Bar — macOS-style: la finestra a fuoco ha una
                 * title bar piu' chiara, le altre restano piu' scure. */
                uint32_t title_color = (win->pid == keyboard_focus_pid)
                                           ? COLOR_TITLE_ACTIVE
                                           : COLOR_TITLE_INACTIVE;

                /* Close button: cerchio pieno rosso (stile traffic light
                 * macOS), disegnato solo se la finestra non e' protetta —
                 * coerente con compositor_handle_click che ignora il
                 * bottone su hit->protected. */
                if (!win->protected) {
                  int btn_cx = win->x + dw - 2 - CLOSE_BUTTON_SIZE / 2;
                  int btn_cy = decor_y + 2 + CLOSE_BUTTON_SIZE / 2;
                  int ddx = screen_x - btn_cx;
                  int ddy = screen_y - btn_cy;
                  int radius = CLOSE_BUTTON_SIZE / 2 - 1;
                  if (ddx * ddx + ddy * ddy <= radius * radius) {
                    title_color = COLOR_CLOSE_BTN;
                  }
                }

                backbuffer[screen_idx] = title_color;
              }
            } else {
              /* Content Area.  Position within the on-screen draw rect... */
              int draw_x = screen_x - win->x;
              int draw_y = screen_y - win->y;

              if (draw_x >= 0 && draw_x < dw && draw_y >= 0 && draw_y < dh) {
                /* ...mapped back to the logical surface (nearest-sample scale
                 * when draw size != logical size — GFX-DYN-01 surface model). */
                int sx = scaled ? (int)((int64_t)draw_x * win->width / dw)
                                : draw_x;
                int sy = scaled ? (int)((int64_t)draw_y * win->height / dh)
                                : draw_y;
                if (sx < 0)
                  sx = 0;
                else if (sx >= win->width)
                  sx = win->width - 1;
                if (sy < 0)
                  sy = 0;
                else if (sy >= win->height)
                  sy = win->height - 1;

                if (win->buffer) {
                  uint32_t pixel = win->buffer[sy * win->width + sx];
                  backbuffer[screen_idx] =
                      gl_blend_pixel(pixel, backbuffer[screen_idx]);
                } else {
                  backbuffer[screen_idx] =
                      gl_blend_pixel(win->bg_color, backbuffer[screen_idx]);
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
      int start_x = win->x + (dw - text_w) / 2;
      int start_y =
          decor_y + (20 - char_h) / 2; /* Center vertically in title bar */

      /* macOS-style: il titolo della finestra a fuoco e' piu' luminoso,
       * quello delle finestre inattive e' attenuato (systemGray). */
      uint32_t text_color = (win->pid == keyboard_focus_pid)
                                ? COLOR_TITLE_TEXT_ACTIVE
                                : COLOR_TITLE_TEXT_INACTIVE;

      gl_draw_string(&screen, start_x, start_y, win->title, text_color);
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
      windows[i].passive = (flags_val & 8) ? 1 : 0; /* bit 3: click-through */
      if (flags_val & 4)
        windows[i].visible = 0; /* bit 2: hide window */
      else if (flags_val & 2)
        windows[i].visible = 1; /* bit 1: show window */
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
}