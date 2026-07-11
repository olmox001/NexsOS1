/*
 * kernel/graphics/compositor.c
 * Window Compositor
 *
 * Manages windows and composites them to the screen.
 */
#include <drivers/gpu/gpu.h>
#include <drivers/timer.h> /* mono_ns: motion-event rate limiting */
#include <drivers/virtio_input.h>
#include <graphics/gl.h>
#include <kernel/arch.h>
#include <kernel/compositor_style.h>
#include <kernel/cpu.h>
#include <kernel/gfx_chrome.h>
#include <kernel/graphics.h>
#include <kernel/kmalloc.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/term.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <object.h> /* struct window_info, WININFO_* — windows as objects (§6.7) */
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
/* macOS-style minimize/background button (yellow), drawn left of the close
 * button.  NOTE(GFX-WIN-THEME): a conventional fixed colour for now; threading
 * it through compositor_theme_t is a non-blocking follow-up. */
#define COLOR_MIN_BTN 0xFFFEBC2E

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
  int minimized; /* If true, sent to background by the user/WM (dock-
                    restorable).  A minimized window has visible==0 but
                    stays in the table so the dock can restore it; this
                    distinguishes it from a window an app hid itself. */
  int pid;
  int protected;     /* If true, cannot be closed */
  int top_most;      /* If true, always on top and no decorations */
  int passive;       /* If true, click-through: never focused, never hit-tested
                        for input (system popups e.g. notifications). */
  uint32_t *buffer;  /* Window's pixel buffer */
  uint32_t bg_color; /* Default background color */
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

static void expand_window_content_damage(struct window *win, int x, int y,
                                         int w, int h) {
  if (!win || w <= 0 || h <= 0)
    return;

  int dw = win->draw_w > 0 ? win->draw_w : win->width;
  int dh = win->draw_h > 0 ? win->draw_h : win->height;
  if (win->width <= 0 || win->height <= 0 || dw <= 0 || dh <= 0)
    return;

  int x1 = x;
  int y1 = y;
  int x2 = x + w;
  int y2 = y + h;

  if (x1 < 0)
    x1 = 0;
  if (y1 < 0)
    y1 = 0;
  if (x2 > win->width)
    x2 = win->width;
  if (y2 > win->height)
    y2 = win->height;
  if (x2 <= x1 || y2 <= y1)
    return;

  int sx1 = win->x + (int)((int64_t)x1 * dw / win->width);
  int sy1 = win->y + (int)((int64_t)y1 * dh / win->height);
  int sx2 = win->x + (int)(((int64_t)x2 * dw + win->width - 1) / win->width);
  int sy2 = win->y + (int)(((int64_t)y2 * dh + win->height - 1) / win->height);
  expand_damage(sx1, sy1, sx2 - sx1, sy2 - sy1);
}

static int window_region_has_alpha(struct window *win, int x, int y, int w,
                                   int h) {
  if (!win || !win->buffer || w <= 0 || h <= 0)
    return 0;

  int x1 = x < 0 ? 0 : x;
  int y1 = y < 0 ? 0 : y;
  int x2 = x + w;
  int y2 = y + h;
  if (x2 > win->width)
    x2 = win->width;
  if (y2 > win->height)
    y2 = win->height;
  if (x2 <= x1 || y2 <= y1)
    return 0;

  for (int py = y1; py < y2; py++) {
    uint32_t *row = win->buffer + (size_t)py * win->width;
    for (int px = x1; px < x2; px++) {
      if ((row[px] >> 24) != 0xFF)
        return 1;
    }
  }
  return 0;
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

/* Interactive resize state (edge/corner grip — GFX-DYN-01 F1).  While resizing
 * we only change the on-screen draw size (draw_w/draw_h): the compositor scales
 * the logical surface for fluidity (no per-frame realloc in IRQ).  On release
 * we send the owner an INPUT_TYPE_RESIZE event so it can reallocate a crisp
 * buffer (apps that ignore it just stay scaled). */
#define RESIZE_GRIP 6 /* px hot zone along window edges */
#define RESIZE_MIN_W 120
#define RESIZE_MIN_H 80
#define RESIZE_EDGE_R 1
#define RESIZE_EDGE_B 2
#define RESIZE_EDGE_L 4
static int resizing_window_id = -1;
static int resize_edge = 0;
static int resize_start_mx = 0, resize_start_my = 0;
static int resize_orig_w = 0, resize_orig_h = 0, resize_orig_x = 0;

/* Title-bar height now comes from the active style (compositor_titlebar_height,
 * Phase 5): 20 by default, 0 for the chrome-less Minimal style.  Button
 * size/gap and their geometry live in gfx_chrome (gfx_chrome_button_geometry),
 * the single source shared by the render pass and the click hit-test. */

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
static int bb_width = 0; /* render/scanout width (== GPU mode; what apps see) */
static int bb_height = 0; /* render/scanout height                            */
static int bb_pages = 0;  /* pages backing compositor_backbuffer              */

/* Desktop zoom / HiDPI (F2, reworked).  We do NOT scale in software: the GPU
 * scanout RESOURCE is resized to native*100/zoom and QEMU stretches that to the
 * host window (which is exactly what the manual set-mode path does).  The
 * backbuffer == the scanout size; native_w/h is the zoom-100 reference (the
 * host display size).  zoom>100 ⇒ smaller scanout, QEMU upscales ⇒ HiDPI. */
static int native_w = 0, native_h = 0;
static int desktop_zoom = 100;

/*
 * Initialize Compositor
 */
void compositor_init(void) {
  memset(windows, 0, sizeof(windows));
  window_count = 0;
  next_window_id = 100;

  /* Backbuffer == the GPU scanout size at boot (no hard-coded size). */
  struct gpu_device *dev = gpu_get_primary();
  int w = dev ? dev->width : COMPOSITOR_FALLBACK_W;
  int h = dev ? dev->height : COMPOSITOR_FALLBACK_H;
  native_w = w;
  native_h = h;
  desktop_zoom = 100;

  bb_pages = (int)(((size_t)w * h * 4 + 4095) / 4096);
  compositor_backbuffer = pmm_alloc_pages_dma(bb_pages);
  if (!compositor_backbuffer) {
    pr_err("%s", "Compositor: Failed to allocate backbuffer!\n");
    bb_pages = 0;
    bb_width = 0;
    bb_height = 0;
    return;
  }
  bb_width = w;
  bb_height = h;

  /* First frame is a full upload. */
  damage_x1 = 0;
  damage_y1 = 0;
  damage_x2 = bb_width;
  damage_y2 = bb_height;

  pr_info("Compositor: Initialized (%dx%d)\n", bb_width, bb_height);
}

/* Mark the whole desktop dirty (full repaint next tick).  Used by the
 * Style/Theme switch (compositor_style.c) so a new look shows immediately. */
void compositor_full_damage(void) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  damage_x1 = 0;
  damage_y1 = 0;
  damage_x2 = bb_width;
  damage_y2 = bb_height;
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);
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
 * compositor_reserved_top_locked / compositor_reserved_bottom_locked -
 * FIX(GFX-COMP-RESERVE-01): infer the screen-edge strip currently claimed by
 * permanent top_most chrome (nxbar's top bar, nxui's dock) so that ordinary
 * window placement/resize/drag never slides underneath it.
 *
 * Before this fix, compositor_create_window(), compositor_resize(), and
 * compositor_update_mouse()'s drag clamp only checked compositor_titlebar_
 * height() at the top (a per-window DECORATION metric — the height of a
 * window's OWN titlebar — which has nothing to do with nxbar) and the bare
 * screen edge at the bottom (nothing reserved at all).  Practical effect:
 *   - a window could be created or dragged with y as low as the titlebar
 *     height, sliding it underneath nxbar's strip, which the compositor had
 *     no notion of;
 *   - any window taller than (screen_h - a few px) got its y clamped to
 *     "screen_h - h" — flush against the physical bottom edge — with no
 *     margin for nxui's dock, so it ended up hidden behind the dock instead
 *     of stopping above it.
 *
 * The compositor still does not know "nxbar" or "nxui" by name (kernel/
 * userland separation, ASTRA §1) — it infers the reservation purely from
 * geometry: any VISIBLE, TOP_MOST window whose edge sits within
 * RESERVE_EDGE_SLOP pixels of a screen edge is treated as permanent chrome
 * anchored to that edge, and its footprint is reserved.  exclude_id lets a
 * window being placed/dragged skip counting itself (pass 0 if the window has
 * no id yet, e.g. during creation).  Caller must hold compositor_lock.
 *
 * Defined here (ahead of compositor_resize, the first user) rather than
 * right before compositor_create_window: a static function must be declared
 * before every call site in the same translation unit, and compositor_resize
 * appears earlier in this file than compositor_create_window.
 */
#define RESERVE_EDGE_SLOP                                                      \
  8 /* nxui's DOCK_MARGIN_BOTTOM (4px) and nxbar's own                         \
     * top inset both fall well inside this */

static int compositor_reserved_top_locked(int exclude_id) {
  /* VECCHIO: partiva da compositor_titlebar_height() → forzava
   * anche le finestre senza barra (top_most) a stare sotto un
   * margine inesistente.
   */
  /* NUOVO: la riserva è determinata SOLO dalle finestre top_most
   * ancorate al bordo superiore, non dalla decorazione.
   */
  int top = 0;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == 0 || windows[i].id == exclude_id)
      continue;
    if (!windows[i].visible || !windows[i].top_most)
      continue;
    if (windows[i].y <= RESERVE_EDGE_SLOP) {
      int reserved = windows[i].y + windows[i].height;
      if (reserved > top)
        top = reserved;
    }
  }
  return top;
}

static int compositor_reserved_bottom_locked(int exclude_id) {
  int bottom = 0;
  int screen_h = bb_height > 0 ? bb_height : 600;
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == 0 || windows[i].id == exclude_id)
      continue;
    if (!windows[i].visible || !windows[i].top_most)
      continue;
    int gap = screen_h - (windows[i].y + windows[i].height);
    if (gap >= 0 && gap <= RESERVE_EDGE_SLOP) {
      int reserved = windows[i].height + gap;
      if (reserved > bottom)
        bottom = reserved;
    }
  }
  return bottom;
}

/*
 * compositor_resize - retarget the backbuffer to the scanout's new w x h.
 *
 * The caller must have already resized the GPU scanout (gpu_set_mode) to the
 * SAME size; the backbuffer always matches the scanout, and QEMU stretches the
 * scanout to the host window (no software scaling here).  Reallocates the
 * backbuffer to exactly w*h — so it must run in process context (it does:
 * SYS_SET_DISPLAY_MODE / SYS_SET_ZOOM / SYS_DISPLAY_POLL), never the IRQ tick.
 * The new buffer is allocated before the lock and the old one freed after, to
 * keep the compositor_lock hold (shared with the render tick) short.
 */
void compositor_resize(int w, int h) {
  if (w <= 0 || h <= 0)
    return;
  if (w == bb_width && h == bb_height)
    return; /* nothing to do */

  int npages = (int)(((size_t)w * h * 4 + 4095) / 4096);
  uint32_t *nbuf = pmm_alloc_pages_dma(npages);
  if (!nbuf) {
    pr_err("Compositor: resize %dx%d: out of memory\n", w, h);
    return;
  }
  memset(nbuf, 0, (size_t)npages * 4096);

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  uint32_t *obuf = compositor_backbuffer;
  int opages = bb_pages;
  compositor_backbuffer = nbuf;
  bb_pages = npages;
  bb_width = w;
  bb_height = h;

  /* Keep every window's title bar / close button on-screen at the new size.
   * FIX(GFX-COMP-RESERVE-01): also keep it above nxbar and above nxui's dock
   * — previously only compositor_titlebar_height() (top) and a bare 20px
   * (bottom) were enforced, with no notion of either chrome strip, so a
   * resolution change could resettle a window behind nxbar or behind the
   * dock. exclude_id=windows[i].id: a top_most window (nxbar/nxui itself)
   * must not count its own footprint against itself. */
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == 0)
      continue;
    if (windows[i].x > bb_width - 40)
      windows[i].x = bb_width - 40;
    if (windows[i].x < 0)
      windows[i].x = 0;
    int reserved_top = compositor_reserved_top_locked(windows[i].id);
    int reserved_bottom = compositor_reserved_bottom_locked(windows[i].id);
    if (windows[i].y > bb_height - reserved_bottom - 20)
      windows[i].y = bb_height - reserved_bottom - 20;
    if (windows[i].y < reserved_top)
      windows[i].y = reserved_top;
  }

  damage_x1 = 0;
  damage_y1 = 0;
  damage_x2 = bb_width;
  damage_y2 = bb_height;
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);

  if (obuf)
    pmm_free_pages(obuf, opages);
  pr_info("Compositor: resized to %dx%d\n", bb_width, bb_height);
}

/* compositor_set_native_mode - record a real mode change (the user/host set a
 * concrete resolution): this becomes the zoom-100 reference and resets zoom. */
void compositor_set_native_mode(int w, int h) {
  if (w > 0 && h > 0) {
    native_w = w;
    native_h = h;
    desktop_zoom = 100;
  }
}

/* compositor_set_zoom - desktop HiDPI/zoom (F2).  Resizes the actual GPU
 * scanout resource to native*100/zoom and lets QEMU stretch it to the host
 * window; the backbuffer follows.  Clamped to [100,400] (zoom-in only —
 * zoom-out would need a scanout larger than the panel).  Backs SYS_SET_ZOOM. */
int compositor_set_zoom(int percent) {
  if (percent < 100)
    percent = 100;
  if (percent > 400)
    percent = 400;
  if (native_w <= 0 || native_h <= 0)
    return -1;

  int nw = (int)((long)native_w * 100 / percent);
  int nh = (int)((long)native_h * 100 / percent);
  if (nw < 1 || nh < 1)
    return -1;

  desktop_zoom = percent;
  if (gpu_set_mode(nw, nh) != 0)
    return -1;
  compositor_resize(nw, nh);
  pr_info("Compositor: zoom %d%% -> scanout %dx%d (native %dx%d)\n", percent,
          nw, nh, native_w, native_h);
  return 0;
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
    /* Clamp into the desktop-virtual area (where windows live), not the
     * physical scanout — they differ under zoom (F2).  We already hold
     * compositor_lock, so read bb_width/bb_height directly (compositor_get_size
     * would re-lock). */
    int screen_w = bb_width > 0 ? bb_width : 800;
    int screen_h = bb_height > 0 ? bb_height : 600;

    if (x + w > screen_w)
      x = screen_w - w;
    if (x < 0)
      x = 0;

    /* FIX(GFX-COMP-RESERVE-01): clamp against the LIVE top/bottom chrome
     * reservation (nxbar/nxui), not just compositor_titlebar_height() at the
     * top and the bare screen edge at the bottom — see the helper comment
     * above. exclude_id=0: this window has no id yet, nothing to exclude. */
    int reserved_top = compositor_reserved_top_locked(0);
    int reserved_bottom = compositor_reserved_bottom_locked(0);

    if (y + h > screen_h - reserved_bottom)
      y = screen_h - reserved_bottom - h;
    if (y < reserved_top)
      y = reserved_top;
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
  uint32_t default_bg = compositor_theme_active()->win_bg;
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
  windows[slot].minimized = 0;
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

  windows[slot].has_alpha = ((default_bg >> 24) != 0xFF);

  /* Clear buffer to background */
  for (int i = 0; i < w * h; i++) {
    buffer[i] = windows[slot].bg_color;
  }

  /* Mark main shell (PID 2) as protected */
  windows[slot].protected = (pid == 2) ? 1 : 0;
  windows[slot].top_most = 0;
  windows[slot].passive = 0;

  window_count++;

  /* ============================================================
   * VECCHIO: nessuna chiamata a expand_damage, nessun compositor_dirty
   * ============================================================ */

  /* ============================================================
   * NUOVO: marca l'intera area della finestra (contenuto + barra
   * del titolo) come danneggiata così il prossimo compositor_tick
   * la ridisegna immediatamente.
   * ============================================================ */
  int ddw = windows[slot].draw_w > 0 ? windows[slot].draw_w : w;
  int ddh = windows[slot].draw_h > 0 ? windows[slot].draw_h : h;
  expand_damage(x, y - compositor_titlebar_height(), ddw,
                ddh + compositor_titlebar_height());
  compositor_dirty = 1;

  pr_debug("Compositor: Created window '%s' (%dx%d) at (%d,%d)\n", title, w, h,
           x, y); /* hot path under GUI churn: demoted (perf §1) */
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

/* ========================================================================= */
/* Window state control + enumeration (ASTRA §6.7: windows as objects).      */
/* These back the OBJ_TYPE_WINDOW capability (kernel/core/object.c) and       */
/* SYS_WINDOW_ENUM, used by the dock /sys/bin/nxui and the titlebar           */
/* background button below.  The compositor stays the mechanism; policy       */
/* (dock layout, what to restore) lives in userland (nxui).                   */
/* ========================================================================= */

/* __raise_to_front_locked - give a window the highest z-order.  Caller holds
 * compositor_lock.  Mirrors the click-to-front logic in
 * compositor_handle_click. */
static void __raise_to_front_locked(struct window *w) {
  int top_z = 0;
  for (int i = 0; i < MAX_WINDOWS; i++)
    if (windows[i].id != 0 && windows[i].z_order > top_z)
      top_z = windows[i].z_order;
  w->z_order = top_z + 1;
}

/* compositor_minimize_window - send a window to the background: hide it but
 * keep its slot/buffer so the dock can restore it.  If it held focus, focus
 * falls to the next top-most visible window.  Returns 0, or -ESRCH for an
 * unknown id. */
int compositor_minimize_window(int window_id) {
  uint64_t flags;
  int ret = -ESRCH;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      windows[i].visible = 0;
      windows[i].minimized = 1;
      if (keyboard_focus_pid == windows[i].pid)
        __focus_topmost_locked(); /* re-point focus off the hidden window */
      expand_damage(0, 0, bb_width, bb_height);
      compositor_dirty = 1;
      ret = 0;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return ret;
}

/* compositor_restore_window - bring a backgrounded window back: show it, raise
 * it to the front and give it keyboard focus.  Returns 0, or -ESRCH for unknown
 * id. The focus push (sched_set_focus_pid + compositor_focus_changed) runs
 * AFTER the unlock, matching SYS_SET_FOCUS and keeping compositor_lock off the
 * focus path. */
int compositor_restore_window(int window_id) {
  uint64_t flags;
  int target_pid = -1;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      windows[i].visible = 1;
      windows[i].minimized = 0;
      __raise_to_front_locked(&windows[i]);
      target_pid = windows[i].pid;
      expand_damage(0, 0, bb_width, bb_height);
      compositor_dirty = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  if (target_pid < 0)
    return -ESRCH;
  sched_set_focus_pid(target_pid);
  compositor_focus_changed(target_pid);
  return 0;
}

/* compositor_focus_window - raise a window and give it keyboard focus; reveals
 * it if it was hidden/minimized.  Returns 0, or -ESRCH for an unknown id. */
int compositor_focus_window(int window_id) {
  uint64_t flags;
  int target_pid = -1;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      if (!windows[i].visible) {
        windows[i].visible = 1;
        windows[i].minimized = 0;
      }
      __raise_to_front_locked(&windows[i]);
      target_pid = windows[i].pid;
      expand_damage(0, 0, bb_width, bb_height);
      compositor_dirty = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  if (target_pid < 0)
    return -ESRCH;
  sched_set_focus_pid(target_pid);
  compositor_focus_changed(target_pid);
  return 0;
}

/* __fill_window_info_locked - serialise one window slot into the shared ABI
 * record (include/api/object.h).  Caller holds compositor_lock. */
static void __fill_window_info_locked(const struct window *w,
                                      struct window_info *wi) {
  wi->id = w->id;
  wi->pid = w->pid;
  wi->x = w->x;
  wi->y = w->y;
  wi->w = w->draw_w > 0 ? w->draw_w : w->width;
  wi->h = w->draw_h > 0 ? w->draw_h : w->height;
  unsigned int f = 0;
  if (w->visible)
    f |= WININFO_VISIBLE;
  if (w->minimized)
    f |= WININFO_MINIMIZED;
  if (w->top_most)
    f |= WININFO_TOPMOST;
  if (w->passive)
    f |= WININFO_PASSIVE;
  if (w->pid == keyboard_focus_pid)
    f |= WININFO_FOCUSED;
  wi->flags = f;
  int k = 0;
  while (k < 63 && w->title[k]) {
    wi->title[k] = w->title[k];
    k++;
  }
  wi->title[k] = '\0';
}

/* compositor_window_info - fill one window's info record by id.  Returns 0, or
 * -ESRCH for an unknown id.  Backs OS1_object_read() on an OBJ_TYPE_WINDOW. */
int compositor_window_info(int window_id, struct window_info *out) {
  uint64_t flags;
  int ret = -ESRCH;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      __fill_window_info_locked(&windows[i], out);
      ret = 0;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return ret;
}

/* sys_window_enum - SYS_WINDOW_ENUM backend: snapshot every live window into
 * the caller's struct window_info[] and return the count.  Read-only and
 * ungated (enumeration is information, not authority — acting on a window still
 * needs an OBJ_TYPE_WINDOW capability).  Mirrors sys_getprocs: snapshot under
 * the lock, copy to user after unlocking. */
long sys_window_enum(struct window_info *ubuf, size_t max) {
  if (!ubuf || max == 0)
    return 0;
  if (max > MAX_WINDOWS)
    max = MAX_WINDOWS;
  struct window_info *kbuf =
      (struct window_info *)kmalloc(sizeof(struct window_info) * max);
  if (!kbuf)
    return -ENOMEM;

  uint64_t flags;
  size_t n = 0;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS && n < max; i++) {
    if (windows[i].id != 0)
      __fill_window_info_locked(&windows[i], &kbuf[n++]);
  }
  spin_unlock_irqrestore(&compositor_lock, flags);

  if (n > 0 &&
      vmm_copy_to_user(ubuf, kbuf, sizeof(struct window_info) * n) != 0) {
    kfree(kbuf);
    return -EFAULT;
  }
  kfree(kbuf);
  return (long)n;
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
      /* #118: damage the vacated footprint BEFORE zeroing so the area under the
       * window (and lower windows) is recomposited at the next tick — no ghost.
       */
      int ddw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
      int ddh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
      expand_damage(windows[i].x, windows[i].y - compositor_titlebar_height(),
                    ddw, ddh + compositor_titlebar_height());
      compositor_dirty = 1;
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
      /* #118: damage the vacated footprint before zeroing (no ghost window). */
      int ddw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
      int ddh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
      expand_damage(windows[i].x, windows[i].y - compositor_titlebar_height(),
                    ddw, ddh + compositor_titlebar_height());
      compositor_dirty = 1;
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

  /* No-op if already at this logical size: keeps the grip-resize release path
   * (which echoes an INPUT_TYPE_RESIZE) from looping when the app re-applies
   * the same size. */
  if (win->width == w && win->height == h && win->draw_w == w &&
      win->draw_h == h) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return 0;
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
  expand_damage(win->x, win->y - compositor_titlebar_height(), old_dw,
                old_dh + compositor_titlebar_height());

  uint32_t *obuf = win->buffer;
  win->buffer = nbuf;
  win->width = w;
  win->height = h;
  win->draw_w = w; /* crisp: on-screen == new logical */
  win->draw_h = h;
  win->has_alpha = ((win->bg_color >> 24) != 0xFF);

  /* Reflow the terminal to the new cell grid. */
  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  if (char_w > 0 && char_h > 0)
    term_resize(&win->term, w / char_w, h / char_h);

  /* Damage the NEW footprint and repaint. */
  expand_damage(win->x, win->y - compositor_titlebar_height(), w,
                h + compositor_titlebar_height());
  compositor_dirty = 1;
  int owner = win->pid;
  spin_unlock_irqrestore(&compositor_lock, flags);

  if (obuf)
    kfree(obuf);

  /* Notify the owner of its new logical size (outside the lock: kernel_ipc_send
   * takes sched_lock — never nest it under compositor_lock, cf. GFX-COMP-03).
   */
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
    expand_damage(win->x, win->y - compositor_titlebar_height(), win->width,
                  win->height + compositor_titlebar_height());
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

  /* The caret is drawn only on the window that currently owns keyboard input.
   */
  win->term.focused = (win->pid == keyboard_focus_pid);
  term_write(&win->term, &win_surf, buf, count);

  /* Mark compositor as needing redraw (window area including title bar) */
  expand_damage(win->x, win->y - compositor_titlebar_height(), win->width,
                win->height + compositor_titlebar_height());
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
  if (state == 0) {
    /* Button up: end drag/resize.  If we were resizing, tell the window owner
     * its new size so it can reallocate a crisp buffer (it may ignore it and
     * stay scaled). */
    int notify_pid = -1, nw = 0, nh = 0;
    int release_pid = -1;
    struct ipc_message release_msg = {0};
    uint64_t rflags;
    spin_lock_irqsave(&compositor_lock, &rflags);
    /* Only the LEFT button drives drag/resize, so only its release ends
     * them (a right/middle release must not cancel an ongoing left-drag). */
    if (button == BTN_LEFT) {
      dragging_window_id = -1;
      if (resizing_window_id != -1) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
          if (windows[i].id == resizing_window_id) {
            notify_pid = windows[i].pid;
            nw = windows[i].draw_w;
            nh = windows[i].draw_h;
            break;
          }
        }
        resizing_window_id = -1;
        resize_edge = 0;
      }
    }
    /* Deliver the RELEASE to the focused app with the same IPC_TYPE_MOUSE
     * message the press uses (data2 = 0) — no new ABI.  Button-state
     * consumers (SDL tracks pressed buttons) need the matching release or
     * every press after the first is treated as still-held.  The pointer may
     * be released outside the window, so coordinates are clamped into the
     * content area before the draw-rect -> logical mapping. */
    if (keyboard_focus_pid > 0) {
      for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *fw = &windows[i];
        if (fw->id == 0 || !fw->visible || fw->passive ||
            fw->pid != keyboard_focus_pid)
          continue;
        int fdw = fw->draw_w > 0 ? fw->draw_w : fw->width;
        int fdh = fw->draw_h > 0 ? fw->draw_h : fw->height;
        int rel_x = mouse_x - fw->x;
        int rel_y = mouse_y - fw->y;
        if (rel_x < 0)
          rel_x = 0;
        if (rel_y < 0)
          rel_y = 0;
        if (rel_x >= fdw)
          rel_x = fdw - 1;
        if (rel_y >= fdh)
          rel_y = fdh - 1;
        if (fdw > 0 && fdw != fw->width)
          rel_x = (int)((int64_t)rel_x * fw->width / fdw);
        if (fdh > 0 && fdh != fw->height)
          rel_y = (int)((int64_t)rel_y * fw->height / fdh);
        release_msg.from = 0; /* Kernel */
        release_msg.type = IPC_TYPE_MOUSE;
        release_msg.data1 = (uint64_t)button;
        release_msg.data2 = 0;
        memcpy(release_msg.payload, &rel_x, 4);
        memcpy(release_msg.payload + 4, &rel_y, 4);
        release_pid = keyboard_focus_pid;
        break;
      }
    }
    spin_unlock_irqrestore(&compositor_lock, rflags);
    if (notify_pid > 0) {
      struct ipc_message msg = {0};
      msg.type = IPC_TYPE_RESIZE;
      msg.data1 = (uint64_t)nw;
      msg.data2 = (uint64_t)nh;
      kernel_ipc_send(notify_pid, &msg);
    }
    if (release_pid > 0)
      kernel_ipc_send(release_pid, &release_msg);
    return;
  }

  if (state != 1)
    return;

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);

  struct window *hit = NULL;
  int max_z = -1;

  for (int i = 0; i < MAX_WINDOWS; i++) {
    /* Passive windows (system notifications) are click-through: never
     * hit-tested, so a click on the popup passes to whatever is beneath it and
     * the popup neither steals focus/caret nor receives an IPC_TYPE_MOUSE
     * event. */
    if (windows[i].id != 0 && windows[i].visible && !windows[i].passive) {
      int dw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
      int dh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
      int title_top = windows[i].y - compositor_titlebar_height();
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

  /* Interactive resize (F1): a press within RESIZE_GRIP of the
   * left/right/bottom edge starts an edge/corner resize.  The top edge stays
   * drag-only (title bar).  Protected windows are not grip-resizable.  While
   * resizing we change only draw_w/draw_h (compositor scales); the crisp
   * realloc happens when the app handles the INPUT_TYPE_RESIZE sent on release.
   */
  {
    int dw = hit->draw_w > 0 ? hit->draw_w : hit->width;
    int dh = hit->draw_h > 0 ? hit->draw_h : hit->height;
    int edge = 0;
    if (mouse_y >= hit->y && mouse_y < hit->y + dh) {
      if (mouse_x >= hit->x + dw - RESIZE_GRIP && mouse_x < hit->x + dw)
        edge |= RESIZE_EDGE_R;
      else if (mouse_x >= hit->x && mouse_x < hit->x + RESIZE_GRIP)
        edge |= RESIZE_EDGE_L;
    }
    if (mouse_x >= hit->x && mouse_x < hit->x + dw &&
        mouse_y >= hit->y + dh - RESIZE_GRIP && mouse_y < hit->y + dh)
      edge |= RESIZE_EDGE_B;

    if (button == BTN_LEFT && edge && !hit->protected) {
      resizing_window_id = hit->id;
      resize_edge = edge;
      resize_start_mx = mouse_x;
      resize_start_my = mouse_y;
      resize_orig_w = dw;
      resize_orig_h = dh;
      resize_orig_x = hit->x;
      expand_damage(0, 0, bb_width, bb_height);
      compositor_dirty = 1;
      spin_unlock_irqrestore(&compositor_lock, flags);
      return;
    }
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
    /* Relative coordinates, mapped from the on-screen draw rect back to the
     * app's LOGICAL surface so the app sees its own coords.  Only presses in
     * the CONTENT area are delivered: chrome presses (titlebar, buttons,
     * drag) belong to the window manager, and a negative rel_y would leak
     * WM geometry into the app's input stream. */
    int hdw = hit->draw_w > 0 ? hit->draw_w : hit->width;
    int hdh = hit->draw_h > 0 ? hit->draw_h : hit->height;
    int rel_x = mouse_x - hit->x;
    int rel_y = mouse_y - hit->y;
    if (rel_x >= 0 && rel_y >= 0 && rel_x < hdw && rel_y < hdh) {
      if (hdw > 0 && hdw != hit->width)
        rel_x = (int)((int64_t)rel_x * hit->width / hdw);
      if (hdh > 0 && hdh != hit->height)
        rel_y = (int)((int64_t)rel_y * hit->height / hdh);
      msg.from = 0; /* Kernel */
      msg.type = IPC_TYPE_MOUSE;
      msg.data1 = (uint64_t)button;
      msg.data2 = (uint64_t)state;
      memcpy(msg.payload, &rel_x, 4);
      memcpy(msg.payload + 4, &rel_y, 4);
      send_pid = keyboard_focus_pid;
    }
  }

  /* Capture a titlebar-button hit; the action is deferred until after unlock.
   * Two buttons, right-aligned: [background][close].  The background button
   * sits one button-width + BG_BUTTON_GAP to the LEFT of the close button. */
  int do_close = 0;
  int close_pid = 0;
  int do_minimize = 0;
  int min_id = 0;
  if (button == BTN_LEFT && !hit->protected) {
    const compositor_style_t *st = compositor_style_active();
    int hdw = hit->draw_w > 0 ? hit->draw_w : hit->width;
    int title_h = compositor_titlebar_height();
    int decor_y = hit->y - title_h;

    /* Same gfx_chrome geometry the render pass paints with — the hit-test
     * can no longer drift from the pixels on screen. */
    gfx_button_geometry_t buttons;
    gfx_chrome_button_geometry(hit->x, hdw, decor_y, title_h, st->button_shape,
                               st->button_side, &buttons);
    int hit_button = gfx_chrome_button_hit(&buttons, mouse_x, mouse_y);
    if (hit_button == GFX_BUTTON_CLOSE) {
      do_close = 1;
      close_pid = hit->pid;
    } else if (hit_button == GFX_BUTTON_BACKGROUND) {
      do_minimize = 1;
      min_id = hit->id;
    }
  }

  /* Check for drag start (skipped when closing/minimizing, matching the old
   * early-return).  Left button only: right/middle never start a drag. */
  if (button == BTN_LEFT && !do_close && !do_minimize &&
      mouse_y >= hit->y - compositor_titlebar_height() && mouse_y < hit->y) {
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
   * NOTE: the close still force-terminates in mouse-IRQ context; deferring it
   * to a safe context is the separate SCHED-03 follow-up, now localised behind
   * the seam. */
  if (send_pid > 0)
    kernel_ipc_send(send_pid, &msg);
  if (do_close) {
    pr_info("Compositor: Close button -> request close of PID %d\n", close_pid);
    window_request_close(close_pid);
  }
  /* Background button: send the window to the dock.  compositor_minimize_window
   * re-takes compositor_lock, so it must run here (after the unlock), and it
   * re-finds the window by id, so a slot that changed meanwhile is handled
   * gracefully — same contract as window_request_close above. */
  if (do_minimize)
    compositor_minimize_window(min_id);
}

/*
 * Update Mouse Position
 */

void compositor_update_mouse(int dx, int dy, int absolute) {
  /* The cursor lives in desktop-virtual space (the backbuffer), which equals
   * the physical scanout at zoom 100 but differs under HiDPI/zoom (F2). */
  int width = bb_width > 0 ? bb_width : 800;
  int height = bb_height > 0 ? bb_height : 600;

  /* S1b: hold compositor_lock for the whole window-list update.  This handler
   * runs in mouse-IRQ context and was the ONLY window-list mutator WITHOUT the
   * lock — racing window create/destroy (e.g. 'stress' churning windows) and
   * the render tick.  The torn drag/resize writes to windows[].draw_w/draw_h
   * could then be consumed out-of-bounds by a concurrent draw syscall
   * (observed: kernel stack/pointer smash, RIP->0xf9, while dragging a demo3d
   * window). Same lock order as compositor_handle_click (no caller holds it;
   * the helpers called below — expand_damage, compositor_titlebar_height — take
   * no lock). */
  uint64_t cflags;
  spin_lock_irqsave(&compositor_lock, &cflags);

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
        /* Enforce screen boundaries (use on-screen draw size).
         * FIX(GFX-COMP-RESERVE-01): clamp against the live nxbar/nxui
         * reservation instead of just compositor_titlebar_height() at the
         * top and the bare screen edge at the bottom — a dragged window used
         * to be draggable right underneath nxbar, or down behind the dock. */
        if (windows[i].x < 0)
          windows[i].x = 0;
        if (windows[i].x + dw > width)
          windows[i].x = width - dw;
        int reserved_top = compositor_reserved_top_locked(windows[i].id);
        int reserved_bottom = compositor_reserved_bottom_locked(windows[i].id);
        if (windows[i].y < reserved_top)
          windows[i].y = reserved_top;
        if (windows[i].y + dh > height - reserved_bottom)
          windows[i].y = height - reserved_bottom - dh;
        break;
      }
    }
  }

  /* Handle interactive resize (F1): adjust the on-screen draw size from the
   * mouse delta + grabbed edge; the compositor scales the logical surface. */
  if (resizing_window_id != -1) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
      if (windows[i].id != resizing_window_id)
        continue;
      int ddx = mouse_x - resize_start_mx;
      int ddy = mouse_y - resize_start_my;
      int nw = resize_orig_w, nh = resize_orig_h, nx = resize_orig_x;
      if (resize_edge & RESIZE_EDGE_R)
        nw = resize_orig_w + ddx;
      if (resize_edge & RESIZE_EDGE_L) {
        nw = resize_orig_w - ddx;
        nx = resize_orig_x + ddx;
      }
      if (resize_edge & RESIZE_EDGE_B)
        nh = resize_orig_h + ddy;
      if (nw < RESIZE_MIN_W) {
        if (resize_edge &
            RESIZE_EDGE_L) /* keep right edge fixed when clamping */
          nx = resize_orig_x + (resize_orig_w - RESIZE_MIN_W);
        nw = RESIZE_MIN_W;
      }
      if (nh < RESIZE_MIN_H)
        nh = RESIZE_MIN_H;
      /* S1b: clamp the on-screen size to the screen so a runaway resize cannot
       * push draw_w/draw_h past the backbuffer (OOB in the scaling blit). */
      if (nw > width)
        nw = width;
      if (nh > height)
        nh = height;
      windows[i].draw_w = nw;
      windows[i].draw_h = nh;
      windows[i].x = nx < 0 ? 0 : nx;
      break;
    }
  }

  /* Mark compositor as needing redraw - don't render from IRQ! */
  if (dragging_window_id != -1 || resizing_window_id != -1) {
    expand_damage(0, 0, bb_width, bb_height);
  } else {
    /* Only the old and new cursor areas (12x16 + 1px border) */
    expand_damage(old_mx - 1, old_my - 1, 14, 18);
    expand_damage(mouse_x - 1, mouse_y - 1, 14, 18);
  }
  compositor_dirty = 1;

  /* Capture a motion event for the focused window using the SAME
   * IPC_TYPE_MOUSE message the click path sends — no new ABI.  button = 0
   * means "no button, motion only": every existing consumer already treats a
   * zero button as not-a-click, and the SDL2 adapter turns it into
   * SDL_MOUSEMOTION.  Rate-limited so the unbounded per-process IPC queue
   * cannot be flooded from mouse-IRQ context; delivery happens after the
   * unlock, mirroring compositor_handle_click.  Suppressed during drag/resize
   * (the window itself is moving under the cursor). */
  int motion_pid = -1;
  struct ipc_message motion_msg = {0};
  if ((mouse_x != old_mx || mouse_y != old_my) && keyboard_focus_pid > 0 &&
      dragging_window_id == -1 && resizing_window_id == -1) {
    static uint64_t last_motion_ns;
    uint64_t now = mono_ns();
    if (now - last_motion_ns >= 8000000ull) { /* >= 8 ms (~125 Hz) */
      for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *fw = &windows[i];
        if (fw->id == 0 || !fw->visible || fw->passive ||
            fw->pid != keyboard_focus_pid)
          continue;
        int fdw = fw->draw_w > 0 ? fw->draw_w : fw->width;
        int fdh = fw->draw_h > 0 ? fw->draw_h : fw->height;
        int rel_x = mouse_x - fw->x;
        int rel_y = mouse_y - fw->y;
        if (rel_x < 0 || rel_y < 0 || rel_x >= fdw || rel_y >= fdh)
          break; /* cursor outside the focused content area: no motion */
        /* Map from the on-screen draw rect back to LOGICAL surface coords,
         * exactly like the click path. */
        if (fdw > 0 && fdw != fw->width)
          rel_x = (int)((int64_t)rel_x * fw->width / fdw);
        if (fdh > 0 && fdh != fw->height)
          rel_y = (int)((int64_t)rel_y * fw->height / fdh);
        motion_msg.from = 0; /* Kernel */
        motion_msg.type = IPC_TYPE_MOUSE;
        motion_msg.data1 = 0; /* no button: motion only */
        motion_msg.data2 = 0;
        memcpy(motion_msg.payload, &rel_x, 4);
        memcpy(motion_msg.payload + 4, &rel_y, 4);
        motion_pid = keyboard_focus_pid;
        last_motion_ns = now;
        break;
      }
    }
  }

  spin_unlock_irqrestore(&compositor_lock, cflags);

  /* Outside compositor_lock (same contract as compositor_handle_click). */
  if (motion_pid > 0)
    kernel_ipc_send(motion_pid, &motion_msg);
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

/* Rounded-rect membership (F3) now lives in gfx_chrome as
 * gfx_rrect_contains(), shared with the chrome shadow/border primitives. */

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

  /* Damage clip for this frame (perf §3.4): composite ONLY the changed region
   * instead of the full scene.  Snapshot the accumulated damage (reset later at
   * the flush) clamped to the screen.  The damage tracking is already complete
   * — mouse-move damages the old+new cursor rects, drag/resize/window-ops
   * full-damage — so this box always covers everything that changed.  The
   * backbuffer persists between frames, so pixels outside the clip stay valid
   * and are never re-touched (and the flush already uploads only this box).
   * Cutting the per-frame composite to the damage box is the main fix for the
   * "scattoso" jank: a cursor move now recomposites ~2 tiny rects, not
   * 1280x800, and the render no longer stalls CPU0's timer IRQ for a full-scene
   * pass. */
  int clip_x1 = damage_x1 < 0 ? 0 : damage_x1;
  int clip_y1 = damage_y1 < 0 ? 0 : damage_y1;
  int clip_x2 = damage_x2 > bb_w ? bb_w : damage_x2;
  int clip_y2 = damage_y2 > bb_h ? bb_h : damage_y2;
  if (clip_x2 < clip_x1)
    clip_x2 = clip_x1;
  if (clip_y2 < clip_y1)
    clip_y2 = clip_y1;
  int clip_w = clip_x2 - clip_x1;
  int clip_h = clip_y2 - clip_y1;

  /* Active theme (colours) + style (form) + background — read once per frame.
   */
  const compositor_theme_t *th = compositor_theme_active();
  const compositor_style_t *st = compositor_style_active();
  const compositor_background_t *desk_bg = compositor_background_active();

  /* Wrap backbuffer in GL Surface */
  struct gl_surface screen = {
      .width = bb_w, .height = bb_h, .stride = bb_w, .buffer = backbuffer};

  /* Damage clip as the rect the gfx_chrome primitives honour. */
  gfx_rect_t chrome_clip = {clip_x1, clip_y1, clip_w, clip_h};

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
    int win_y = win->top_most ? win->y : win->y - compositor_titlebar_height();
    int win_h = win->top_most ? dh : dh + compositor_titlebar_height();

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
      /* Clip to this frame's damage box (perf §3.4): only the changed area is
       * recomposited; the rest of the backbuffer persists from last frame. */
      region_intersect_rect(vis, clip_x1, clip_y1, clip_w, clip_h);
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
    /* Only repaint the background within this frame's damage box (perf §3.4).
     */
    region_intersect_rect(bg_region, clip_x1, clip_y1, clip_w, clip_h);

    /* Draw Background — vertical gradient from the active background preset
     * (desk_bg->bg_top -> desk_bg->bg_bottom), interpolated per row. */
    uint32_t t_r = (desk_bg->bg_top >> 16) & 0xFF,
             t_g = (desk_bg->bg_top >> 8) & 0xFF, t_b = desk_bg->bg_top & 0xFF;
    uint32_t b_r = (desk_bg->bg_bottom >> 16) & 0xFF,
             b_g = (desk_bg->bg_bottom >> 8) & 0xFF,
             b_b = desk_bg->bg_bottom & 0xFF;
    for (int r = 0; r < bg_region->count; r++) {
      struct rect *bg = &bg_region->rects[r];
      for (int y = 0; y < bg->h; y++) {
        int sy = bg->y + y;
        if (sy < 0 || sy >= bb_h)
          continue;
        /* Row colour: linear blend top->bottom by sy/bb_h. */
        int denom = bb_h > 1 ? bb_h - 1 : 1;
        uint32_t rr = t_r + (int)(b_r - t_r) * sy / denom;
        uint32_t gg = t_g + (int)(b_g - t_g) * sy / denom;
        uint32_t bb = t_b + (int)(b_b - t_b) * sy / denom;
        uint32_t row_color = 0xFF000000 | (rr << 16) | (gg << 8) | bb;
        for (int x = 0; x < bg->w; x++) {
          int sx = bg->x + x;
          if (sx >= 0 && sx < bb_w)
            backbuffer[sy * bb_w + sx] = row_color;
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

    /* Skip windows entirely outside this frame's damage box (perf §3.4): their
     * content (vis is already clipped to empty) AND chrome (title text / border
     * / shadow drawn below) need not be repainted — that part of the backbuffer
     * is unchanged.  Any leftover vis region is freed by the end-of-function
     * cleanup loop, so the early continue does not leak. */
    {
      int wth = win->top_most ? 0 : compositor_titlebar_height();
      int wy = win->y - wth;
      int wfh = dh + wth;
      /* Include the drop-shadow fringe in the footprint so a
       * window whose body is just outside the damage box but whose shadow
       * reaches into it is still reprocessed. */
      int so = (st->shadows && !win->top_most) ? st->shadow_size : 0;
      gfx_chrome_margins_t margins;
      gfx_chrome_shadow_margins(st->shadow_type, so, &margins);
      if (win->x - margins.left >= clip_x2 ||
          win->x + dw + margins.right <= clip_x1 ||
          wy - margins.top >= clip_y2 || wy + wfh + margins.bottom <= clip_y1)
        continue;
    }

    /* Decoration Params */
    int title_h = win->top_most ? 0 : compositor_titlebar_height();
    int content_y = win->y;
    int decor_y = win->y - title_h;

    /* Style (F3): rounded corners + border applied to the full window rect
     * (title + content); shadows drawn under the window.  top_most overlays
     * (notifications, cursor layers) keep square, chrome-less. */
    int full_h = title_h + dh;
    int rr = (st->rounded_corners && !win->top_most) ? st->border_radius : 0;

    /* Button geometry once per window (was recomputed per pixel). */
    gfx_button_geometry_t buttons;
    gfx_chrome_button_geometry(win->x, dw, decor_y, title_h, st->button_shape,
                               st->button_side, &buttons);

    /* Drop shadow: behaviour depends on st->shadow_type.
     * Guard: shadows enabled, non-zero size, not a top-most overlay.
     * Painters live in gfx_chrome and honour the frame's damage clip. */
    if (st->shadows && st->shadow_size > 0 && !win->top_most) {
      gfx_rect_t chrome_frame = {win->x, decor_y, dw, full_h};
      if (st->shadow_type == 0)
        gfx_chrome_shadow_solid(&screen, &chrome_clip, &chrome_frame, rr,
                                th->win_bg);
      else if (st->shadow_type == 2)
        gfx_chrome_shadow_premium(&screen, &chrome_clip, &chrome_frame, rr,
                                  st->shadow_size);
      else if (st->shadow_type == 1)
        gfx_chrome_shadow_fast(&screen, &chrome_clip, &chrome_frame, rr,
                               st->shadow_size);
    }

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
                                           ? th->title_active
                                           : th->title_inactive;

                if (st->shadows && title_h > 0)
                  title_color = gfx_chrome_titlebar_tint(st->shadow_type,
                                                         screen_y - decor_y,
                                                         title_h, title_color);

                /* Titlebar buttons (gfx_chrome): geometry computed once per
                 * window below; drawn only when the window is not protected —
                 * consistent with compositor_handle_click ignoring the
                 * buttons on hit->protected, which now shares the same
                 * gfx_chrome_button_geometry. */
                if (!win->protected)
                  title_color = gfx_chrome_button_pixel(
                      &buttons, st->button_shape, screen_x, screen_y,
                      th->close_btn, COLOR_MIN_BTN,
                      st->shadows && st->shadow_type == 2, title_color);

                /* Round the top corners (F3). */
                if (rr == 0 ||
                    gfx_rrect_contains(screen_x - win->x, screen_y - decor_y,
                                       dw, full_h, rr))
                  backbuffer[screen_idx] = title_color;
              }
            } else {
              /* Content Area.  Position within the on-screen draw rect... */
              int draw_x = screen_x - win->x;
              int draw_y = screen_y - win->y;

              if (draw_x >= 0 && draw_x < dw && draw_y >= 0 && draw_y < dh &&
                  (rr == 0 ||
                   gfx_rrect_contains(screen_x - win->x, screen_y - decor_y, dw,
                                      full_h, rr))) {
                /* ...mapped back to the logical surface (nearest-sample scale
                 * when draw size != logical size — GFX-DYN-01 surface model).
                 */
                int sx =
                    scaled ? (int)((int64_t)draw_x * win->width / dw) : draw_x;
                int sy =
                    scaled ? (int)((int64_t)draw_y * win->height / dh) : draw_y;
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

                /* Inner shadow / separator under titlebar (gfx_chrome) */
                if (st->shadows && title_h > 0)
                  backbuffer[screen_idx] = gfx_chrome_content_separator(
                      st->shadow_type, draw_y, backbuffer[screen_idx]);
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
                                ? th->title_text_active
                                : th->title_text_inactive;

      /* Damage clip (GFX-COMP-03): title text is drawn unoccluded over the
       * titlebar, so it must be confined to this frame's damage box or it would
       * overpaint pixels outside the recomposited region. */
      gl_draw_string_clipped(&screen, start_x, start_y, win->title, text_color,
                             clip_x1, clip_y1, clip_x2, clip_y2);
    }

    /* Window border (F3): 1px outline around the full window rect, following
     * the rounded corners.  Drawn last so it sits on top of content + title. */
    if (st->window_borders && !win->top_most) {
      gfx_rect_t chrome_frame = {win->x, decor_y, dw, full_h};
      gfx_chrome_border(&screen, &chrome_clip, &chrome_frame, rr, th->border);
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
    /* Backbuffer == scanout (zoom resizes the scanout itself; QEMU stretches it
     * to the host window).  Upload only the damage bounding box. */
    if (fb_va && bb_w == dev->width && bb_h == dev->height) {
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
      if ((color >> 24) != 0xFF) {
        windows[i].has_alpha = 1;
      } else if (x <= 0 && y <= 0 && w >= windows[i].width &&
                 h >= windows[i].height) {
        windows[i].has_alpha = 0;
      }

      expand_window_content_damage(&windows[i], x, y, w, h);
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

      int copied_x1 = windows[i].width;
      int copied_y1 = windows[i].height;
      int copied_x2 = 0;
      int copied_y2 = 0;

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
        if (dest_x < copied_x1)
          copied_x1 = dest_x;
        if (py < copied_y1)
          copied_y1 = py;
        if (dest_x + copy_w > copied_x2)
          copied_x2 = dest_x + copy_w;
        if (py + 1 > copied_y2)
          copied_y2 = py + 1;
      }

      if (copied_x2 > copied_x1 && copied_y2 > copied_y1) {
        int cw = copied_x2 - copied_x1;
        int ch = copied_y2 - copied_y1;
        int copied_has_alpha =
            window_region_has_alpha(&windows[i], copied_x1, copied_y1, cw, ch);
        if (copied_has_alpha) {
          windows[i].has_alpha = 1;
        } else if (copied_x1 == 0 && copied_y1 == 0 &&
                   copied_x2 >= windows[i].width &&
                   copied_y2 >= windows[i].height) {
          windows[i].has_alpha = 0;
        }
        expand_window_content_damage(&windows[i], copied_x1, copied_y1, cw, ch);
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
      windows[i].passive = (flags_val & 8) ? 1 : 0; /* bit 3: click-through */
      if (flags_val & 4)
        windows[i].visible = 0; /* bit 2: hide window */
      else if (flags_val & 2)
        windows[i].visible = 1; /* bit 1: show window */
      /* Damage the window's footprint so the show/hide/restack is actually
       * composited + flushed on the next render.  Without this the state
       * changed but the damage box stayed empty, so the popup only refreshed
       * when some OTHER event (the mouse passing over it) damaged that region —
       * the "notification sticks / won't disappear until I move the mouse" bug.
       * Same footprint math as compositor_destroy_windows_by_pid. */
      {
        int ddw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
        int ddh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
        expand_damage(windows[i].x, windows[i].y - compositor_titlebar_height(),
                      ddw, ddh + compositor_titlebar_height());
        compositor_dirty = 1;
      }
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
}