/*
 * kernel/graphics/compositor.c
 * Window Compositor
 *
 * Manages windows and composites them to the screen.
 */
#include <drivers/gpu/gpu.h>
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
  uint8_t *text_grid;  /* Character grid */
  uint32_t *attr_grid; /* Attribute grid (colors) */
  int grid_cols, grid_rows;
  uint32_t fg_color;
  int escape_state;
  char escape_buf[32];
  int escape_len;
};

/* Global State */
static struct window windows[MAX_WINDOWS];
static int window_count = 0;
static int next_window_id = 100;
static volatile int compositor_dirty = 1;
static DEFINE_SPINLOCK(compositor_lock);

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

  /* Initialize text grids */
  int char_w = 8;
  int char_h = 16;
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
  windows[slot].fg_color = 0xFFFFFFFF;
  windows[slot].escape_state = 0;
  windows[slot].escape_len = 0;

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
 * Destroy Window
 */
void compositor_destroy_window(int window_id) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      if (windows[i].buffer) {
        kfree(windows[i].buffer);
      }
      if (windows[i].text_grid)
        kfree(windows[i].text_grid);
      if (windows[i].attr_grid)
        kfree(windows[i].attr_grid);
      memset(&windows[i], 0, sizeof(struct window));
      window_count--;
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
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].pid == pid) {
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

  int char_w = 8;
  int char_h = 16;
  int cols = win->width / char_w;
  int rows = win->height / char_h;

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
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        if (c == 'm') {
          handle_sgr(win);
        } else if (c == 'J') {
          for (int p = 0; p < win->width * win->height; p++)
            win->buffer[p] = win->curr_bg_color;

          /* Also clear text grids */
          if (win->text_grid && win->attr_grid) {
            memset(win->text_grid, ' ', win->grid_cols * win->grid_rows);
            for (int p = 0; p < win->grid_cols * win->grid_rows; p++)
              win->attr_grid[p] = win->curr_bg_color;
          }
          win->cursor_x = 0;
          win->cursor_y = 0;
        }
        win->escape_state = 0;
      } else if (win->escape_len < 31) {
        win->escape_buf[win->escape_len++] = c;
      } else {
        win->escape_state = 0;
      }
    }
  }

  /* Mark compositor as needing redraw */
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
    /* Release - Stop dragging */
    dragging_window_id = -1;
    return;
  }

  if (state != 1) /* Only handle press down */
    return;

  /* Debug click */
  /* pr_info("Click: %d, %d\n", mouse_x, mouse_y); */

  /* Check for window hit - include title bar area (y - TITLE_BAR_HEIGHT to y +
   * height) */
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

  if (hit) {
    /* Bring to front */
    int top_z = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
      if (windows[i].id != 0 && windows[i].z_order > top_z)
        top_z = windows[i].z_order;
    }
    hit->z_order = top_z + 1;

    /* Check for close button click */
    if (!hit->protected) {
      int btn_x = hit->x + hit->width - CLOSE_BUTTON_SIZE - 2;
      int btn_y = hit->y - TITLE_BAR_HEIGHT + 2;
      if (mouse_x >= btn_x && mouse_x < btn_x + CLOSE_BUTTON_SIZE &&
          mouse_y >= btn_y && mouse_y < btn_y + CLOSE_BUTTON_SIZE) {
        pr_info("Compositor: Close button clicked on window %d (PID %d)\n",
                hit->id, hit->pid);
        /* Terminate the owning process */
        extern int process_terminate(int pid);
        process_terminate(hit->pid);
        /* Destroy the window */
        compositor_destroy_window(hit->id);
        compositor_dirty = 1;
        compositor_render();
        return;
      }
    }

    /* Check for drag start (Title bar area) */
    if (mouse_y >= hit->y - TITLE_BAR_HEIGHT && mouse_y < hit->y) {
      dragging_window_id = hit->id;
      drag_off_x = mouse_x - hit->x;
      drag_off_y = mouse_y - hit->y;
    }

    compositor_render();
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

  if (absolute) {
    mouse_x = dx;
    mouse_y = dy;
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

static int in_render = 0;
static void compositor_render_internal(void) {
  if (in_render)
    return;
  in_render = 1;

  struct gpu_device *dev = gpu_get_primary();
  if (!dev || !compositor_backbuffer) {
    in_render = 0;
    return;
  }

  /* Use current buffer dimensions */
  int bb_w = bb_width;
  int bb_h = bb_height;
  uint32_t *backbuffer = compositor_backbuffer;

  /* Wrap backbuffer in GL Surface */
  struct gl_surface screen = {
      .width = bb_w, .height = bb_h, .stride = bb_w, .buffer = backbuffer};

  /* Sort Windows by Z-Order (Bottom to Top) */
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
  /* Top Most handling: move all top-most windows to the end of the sorted list */
  int current_count = count;
  for (int i = 0; i < current_count; i++) {
    if (sorted[i]->top_most) {
      struct window *tmp = sorted[i];
      /* Shift remaining windows left */
      for (int k = i; k < current_count - 1; k++) {
        sorted[k] = sorted[k + 1];
      }
      sorted[current_count - 1] = tmp;
      /* Decrement current_count so we don't re-process the window we just moved */
      current_count--;
      /* Decrement i to process the window that was shifted into the current slot */
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
    return;
  }

  /* Iterate Top-to-Bottom for Occlusion */
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

    /* Add Self to Occluded (Assuming Opaque) */
    region_add_rect(occluded, win->x, win_y, win->width, win_h);
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
          /* Proper Gradient Background */
          uint32_t r_chk = 20;
          uint32_t g_chk = 40 + (sy * 40 / bb_h);
          uint32_t b_chk = 80 + (sy * 80 / bb_h);
          backbuffer[sy * bb_w + sx] =
              0xFF000000 | (r_chk << 16) | (g_chk << 8) | b_chk;
        }
      }
    }
    region_destroy(bg_region);
  }
  region_destroy(occluded);

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

      int char_w = 8;
      int text_w = title_len * char_w;
      int start_x = win->x + (win->width - text_w) / 2;
      int start_y = decor_y + 2;

      for (int c = 0; c < title_len; c++) {
        if (start_x + c * char_w + char_w > 0 && start_x + c * char_w < bb_w) {
          gl_draw_char(&screen, start_x + c * char_w, start_y, win->title[c],
                       0xFFFFFFFF);
        }
      }
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
  /* ... (Existing mouse code) ... */
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

  /* Flush */
  if (dev->ops && dev->ops->flush && dev->ops->get_framebuffer) {
    void *fb_va = dev->ops->get_framebuffer(dev, NULL);
    if (fb_va) {
      memcpy(fb_va, backbuffer, bb_w * bb_h * 4);
      dev->ops->flush(dev, 0, 0, bb_w, bb_h);
    }
  }

  in_render = 0;
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

        /* Use memcpy */
        void *dst_ptr = &windows[i].buffer[py * windows[i].width + dest_x];
        const void *src_ptr = &user_buf[dy * w + src_x];

        memcpy(dst_ptr, src_ptr, copy_w * sizeof(uint32_t));
      }
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
