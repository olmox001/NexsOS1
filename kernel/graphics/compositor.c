/*
 * kernel/graphics/compositor.c
 * Window Compositor – Material Design 3 Convergent UI
 *
 * Manages windows and composites them to the screen.
 *
 * UI changes (logic untouched):
 *   - ui_profile_t: desktop/mobile policy struct with percentage-based sizes.
 *   - TITLE_BAR_HEIGHT / CLOSE_BUTTON_SIZE: now computed from GPU resolution
 *     at init time (6 % screen_h and 55 % title-bar-height respectively).
 *   - WIN_CORNER_RADIUS: derived from corner_radius_pct × base_unit.
 *   - Background gradient: MD3 light theme (COLOR_BG_TOP → COLOR_BG_BOTTOM)
 *     instead of the previous dark-blue gradient.
 *   - Window shadow: three-layer darkening pass rendered before each window.
 *   - Titlebar: 1-px MD3 separator at bottom; rounded top corners; outer ring
 *     on the close button.
 *   - Content area: rounded bottom corners (pixels outside the radius are
 *     skipped so the background shows through).
 */
#include <drivers/gpu/gpu.h>
#include <drivers/virtio_input.h>
#include <graphics/gl.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/graphics.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <stdint.h>

#define MAX_WINDOWS 32

/* =========================================================================
 * Material Design 3 – Convergent UI Profile
 * =========================================================================
 *
 * ui_profile_t describes the active desktop/mobile rendering policy.
 * All visual measurements are expressed as percentages of base_unit
 * (= min(screen_w, screen_h) / 100).  The compositor selects one profile
 * at init time based on the GPU resolution; applications run unchanged on
 * both form factors.
 *
 * Values follow the specification in the technical document:
 *   Desktop: scale 1.0×, 2 % padding, 2 % corner radius, 300 ms animation.
 *   Mobile : scale 1.75×, 4 % padding, 4 % corner radius, 400 ms animation.
 */
typedef struct {
  /* Window & Layout */
  int floating_windows; /* 1 = overlapping windows allowed          */
  int resize_enabled;
  int titlebars;
  int maximize_button;
  int minimize_button;
  int close_button;
  int max_visible_windows;

  /* Input */
  int hover_effects;
  int gestures_enabled;
  int keyboard_button;
  int app_scroll_buttons;
  int touch_target_min_pct; /* % of base_unit; always ≥ 11              */

  /* Visual & Spacing */
  int scale_factor_pct;  /* 100 = 1.0×, 175 = 1.75×                 */
  int padding_base_pct;  /* % of base_unit                           */
  int corner_radius_pct; /* % of base_unit                           */
  int elevation_pct;     /* shadow depth, % of base_unit             */

  /* Navigation & Launcher */
  int bottom_bar_permanent;
  int launcher_fullscreen;
  int launcher_columns;

  /* Motion & Theme */
  int animation_duration_ms;
  int reduce_motion;
  int blur_enabled;
  int blur_radius_pct;
  int use_dynamic_color;
  int shadows_enabled; /* 1 = ombre attive, 0 = disattivate */
} ui_profile_t;

/* Desktop profile – floating windows, hover effects, persistent dock --------
 */
static const ui_profile_t ui_desktop = {
    .floating_windows = 1,
    .resize_enabled = 1,
    .titlebars = 1,
    .maximize_button = 1,
    .minimize_button = 1,
    .close_button = 1,
    .max_visible_windows = 99,
    .hover_effects = 1,
    .gestures_enabled = 0,
    .keyboard_button = 0,
    .touch_target_min_pct = 11,
    .scale_factor_pct = 100,
    .padding_base_pct = 2,
    .corner_radius_pct = 2, /* 2 % base_unit ≈ 8-10 px at 1080 p     */
    .elevation_pct = 1,     /* subtle drop shadow                     */
    .bottom_bar_permanent = 1,
    .launcher_fullscreen = 0,
    .launcher_columns = 6,
    .animation_duration_ms = 300,
    .reduce_motion = 0,
    .blur_enabled = 1,
    .blur_radius_pct = 2,
    .use_dynamic_color = 1,
};

/* Mobile profile – single-window, gestures, fullscreen launcher -------------
 */
static const ui_profile_t ui_mobile = {
    .floating_windows = 0,
    .resize_enabled = 0,
    .titlebars = 0,
    .maximize_button = 0,
    .minimize_button = 0,
    .close_button = 0,
    .max_visible_windows = 1,
    .hover_effects = 0,
    .gestures_enabled = 1,
    .keyboard_button = 1,
    .touch_target_min_pct = 12,
    .scale_factor_pct = 175,
    .padding_base_pct = 4,
    .corner_radius_pct = 4, /* 4 % base_unit                          */
    .elevation_pct = 2,
    .bottom_bar_permanent = 1,
    .launcher_fullscreen = 1,
    .launcher_columns = 4,
    .animation_duration_ms = 400,
    .reduce_motion = 0,
    .blur_enabled = 1,
    .blur_radius_pct = 3,
    .use_dynamic_color = 1,
};

/* Active profile – set in compositor_init() based on GPU resolution ---------
 */
static const ui_profile_t *ui_profile = &ui_desktop;

/*
 * compositor_base_unit - 1 % of the smaller screen dimension in pixels.
 *
 * base_unit% = min(screen_w, screen_h) / 100.0f   (spec §2)
 * All percentage-based sizes are computed as  N × compositor_base_unit().
 */
static inline int compositor_base_unit(void) {
  struct gpu_device *dev = gpu_get_primary();
  if (!dev)
    return 8;
  int m = dev->width < dev->height ? dev->width : dev->height;
  int bu = m / 100;
  return (bu < 1) ? 1 : bu;
}

/* =========================================================================
 * Desktop background                                                       */
/* MD3 light surface gradient: systemBackground (iOS) → Blue-grey 50 -------- */
#define COLOR_BG_TOP 0xFFF2F2F7
#define COLOR_BG_BOTTOM 0xFFECEFF1

/* =========================================================================
 * Window                                                                   */
#define COLOR_WIN_BG 0xFFFCFCFD

/* =========================================================================
 * Title bar                                                                */
#define COLOR_TITLE_ACTIVE 0xFFEFEFF4
#define COLOR_TITLE_INACTIVE 0xFFE5E5EA
/* 1-px MD3 divider between title bar and window content */
#define COLOR_TITLE_SEPARATOR 0xFFD1D1D6

#define COLOR_TITLE_TEXT_ACTIVE 0xFF000000
#define COLOR_TITLE_TEXT_INACTIVE 0xFF8E8E93

/* macOS-style traffic-light close button */
#define COLOR_CLOSE_BTN 0xFFFF5F57
#define COLOR_CLOSE_BTN_RING 0xFFE0443E /* darker outer ring               */

/* =========================================================================
 * Text                                                                     */
#define COLOR_FG 0xFF212121
#define COLOR_FG_SECONDARY 0xFF757575
#define COLOR_FG_DISABLED 0xFFBDBDBD

/* =========================================================================
 * Caret / selection                                                        */
#define COLOR_CARET 0x40007AFF
#define COLOR_SELECTION 0x40007AFF
#define COLOR_SELECTION_ACTIVE 0xFF007AFF

/* =========================================================================
 * Borders                                                                  */
#define COLOR_BORDER 0xFFD1D1D6
#define COLOR_BORDER_LIGHT 0xFFE5E5EA
#define COLOR_BORDER_DARK 0xFFC7C7CC

/* =========================================================================
 * Buttons                                                                  */
#define COLOR_BUTTON_BG 0xFFFFFFFF
#define COLOR_BUTTON_HOVER 0xFFF5F5F5
#define COLOR_BUTTON_PRESSED 0xFFE0E0E0
#define COLOR_BUTTON_TEXT 0xFF000000
#define COLOR_BUTTON_DISABLED 0xFFFAFAFA

/* =========================================================================
 * Input fields                                                             */
#define COLOR_INPUT_BG 0xFFFFFFFF
#define COLOR_INPUT_BORDER 0xFFD1D1D6
#define COLOR_INPUT_BORDER_ACTIVE 0xFF007AFF

/* =========================================================================
 * Menus                                                                    */
#define COLOR_MENU_BG 0xFFFFFFFF
#define COLOR_MENU_HOVER 0xFFF5F5F5
#define COLOR_MENU_SELECTED 0xFFE3F2FD

/* =========================================================================
 * Scrollbars                                                               */
#define COLOR_SCROLL_TRACK 0xFFF2F2F7
#define COLOR_SCROLL_THUMB 0xFFC7C7CC
#define COLOR_SCROLL_THUMB_HOVER 0xFF8E8E93

/* =========================================================================
 * Tooltip                                                                  */
#define COLOR_TOOLTIP_BG 0xFF212121
#define COLOR_TOOLTIP_TEXT 0xFFFFFFFF

/* =========================================================================
 * Shadows                                                                  */
#define COLOR_SHADOW 0x20000000
#define COLOR_SHADOW_STRONG 0x40000000

/* =========================================================================
 * Status colors                                                            */
#define COLOR_SUCCESS 0xFF34C759
#define COLOR_WARNING 0xFFFF9500
#define COLOR_ERROR 0xFFFF3B30
#define COLOR_INFO 0xFF007AFF

/* =========================================================================
 * Terminal colors                                                          */
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
  int radius;
  int has_rounded_corners;

  /* Terminal State */
  int cursor_x, cursor_y;
  int cursor_visible;     /* VT100 DECTCEM (\x1b[?25h/l); 1 = draw the caret */
  int caret_px, caret_py; /* cell where the caret was last painted */
  int caret_shown;        /* 1 = a caret is currently baked at caret_px/py */
  uint8_t *text_grid;     /* Character grid */
  uint32_t *attr_grid;    /* Attribute grid (colors) */
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

/* Pre-allocated buffers for rendering */
static struct window *sorted_windows[MAX_WINDOWS];
static struct region *visible_regions_store[MAX_WINDOWS];

/* Mouse State */
static int mouse_x = 400;
static int mouse_y = 300;

/* Dragging State */
static int dragging_window_id = -1;
static int drag_off_x = 0;
static int drag_off_y = 0;

/*
 * UI Dimensions – computed from GPU resolution in compositor_init().
 *
 *   TITLE_BAR_HEIGHT  = screen_h × 6 %, clamped [24, 40]   (spec §4)
 *   CLOSE_BUTTON_SIZE = TITLE_BAR_HEIGHT × 55 %, clamped [12, 22]
 *   WIN_CORNER_RADIUS = corner_radius_pct × base_unit, clamped [4, 16]
 *
 * Declared as plain ints (not #defines) so they can be set at runtime from
 * the actual framebuffer dimensions rather than a compile-time guess.
 */
static int TITLE_BAR_HEIGHT = 28;
static int CLOSE_BUTTON_SIZE = 14;
static int WIN_CORNER_RADIUS = 8;

/* Global backbuffer – pre-allocated; size is set from GPU in compositor_init */
static uint32_t *compositor_backbuffer = NULL;
static int bb_width = 0;
static int bb_height = 0;

/* =========================================================================
 * Initialize Compositor
 * ========================================================================= */
void compositor_init(void) {
  memset(windows, 0, sizeof(windows));
  window_count = 0;
  next_window_id = 100;

  /* ------------------------------------------------------------------
   * Derive percentage-based UI dimensions from the actual GPU resolution.
   * Falls back to 1280 × 720 when no GPU is detected yet.
   * ------------------------------------------------------------------ */
  {
    struct gpu_device *dev = gpu_get_primary();
    int sw = dev ? dev->width : 1280;
    int sh = dev ? dev->height : 720;

    /* Profile selection: ≤ 600 px wide → mobile, otherwise desktop */
    ui_profile = (sw <= 600) ? &ui_mobile : &ui_desktop;

    /* base_unit = 1 % of the smaller screen axis (spec §2) */
    int bu = (sw < sh ? sw : sh) / 100;
    if (bu < 1)
      bu = 1;

    /* Title bar: 6 % of screen height, clamped [24, 40] */
    TITLE_BAR_HEIGHT = (sh * 6) / 100;
    if (TITLE_BAR_HEIGHT < 24)
      TITLE_BAR_HEIGHT = 24;
    if (TITLE_BAR_HEIGHT > 40)
      TITLE_BAR_HEIGHT = 40;

    /* Close button: 55 % of title bar height, clamped [12, 22] */
    CLOSE_BUTTON_SIZE = (TITLE_BAR_HEIGHT * 60) / 100;
    if (CLOSE_BUTTON_SIZE < 12)
      CLOSE_BUTTON_SIZE = 12;
    if (CLOSE_BUTTON_SIZE > 22)
      CLOSE_BUTTON_SIZE = 22;

    /* Corner radius: corner_radius_pct × base_unit, clamped [4, 16] */
    WIN_CORNER_RADIUS = (ui_profile->corner_radius_pct * bu);
    if (WIN_CORNER_RADIUS < 4)
      WIN_CORNER_RADIUS = 4;
    if (WIN_CORNER_RADIUS > 16)
      WIN_CORNER_RADIUS = 16;

    bb_width = sw;
    bb_height = sh;
  }

  compositor_backbuffer = kmalloc(bb_width * bb_height * 4);

  /* Initialise damage rect to full screen so the first frame is fully uploaded
   */
  damage_x1 = 0;
  damage_y1 = 0;
  damage_x2 = bb_width;
  damage_y2 = bb_height;

  if (!compositor_backbuffer) {
    pr_err("%s", "Compositor: Failed to allocate backbuffer!\n");
  }

  pr_info(
      "Compositor: Initialized (%dx%d profile=%s titlebar=%dpx corner=%dpx)\n",
      bb_width, bb_height, (ui_profile == &ui_desktop) ? "desktop" : "mobile",
      TITLE_BAR_HEIGHT, WIN_CORNER_RADIUS);
}

/* Forward Declarations */
static void compositor_render_internal(void);
static void draw_rect_internal(int window_id, int x, int y, int w, int h,
                               uint32_t color, int caller_pid);

/* =========================================================================
 * Create Window
 * ========================================================================= */
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
  uint32_t default_bg = COLOR_WIN_BG;
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
      windows[slot].attr_grid[i] = COLOR_FG;
  } else {
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
  windows[slot].fg_color = COLOR_FG;
  windows[slot].escape_state = 0;
  windows[slot].escape_len = 0;

  windows[slot].has_alpha = 1;

  /* Clear buffer to background */
  for (int i = 0; i < w * h; i++)
    buffer[i] = windows[slot].bg_color;

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
 */
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
  keyboard_focus_pid = pid;
  __clear_other_carets_locked(pid);
  pr_debug("Compositor: Focus reset to PID %d\n", pid);
}

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

/* =========================================================================
 * Destroy Window
 * ========================================================================= */
void compositor_destroy_window(int window_id) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      int refocus = (windows[i].pid == keyboard_focus_pid);
      if (windows[i].buffer)
        kfree(windows[i].buffer);
      if (windows[i].text_grid)
        kfree(windows[i].text_grid);
      if (windows[i].attr_grid)
        kfree(windows[i].attr_grid);
      memset(&windows[i], 0, sizeof(struct window));
      window_count--;
      if (refocus)
        __focus_topmost_locked();
      spin_unlock_irqrestore(&compositor_lock, flags);
      return;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
}

void compositor_destroy_windows_by_pid(int pid) {
  uint64_t flags;
  int refocus = 0;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].pid == pid) {
      if (windows[i].pid == keyboard_focus_pid)
        refocus = 1;
      if (windows[i].buffer)
        kfree(windows[i].buffer);
      if (windows[i].text_grid)
        kfree(windows[i].text_grid);
      if (windows[i].attr_grid)
        kfree(windows[i].attr_grid);
      memset(&windows[i], 0, sizeof(struct window));
      window_count--;
    }
  }
  if (refocus)
    __focus_topmost_locked();
  spin_unlock_irqrestore(&compositor_lock, flags);
}

uint32_t *compositor_get_buffer(int window_id) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id)
      return windows[i].buffer;
  }
  return NULL;
}

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

int compositor_get_focus_pid(void) {
  uint64_t flags;
  if (!spin_trylock_irqsave(&compositor_lock, &flags))
    return -1;
  int max_z = -1, pid = -1;
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

void compositor_move_window(int window_id, int x, int y) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      windows[i].x = x;
      windows[i].y = y;
      return;
    }
  }
}

/* =========================================================================
 * Alpha blend two colors
 * ========================================================================= */
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

/* =========================================================================
 * Process ANSI SGR (Select Graphic Rendition) parameters
 * ========================================================================= */
static void handle_sgr(struct window *win) {
  if (win->escape_len == 0) {
    win->fg_color = COLOR_FG;
    return;
  }
  int val = 0;
  for (int i = 0; i < win->escape_len; i++) {
    if (win->escape_buf[i] >= '0' && win->escape_buf[i] <= '9')
      val = val * 10 + (win->escape_buf[i] - '0');
  }
  if (val == 0) {
    win->fg_color = COLOR_FG;
    win->curr_bg_color = win->bg_color;
  } else if (val == 39) {
    win->fg_color = COLOR_FG;
  } else if (val == 49) {
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

static int csi_params(const char *buf, int len, int *a, int *b) {
  int vals[2] = {0, 0};
  int which = 0, have = 0;
  for (int i = 0; i < len; i++) {
    char c = buf[i];
    if (c >= '0' && c <= '9') {
      vals[which] = vals[which] * 10 + (c - '0');
      have = 1;
    } else if (c == ';' && which < 1)
      which++;
  }
  *a = vals[0];
  *b = vals[1];
  return have ? which + 1 : 0;
}

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

static void term_draw_cursor(int win_id, struct window *win,
                             struct gl_surface *surf, int char_w, int char_h) {
  extern int keyboard_focus_pid;
  term_erase_caret(win_id, win, surf, char_w, char_h);
  if (!win->cursor_visible || win->pid != keyboard_focus_pid)
    return;
  int cx = win->cursor_x, cy = win->cursor_y;
  if (cx < 0 || cy < 0 || cx >= win->grid_cols || cy >= win->grid_rows)
    return;
  const uint32_t caret = COLOR_CARET;
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

void compositor_focus_changed(int new_pid) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  __clear_other_carets_locked(new_pid);
  spin_unlock_irqrestore(&compositor_lock, flags);
}

static void handle_csi(int win_id, struct window *win, char final, int char_w,
                       int char_h) {
  const char *eb = win->escape_buf;
  int len = win->escape_len;

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

  int a, b, n = csi_params(eb, len, &a, &b);
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

/* =========================================================================
 * Write text to a window (Terminal Emulator)
 * ========================================================================= */
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
  int cols = win->grid_cols;
  int rows = win->grid_rows;

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
        if (win->cursor_x < 0)
          win->cursor_x = 0;
        if (win->cursor_y < 0)
          win->cursor_y = 0;
        if (win->cursor_x >= cols) {
          win->cursor_x = 0;
          win->cursor_y++;
        }
        if (win->cursor_y >= rows) {
          if (win->height > char_h) {
            size_t line_size = win->width * char_h;
            memmove(win->buffer, win->buffer + line_size,
                    win->width * (win->height - char_h) * 4);
          }
          for (int p = win->width * (win->height - char_h);
               p < win->width * win->height; p++)
            win->buffer[p] = win->bg_color;
          if (win->text_grid && win->attr_grid) {
            memmove(win->text_grid, win->text_grid + win->grid_cols,
                    win->grid_cols * (win->grid_rows - 1));
            memmove(win->attr_grid, win->attr_grid + win->grid_cols,
                    win->grid_cols * (win->grid_rows - 1) * 4);
            int last_row_start = win->grid_cols * (win->grid_rows - 1);
            memset(win->text_grid + last_row_start, ' ', win->grid_cols);
            for (int p = 0; p < win->grid_cols; p++)
              win->attr_grid[last_row_start + p] = COLOR_FG;
          }
          win->cursor_y = rows - 1;
        }
        draw_rect_internal(win_id, win->cursor_x * char_w,
                           win->cursor_y * char_h, char_w, char_h,
                           win->curr_bg_color, 1);
        gl_draw_char(&win_surf, win->cursor_x * char_w, win->cursor_y * char_h,
                     c, win->fg_color);
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
        handle_csi(win_id, win, c, char_w, char_h);
        win->escape_state = 0;
      } else if (win->escape_len < 31) {
        win->escape_buf[win->escape_len++] = c;
      } else {
        win->escape_state = 0;
      }
    }
  }

  term_draw_cursor(win_id, win, &win_surf, char_w, char_h);
  expand_damage(win->x, win->y - TITLE_BAR_HEIGHT, win->width,
                win->height + TITLE_BAR_HEIGHT);
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/* =========================================================================
 * Handle Mouse Click
 * ========================================================================= */
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

  int top_z = 0;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].z_order > top_z)
      top_z = windows[i].z_order;
  }
  hit->z_order = top_z + 1;

  if (keyboard_focus_pid != hit->pid) {
    pr_info("Compositor: Focus changed to PID %d (Window '%s')\n", hit->pid,
            hit->title);
    keyboard_focus_pid = hit->pid;
  }

  /* Capture work to deliver AFTER unlock (FIX GFX-COMP-03) */
  int send_pid = -1;
  struct ipc_message msg = {0};
  if (keyboard_focus_pid > 0) {
    msg.from = 0;
    msg.type = IPC_TYPE_MOUSE;
    msg.data1 = (uint64_t)button;
    msg.data2 = (uint64_t)state;
    int rel_x = mouse_x - hit->x;
    int rel_y = mouse_y - hit->y;
    memcpy(msg.payload, &rel_x, 4);
    memcpy(msg.payload + 4, &rel_y, 4);
    send_pid = keyboard_focus_pid;
  }

  int do_close = 0, close_pid = 0;
  if (!hit->protected) {
    int btn_x = hit->x + hit->width - CLOSE_BUTTON_SIZE - 2;
    int btn_y = hit->y - TITLE_BAR_HEIGHT + 2;
    if (mouse_x >= btn_x && mouse_x < btn_x + CLOSE_BUTTON_SIZE &&
        mouse_y >= btn_y && mouse_y < btn_y + CLOSE_BUTTON_SIZE) {
      do_close = 1;
      close_pid = hit->pid;
    }
  }

  if (!do_close && mouse_y >= hit->y - TITLE_BAR_HEIGHT && mouse_y < hit->y) {
    dragging_window_id = hit->id;
    drag_off_x = mouse_x - hit->x;
    drag_off_y = mouse_y - hit->y;
  }

  expand_damage(0, 0, bb_width, bb_height);
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);

  if (send_pid > 0)
    kernel_ipc_send(send_pid, &msg);
  if (do_close) {
    pr_info("Compositor: Close button -> terminate PID %d\n", close_pid);
    extern int process_terminate(int pid);
    process_terminate(close_pid);
  }
}

/* =========================================================================
 * Update Mouse Position
 * ========================================================================= */
void compositor_update_mouse(int dx, int dy, int absolute) {
  struct gpu_device *dev = gpu_get_primary();
  int width = 800;
  int height = 600;
  if (dev) {
    width = dev->width;
    height = dev->height;
  }

  int old_mx = mouse_x, old_my = mouse_y;

  if (absolute) {
    if (dx >= 0)
      mouse_x = (int)(((long)dx * (width - 1)) / INPUT_ABS_MAX);
    if (dy >= 0)
      mouse_y = (int)(((long)dy * (height - 1)) / INPUT_ABS_MAX);
  } else {
    mouse_x += dx;
    mouse_y += dy;
  }

  if (mouse_x < 0)
    mouse_x = 0;
  if (mouse_x >= width)
    mouse_x = width - 1;
  if (mouse_y < 0)
    mouse_y = 0;
  if (mouse_y >= height)
    mouse_y = height - 1;

  if (dragging_window_id != -1) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
      if (windows[i].id == dragging_window_id) {
        windows[i].x = mouse_x - drag_off_x;
        windows[i].y = mouse_y - drag_off_y;
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

  if (dragging_window_id != -1) {
    expand_damage(0, 0, bb_width, bb_height);
  } else {
    expand_damage(old_mx - 1, old_my - 1, 14, 18);
    expand_damage(mouse_x - 1, mouse_y - 1, 14, 18);
  }
  compositor_dirty = 1;
}

/* =========================================================================
 * draw_window_shadow
 *
 * Renders a soft multi-layer drop shadow behind a window by darkening the
 * backbuffer with three concentric rectangles (outermost = lightest).
 * Shadow is offset 2 px right and 3 px down to suggest overhead lighting.
 *
 * Called once per non-top_most window in the bottom-up rendering pass,
 * BEFORE the window's own pixels are placed so the window content correctly
 * overdraws the shadow within its own bounds.
 *
 * The shadow radius equals WIN_CORNER_RADIUS / 2 (min 4 px).
 * ========================================================================= */
static void draw_window_shadow(uint32_t *bb, int bb_w, int bb_h, int wx, int wy,
                               int ww, int wh, int title_h) {
  int elev = WIN_CORNER_RADIUS / 4;
  if (elev < 2)
    elev = 2;

  int decor_y = wy - title_h;
  int decor_h = wh + title_h;

  /* Three concentric layers; alpha values from lightest to darkest */
  static const int shadow_inv[3] = {254, 252, 250}; /* 255 * (1 - alpha) */

  for (int layer = 0; layer < 3; layer++) {
    int spread = elev - layer;
    if (spread <= 0)
      continue;
    int sx = wx - spread + 2;      /* 2-px horizontal offset */
    int sy = decor_y - spread + 3; /* 3-px vertical offset   */
    int sw = ww + spread * 2;
    int sh = decor_h + spread * 2;
    int inv = shadow_inv[layer];

    for (int y = 0; y < sh; y++) {
      int py = sy + y;
      if (py < 0 || py >= bb_h)
        continue;
      for (int x = 0; x < sw; x++) {
        int px = sx + x;
        if (px < 0 || px >= bb_w)
          continue;
        uint32_t *p = &bb[py * bb_w + px];
        uint32_t bg = *p;
        uint32_t r = ((bg >> 16) & 0xFF) * inv / 255;
        uint32_t g = ((bg >> 8) & 0xFF) * inv / 255;
        uint32_t b = (bg & 0xFF) * inv / 255;
        *p = 0xFF000000 | (r << 16) | (g << 8) | b;
      }
    }
  }
}

/* =========================================================================
 * Compositor Render (Region-based / Front-to-Back with Occlusion Culling)
 * ========================================================================= */
#include <kernel/region.h>

static volatile int in_render = 0;
static void compositor_render_internal(void) {
  if (__sync_lock_test_and_set(&in_render, 1))
    return;

  struct gpu_device *dev = gpu_get_primary();
  if (!dev || !compositor_backbuffer) {
    __sync_lock_release(&in_render);
    return;
  }

  int bb_w = bb_width;
  int bb_h = bb_height;
  uint32_t *backbuffer = compositor_backbuffer;

  struct gl_surface screen = {
      .width = bb_w, .height = bb_h, .stride = bb_w, .buffer = backbuffer};

  struct window **sorted = sorted_windows;
  struct region **visible_regions = visible_regions_store;
  memset(visible_regions, 0, sizeof(struct region *) * MAX_WINDOWS);

  int count = 0;
  for (int i = 0; i < MAX_WINDOWS && count < MAX_WINDOWS; i++) {
    if (windows[i].id != 0 && windows[i].visible) {
      if (windows[i].x >= bb_w || windows[i].y >= bb_h ||
          windows[i].x + windows[i].width <= 0 ||
          windows[i].y + windows[i].height <= 0)
        continue;
      sorted[count++] = &windows[i];
    }
  }

  /* Bubble sort by z_order (ascending) */
  for (int i = 0; i < count - 1; i++) {
    for (int j = 0; j < count - i - 1; j++) {
      if (sorted[j]->z_order > sorted[j + 1]->z_order) {
        struct window *tmp = sorted[j];
        sorted[j] = sorted[j + 1];
        sorted[j + 1] = tmp;
      }
    }
  }

  /* Move top-most windows to end of sorted list */
  int current_count = count;
  for (int i = 0; i < current_count; i++) {
    if (sorted[i]->top_most) {
      struct window *tmp = sorted[i];
      for (int k = i; k < current_count - 1; k++)
        sorted[k] = sorted[k + 1];
      sorted[current_count - 1] = tmp;
      current_count--;
      i--;
    }
  }

  /* ------------------------------------------------------------------
   * Pass 1: Visibility Calculation (Top-Down)
   * ------------------------------------------------------------------ */
  struct region *occluded = region_create();
  if (!occluded) {
    __sync_lock_release(&in_render);
    return;
  }

  for (int i = count - 1; i >= 0 && i < MAX_WINDOWS; i--) {
    struct window *win = sorted[i];
    int win_y = win->top_most ? win->y : win->y - TITLE_BAR_HEIGHT;
    int win_h = win->top_most ? win->height : win->height + TITLE_BAR_HEIGHT;

    struct region *vis = region_create();
    if (vis) {
      region_add_rect(vis, win->x, win_y, win->width, win_h);
      for (int r = 0; r < occluded->count; r++) {
        struct rect *or = &occluded->rects[r];
        region_subtract(vis, or->x, or->y, or->w, or->h);
      }
      region_intersect_rect(vis, 0, 0, bb_w, bb_h);
    }
    visible_regions[i] = vis;

    if (!win->has_alpha)
      region_add_rect(occluded, win->x, win_y, win->width, win_h);
  }

  /* Background region = full screen minus occluded area */
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
  occluded = NULL;

  /* ------------------------------------------------------------------
   * Pass 2: Rendering (Bottom-Up – Painter's Algorithm with Clipping)
   * ------------------------------------------------------------------ */
  for (int i = 0; i < count && i < MAX_WINDOWS; i++) {
    struct window *win = sorted[i];
    struct region *vis = visible_regions[i];

    int title_h = win->top_most ? 0 : TITLE_BAR_HEIGHT;
    int content_y = win->y;
    int decor_y = win->y - title_h;

    /* ----------------------------------------------------------------
     * Window shadow: darken backbuffer before placing window pixels.
     * Only non-top_most windows have elevation / shadow.
     * ---------------------------------------------------------------- */
    if (!win->top_most && vis && vis->count > 0)
      draw_window_shadow(backbuffer, bb_w, bb_h, win->x, win->y, win->width,
                         win->height, title_h);

    if (vis) {
      for (int r = 0; r < vis->count; r++) {
        struct rect *vr = &vis->rects[r];

        for (int dy = 0; dy < vr->h; dy++) {
          for (int dx = 0; dx < vr->w; dx++) {
            int screen_x = vr->x + dx;
            int screen_y = vr->y + dy;
            int screen_idx = screen_y * bb_w + screen_x;
            int render = 1; /* skip flag for corner masking */

            if (screen_y < content_y) {
              /* --------------------------------------------------------
               * Decoration Area (Title Bar)
               * -------------------------------------------------------- */
              if (screen_y >= decor_y) {

                /* 1-px MD3 divider at bottom of title bar */
                if (screen_y == content_y - 1) {
                  backbuffer[screen_idx] = COLOR_TITLE_SEPARATOR;
                  continue;
                }

                /* Rounded top corners of the window chrome */
                if (render) {
                  int cr = WIN_CORNER_RADIUS;
                  int tlx = screen_x - win->x;
                  int tly = screen_y - decor_y;
                  if (tlx < cr && tly < cr) {
                    int ddx = tlx - cr, ddy = tly - cr;
                    if (ddx * ddx + ddy * ddy > cr * cr)
                      render = 0;
                  } else if (tlx >= win->width - cr && tly < cr) {
                    int ddx = tlx - (win->width - cr - 1), ddy = tly - cr;
                    if (ddx * ddx + ddy * ddy > cr * cr)
                      render = 0;
                  }
                }

                if (render) {
                  /* Title bar base colour: active vs inactive */
                  uint32_t title_color = (win->pid == keyboard_focus_pid)
                                             ? COLOR_TITLE_ACTIVE
                                             : COLOR_TITLE_INACTIVE;

                  /* Close button with outer ring (macOS traffic-light style).
                   * Position is the same as compositor_handle_click so the
                   * visual exactly matches the click-target rectangle. */
                  if (!win->protected) {
                    int btn_cx =
                        win->x + win->width - 2 - CLOSE_BUTTON_SIZE / 2 - 2;
                    int btn_cy = decor_y + 2 + CLOSE_BUTTON_SIZE / 2 + 3;
                    int ddx = screen_x - btn_cx;
                    int ddy = screen_y - btn_cy;
                    int radius = CLOSE_BUTTON_SIZE / 2 - 1;
                    int ring_r = radius + 1;
                    if (ddx * ddx + ddy * ddy <= ring_r * ring_r) {
                      title_color = (ddx * ddx + ddy * ddy <= radius * radius)
                                        ? COLOR_CLOSE_BTN
                                        : COLOR_CLOSE_BTN_RING;
                    }
                  }
                  backbuffer[screen_idx] = title_color;
                }
                /* if render == 0: corner pixel → background already in buf */
              }

            } else {
              /* --------------------------------------------------------
               * Content Area
               * -------------------------------------------------------- */
              int win_local_x = screen_x - win->x;
              int win_local_y = screen_y - win->y;

              if (win_local_x >= 0 && win_local_x < win->width &&
                  win_local_y >= 0 && win_local_y < win->height) {

                /* Rounded bottom corners of the content area */
                int cr = WIN_CORNER_RADIUS;
                if (win_local_x < cr && win_local_y >= win->height - cr) {
                  int ddx = win_local_x - cr;
                  int ddy = win_local_y - (win->height - cr - 1);
                  if (ddx * ddx + ddy * ddy > cr * cr)
                    render = 0;
                } else if (win_local_x >= win->width - cr &&
                           win_local_y >= win->height - cr) {
                  int ddx = win_local_x - (win->width - cr - 1);
                  int ddy = win_local_y - (win->height - cr - 1);
                  if (ddx * ddx + ddy * ddy > cr * cr)
                    render = 0;
                }

                if (render) {
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
                /* if render == 0: corner pixel → background shows through */
              }
            }
          } /* dx */
        } /* dy */
      }
      region_destroy(vis);
      visible_regions[i] = NULL;
    }

    /* Title text – centred, macOS-style active/inactive colour */
    if (!win->top_most) {
      int char_h = graphics_font_height();
      int text_w = graphics_string_width(win->title);
      int start_x = win->x + (win->width - text_w) / 2;
      int start_y = decor_y + (TITLE_BAR_HEIGHT - char_h) / 2;
      uint32_t text_color = (win->pid == keyboard_focus_pid)
                                ? COLOR_TITLE_TEXT_ACTIVE
                                : COLOR_TITLE_TEXT_INACTIVE;
      gl_draw_string(&screen, start_x, start_y, win->title, text_color);
    }
  }

  /* Cleanup any remaining regions */
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (visible_regions_store[i]) {
      region_destroy(visible_regions_store[i]);
      visible_regions_store[i] = NULL;
    }
  }

  /* Mouse cursor (always on top) */
  static const char *cursor_bits[] = {
      "X           ", "XX          ", "X.X         ", "X..X        ",
      "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
      "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
      "X.X X..X    ", "XX  X..X    ", "X    XX     ", "     XX     "};
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 12; x++) {
      int px = mouse_x + x, py = mouse_y + y;
      if (px >= 0 && px < bb_w && py >= 0 && py < bb_h) {
        char p = cursor_bits[y][x];
        if (p == 'X')
          backbuffer[py * bb_w + px] = 0xFFFFFFFF;
        else if (p == '.')
          backbuffer[py * bb_w + px] = 0xFF000000;
      }
    }
  }

  /* Flush – only upload the damage bounding box */
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
      damage_x1 = bb_w;
      damage_y1 = bb_h;
      damage_x2 = 0;
      damage_y2 = 0;
    }
  }

  /* Cleanup regions (guard against early-return paths above) */
  region_destroy(occluded);
  for (int i = 0; i < count; i++) {
    if (visible_regions[i]) {
      region_destroy(visible_regions[i]);
      visible_regions[i] = NULL;
    }
  }

  __sync_lock_release(&in_render);
}

/* =========================================================================
 * Public render entry points
 * ========================================================================= */
void compositor_render(void) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  compositor_render_internal();
  compositor_dirty = 0;
  spin_unlock_irqrestore(&compositor_lock, flags);
}

void compositor_tick(void) {
  uint64_t flags;
  if (spin_trylock_irqsave(&compositor_lock, &flags)) {
    if (compositor_dirty) {
      compositor_dirty = 0;
      compositor_render_internal();
    }
    spin_unlock_irqrestore(&compositor_lock, flags);
  }
}

/* =========================================================================
 * Draw to Window
 * ========================================================================= */
static void draw_rect_internal(int window_id, int x, int y, int w, int h,
                               uint32_t color, int caller_pid) {
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id && windows[i].buffer) {
      if (windows[i].pid != caller_pid && caller_pid != 1) {
        pr_warn(
            "Compositor: Process %d tried to draw to window %d (owned by %d)\n",
            caller_pid, window_id, windows[i].pid);
        return;
      }
      for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
          int px = x + dx, py = y + dy;
          if (px >= 0 && px < windows[i].width && py >= 0 &&
              py < windows[i].height && windows[i].buffer)
            windows[i].buffer[py * windows[i].width + px] = color;
        }
      }
      int win_y = windows[i].y + (windows[i].top_most ? 0 : TITLE_BAR_HEIGHT);
      expand_damage(windows[i].x + x, win_y + y, w, h);
      return;
    }
  }
}

void compositor_draw_rect(int window_id, int x, int y, int w, int h,
                          uint32_t color, int caller_pid) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  draw_rect_internal(window_id, x, y, w, h, color, caller_pid);
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/* =========================================================================
 * Blit user buffer to window
 * ========================================================================= */
void compositor_blit(int window_id, int x, int y, int w, int h,
                     const uint32_t *user_buf, int caller_pid) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id && windows[i].buffer) {
      if (windows[i].pid != caller_pid && caller_pid != 1) {
        spin_unlock_irqrestore(&compositor_lock, flags);
        return;
      }
      for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < 0 || py >= windows[i].height)
          continue;
        int dest_x = x, src_x = 0, copy_w = w;
        if (dest_x < 0) {
          src_x += -dest_x;
          copy_w -= -dest_x;
          dest_x = 0;
        }
        if (dest_x + copy_w > windows[i].width)
          copy_w = windows[i].width - dest_x;
        if (copy_w <= 0)
          continue;
        void *dst_ptr = &windows[i].buffer[py * windows[i].width + dest_x];
        const void *src_ptr = &user_buf[dy * w + src_x];
        if (vmm_copy_from_user(dst_ptr, src_ptr, copy_w * sizeof(uint32_t)) !=
            0) {
          spin_unlock_irqrestore(&compositor_lock, flags);
          return;
        }
      }
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
        windows[i].visible = 0;
      else if (flags_val & 2)
        windows[i].visible = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
}