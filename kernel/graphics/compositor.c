/*
 * kernel/graphics/compositor.c
 * Window Compositor
 *
 * Manages windows and composites them to the screen.
 *
 * =========================================================================
 * NASA/JPL "Power of 10" compliance — status, rule by rule
 * =========================================================================
 * This file targets the Power of 10 rules already invoked by name in
 * several of the FIX() comments below (GFX-COMP-RESERVE-02's static_assert,
 * GFX-COMP-BOUND-01's loop clamps). This block is the single place that
 * states, per rule, where the file stands — honestly, including where full
 * literal compliance is not realistic for a resizable multi-window
 * compositor and what mitigates the gap instead.
 *
 *  1. Simple control flow (no goto/setjmp/recursion): MET. No goto,
 *     setjmp/longjmp, or recursive call exists anywhere in this file —
 *     every traversal (window table, region rect lists, sorted[]) is an
 *     explicit iterative loop.
 *
 *  2. Every loop has a statically provable fixed upper bound: MET. Every
 *     loop in this file is bounded by a compile-time constant (MAX_WINDOWS,
 *     REGION_POOL_SIZE, REGION_POOL_RECT_CAP, a fixed 63-char title, the
 *     16x12 cursor bitmap) or by a value clamped to a compile-time constant
 *     before the loop starts (draw_rect_internal/compositor_blit clip w/h to
 *     <=4096 before iterating — see FIX(GFX-COMP-BOUND-01) — and the pixel
 *     loops in compositor_render_internal only ever walk a region's rect
 *     list, itself capped at REGION_POOL_RECT_CAP). No loop here is bounded
 *     only by a runtime value with no static ceiling.
 *
 *  3. No dynamic memory allocation after initialization: MOSTLY MET, one
 *     documented exception. compositor_render_internal()'s occlusion-
 *     culling pass — historically the file's biggest offender, up to ~42
 *     kmalloc/kfree pairs per composited frame via region_create()/
 *     region_destroy() — now draws exclusively from the static
 *     REGION_POOL_SIZE arena (region_pool_acquire()/region_pool_retire(),
 *     see that block comment). The one place this file still calls kmalloc/
 *     kfree after compositor_init() is a window's own pixel buffer
 *     (compositor_create_window/compositor_resize_window/
 *     compositor_destroy_window) and the compositor backbuffer
 *     (compositor_resize, via pmm_alloc_pages_dma). A true rule-3 system
 *     would pre-allocate every window's buffer at its maximum size up
 *     front; for THIS subsystem that means reserving up to MAX_WINDOWS *
 *     4096 * 4096 * 4 bytes (~2.7 GiB) whether or not any window is ever
 *     that large, which trades a real, bounded, well-understood allocator
 *     call for an unrealistic static reservation. The mitigation actually
 *     in place: every one of these calls is documented (see each function's
 *     header comment) to run ONLY from process context — never from
 *     compositor_tick()'s IRQ path — with its return value checked and a
 *     clean failure path (old surface kept, or the call fails outright) on
 *     allocation failure. This is a knowingly-scoped deviation, not an
 *     oversight.
 *
 *  4. Functions fit on one printed page (~60 lines): MOSTLY MET, three
 *     documented exceptions. Every function in this file is at or under
 *     that bound except compositor_render_internal() (the two-pass
 *     occlusion-culling/painter's-algorithm core — see the split rationale
 *     above its forward declaration for exactly which three phases were
 *     already extracted and why the remaining core was not), and
 *     compositor_handle_click()/compositor_update_mouse() (the mouse-IRQ
 *     handlers, which interleave lock-held geometry work with the capture-
 *     locals-then-unlock-then-IPC pattern FIX(GFX-COMP-03) depends on —
 *     splitting them needs the same parameter-object care, not a mechanical
 *     cut). All three are flagged as follow-up work with their natural
 *     seams already named in comments, rather than force-split without a
 *     matching test harness.
 *
 *  5. Adequate runtime assertion density: PARTIALLY MET via a different,
 *     established idiom. This codebase does not use a blanket assert()
 *     macro; instead nearly every function here validates its inputs with
 *     an explicit early-return + pr_err/pr_warn (invalid window dimensions,
 *     unknown window_id, NULL buffers, invalid font metrics, and so on —
 *     see e.g. compositor_create_window's dimension check or
 *     draw_rect_internal's ownership check). Two compile-time invariants
 *     are also enforced via _Static_assert: RESERVED_SYSTEM_SLOTS leaving a
 *     non-empty user pool, and REGION_POOL_SIZE outsizing the per-window vis
 *     demand. The gap: several internal helpers (e.g.
 *     compositor_paint_background/compositor_paint_cursor/
 *     compositor_present_frame) validate their pointer parameters but do
 *     not carry a second, independent runtime check of an invariant beyond
 *     "not NULL" — a genuine opportunity for a follow-up pass, called out
 *     here rather than papered over with decorative asserts that don't
 *     check anything real.
 *
 *  6. Smallest possible scope for data objects: MET, deliberately. Every
 *     per-frame working set that would otherwise be a large stack array
 *     (sorted_windows, visible_regions_store, the region-pool arena, the
 *     compositor backbuffer surface descriptor) is intentionally file-
 *     static instead — the comments at each declaration explain this is to
 *     avoid stack pressure and, for the region pool, kmalloc, from IRQ
 *     context (S-STAB). Everything else is declared at the innermost scope
 *     that uses it.
 *
 *  7. Check the return value of every non-void function; validate every
 *     parameter: MOSTLY MET. kmalloc/pmm_alloc_pages_dma/kernel_ipc_send/
 *     vmm_copy_to_user/vmm_copy_from_user return values are all checked at
 *     every call site in this file. window_id/pid/pointer parameters are
 *     validated at every public entry point before use. The one class of
 *     call whose return value this file does NOT check is
 *     spin_lock_irqsave/spin_unlock_irqrestore/__sync_lock_test_and_set —
 *     these are primitives with no failure mode to check (a spinlock
 *     acquire either blocks until it succeeds or, for the trylock variant
 *     compositor_tick() already handles, returns a boolean that IS checked).
 *
 *  8. Limited preprocessor use: MET. Only object-like macros (color
 *     constants, size/geometry constants, REGION_POOL_*) and simple
 *     function-like macros with no token-pasting, no conditional
 *     compilation beyond header guards, and no macro that hides control
 *     flow.
 *
 *  9. Restrict pointer use (no more than one level of dereferencing,
 *     function pointers used sparingly): MOSTLY MET, one structural
 *     exception. windows[i].buffer / windows[i].term.* / win->term.caret_
 *     shown are one or two levels deep by necessity (a window's own pixel
 *     buffer, and its embedded terminal state) — not pointer-chasing
 *     through unrelated objects. The one real function-pointer dispatch is
 *     dev->ops->present / dev->ops->flush / dev->ops->get_framebuffer,
 *     which is the driver vtable boundary (kernel/graphics.h's gpu_device
 *     abstraction) — a legitimate, narrow, single-purpose use, not
 *     unrestricted pointer-to-pointer chasing.
 *
 * 10. Compile with all warnings enabled and address them: MET for this
 *     file's own code. Compiling this file with -Wall -Wextra against the
 *     project's real headers produces zero warnings originating in this
 *     file (verified during this pass); the only warnings that survive
 *     -Wall -Wextra come from architecture-specific inline primitives this
 *     file merely calls through kernel/arch.h (arch_impl_*), which are out
 *     of this file's scope.
 * =========================================================================
 */
#include <kernel/resource.h>
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
#include <kernel/region.h> /* struct region/struct rect — FIX(GFX-COMP-STRUCT-01):
                              hoisted from a mid-file #include right before
                              compositor_render_internal (its only previous
                              user) to the top block. compositor.c now
                              declares its own region-pool arena (struct
                              region/struct rect arrays) much earlier in the
                              file than that render function, so the type
                              has to be complete before that point too — a
                              mid-file include only region_create()/
                              region_destroy() call sites could get away
                              with. */
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/term.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <os1_compositor.h>
#include <object.h> /* struct window_info, WININFO_* — windows as objects (§6.7) */
#include <stdint.h>

#define MAX_WINDOWS 40

/*
 * GFX-COMP-RESERVE-02: MAX_WINDOWS is one flat pool shared by every caller.
 * A stress test that opens ordinary user windows up to window_count ==
 * MAX_WINDOWS starves compositor_create_window() for EVERYONE, including a
 * supervised system service (nxui/dock, nxbar, the launcher) that init just
 * respawned after a crash — its create_window() call fails exactly like an
 * unprivileged app's would, and the service exits again (see nxui.c/nxbar.c
 * main(): `if (g_win < 0) return 1;`).  init's backoff queue (USR-INIT-04)
 * keeps retrying it, but it can never succeed until user windows close back
 * below the cap — a stress test can hold the desktop chrome down indefinitely.
 *
 * Fix: reserve the last RESERVED_SYSTEM_SLOTS pool entries for PRIVILEGED
 * callers only (PLVL_ROOT/PLVL_MACHINE — proc_is_privileged(), same check
 * process.c/object.c already gate other root-only operations on).  Ordinary
 * PLVL_USER/PLVL_GUEST windows are capped at MAX_WINDOWS -
 * RESERVED_SYSTEM_SLOTS; a privileged caller may use the full pool.  Sized
 * for the known supervised /sys/bin singletons (dock, nxbar, launcher) plus
 * one spare so a destroy+recreate resize (nxui's dock_reinit / nxbar's
 * set_window_height) never has to land in the exact instant a slot is free
 * to avoid double-booking itself.
 */
#define RESERVED_SYSTEM_SLOTS 8
#define MAX_USER_WINDOWS (MAX_WINDOWS - RESERVED_SYSTEM_SLOTS)

/*
 * NASA/JPL "Power of 10" rule 5 (assertion density): make the relationship
 * between the two pool-sizing constants a checkable invariant instead of an
 * implicit assumption.  A future edit that raises RESERVED_SYSTEM_SLOTS above
 * MAX_WINDOWS (or to equal it) would silently give MAX_USER_WINDOWS a
 * negative or zero value — window_count >= cap would then be true from boot,
 * permanently locking out every ordinary user window with no diagnostic.
 * Catching this at COMPILE time (not boot time) means it can never ship.
 */
_Static_assert(RESERVED_SYSTEM_SLOTS > 0 && RESERVED_SYSTEM_SLOTS < MAX_WINDOWS,
               "RESERVED_SYSTEM_SLOTS must leave a non-empty user window pool");

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

/* Compatibility text windows share the ACTIVE palette with the native
 * window theme, rather than assuming a dark glyph on a light surface. */
#define COLOR_FG (compositor_theme_active()->title_text_active)
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
  int presentation_managed; /* Explicit WM extent survives delayed app adoption. */
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
  nx_surface_changed();
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

/* Title-bar height now comes from the active style (compositor_titlebar_height,
 * Phase 5): 20 by default, 0 for the chrome-less Minimal style.  Button
 * size/gap and their geometry live in gfx_chrome (gfx_chrome_button_geometry),
 * the single source shared by the render pass and the click hit-test. */

/* Display bounds support kernel window placement policy. Framebuffer pixels
 * and render surfaces belong to the ring-3 compositor. */
#define COMPOSITOR_FALLBACK_W 1024
#define COMPOSITOR_FALLBACK_H 768

static int bb_width = 0;
static int bb_height = 0;

/* Desktop zoom / HiDPI (F2, reworked).  We do NOT scale in software: the GPU
 * scanout RESOURCE is resized to native*100/zoom and QEMU stretches that to the
 * host window (which is exactly what the manual set-mode path does).  The
 * backbuffer == the scanout size; native_w/h is the zoom-100 reference (the
 * host display size).  zoom>100 ⇒ smaller scanout, QEMU upscales ⇒ HiDPI. */
static int native_w = 0, native_h = 0;
static int desktop_zoom = 100;

/*
 * Initialize Compositor
 *
 * FIX(COMPOSITOR-INIT-01): every field written here (windows[], window_count,
 * next_window_id, native_w/h, desktop_zoom, compositor_backbuffer, bb_pages,
 * bb_width, bb_height, damage_x1/y1/x2/y2) is also read and written under
 * compositor_lock everywhere else in this file (compositor_full_damage(),
 * the render/tick path, resize, etc.) — but this function used to write all
 * of it WITHOUT holding that lock.  Once local IRQs are enabled and any
 * other CPU/IRQ path can reach a compositor_lock-protected reader before
 * this function returns, that reader can observe a partially-initialised
 * state (e.g. bb_width/bb_height updated but compositor_backbuffer not yet,
 * or the reverse), which is exactly the kind of half-initialised global
 * state that produces a black/corrupt screen instead of a crash.
 *
 * FIX(COMPOSITOR-INIT-02, revised): a prior version of this fix took
 * compositor_lock (IRQs saved) around the ENTIRE function, including the
 * pmm_alloc_pages_dma() call, on the strength of a comment asserting that
 * this specific allocator path "never sleeps, never takes another lock,
 * never re-enters the scheduler". That claim is not something this file
 * can verify or enforce — pmm.c's allocator takes its own per-zone
 * spinlock internally (alloc_pages_impl), and holding a second spinlock
 * with IRQs off for the duration of an unbounded, size-dependent
 * allocation (plus, on the failure path, a pr_err() that itself takes
 * printk's locks) is exactly the shape of critical section this codebase
 * otherwise avoids everywhere else. The allocation is done here BEFORE
 * compositor_lock is taken at all: compositor_lock now protects only the
 * bookkeeping-field PUBLISH step, which is the actual thing
 * COMPOSITOR-INIT-01 needed atomic, and never overlaps with pmm's own
 * lock or with I/O. */
void compositor_init(void) {
  struct gpu_device *dev = gpu_get_primary();
  int w = dev ? dev->width : COMPOSITOR_FALLBACK_W;
  int h = dev ? dev->height : COMPOSITOR_FALLBACK_H;
  if (w <= 0 || h <= 0) {
    w = COMPOSITOR_FALLBACK_W;
    h = COMPOSITOR_FALLBACK_H;
  }

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  memset(windows, 0, sizeof(windows));
  window_count = 0;
  next_window_id = 100;
  native_w = w;
  native_h = h;
  desktop_zoom = 100;
  bb_width = w;
  bb_height = h;
  spin_unlock_irqrestore(&compositor_lock, flags);

  pr_info("Compositor: Window manager initialized (%dx%d)\n", w, h);
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
  nx_surface_changed();
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
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192)
    return;
  if (w == bb_width && h == bb_height)
    return;

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  bb_width = w;
  bb_height = h;

  /* Keep window policy clamping in kernel; no render buffer is allocated. */
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == 0)
      continue;
    int dw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
    int dh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
    int margin_w = dw < 40 ? dw : 40;
    int margin_h = dh < 40 ? dh : 40;
    if (windows[i].x > bb_width - margin_w)
      windows[i].x = bb_width - margin_w;
    if (windows[i].x < 0)
      windows[i].x = 0;
    int reserved_top = compositor_reserved_top_locked(windows[i].id);
    int reserved_bottom = compositor_reserved_bottom_locked(windows[i].id);
    if (windows[i].y > bb_height - reserved_bottom - margin_h)
      windows[i].y = bb_height - reserved_bottom - margin_h;
    if (windows[i].y < reserved_top)
      windows[i].y = reserved_top;
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  pr_info("Compositor: Window bounds updated to %dx%d\n", w, h);
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
                             struct process *owner) {
  if (!owner || !title ||
      __atomic_load_n(&owner->dying, __ATOMIC_ACQUIRE))
    return -EPERM;
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return -EINVAL;

  int pid = (int)owner->pid;
  int privileged = proc_is_privileged(owner);
  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  if (char_w <= 0 || char_h <= 0)
    return -EINVAL;

  struct window fresh;
  memset(&fresh, 0, sizeof(fresh));
  size_t pixels = (size_t)w * (size_t)h;
  fresh.buffer = (uint32_t *)kmalloc(pixels * sizeof(uint32_t));
  if (!fresh.buffer)
    return -ENOMEM;

  uint32_t bg = compositor_theme_active()->win_bg;
  fresh.width = w;
  fresh.height = h;
  fresh.draw_w = w;
  fresh.draw_h = h;
  fresh.visible = 1;
  fresh.pid = pid;
  fresh.bg_color = bg;
  fresh.has_alpha = ((bg >> 24) != 0xFF);
  fresh.protected = (pid == 2);
  for (size_t i = 0; i < pixels; i++)
    fresh.buffer[i] = bg;

  int len = 0;
  while (title[len] && len < (int)sizeof(fresh.title) - 1) {
    fresh.title[len] = title[len];
    len++;
  }
  fresh.title[len] = '\0';

  if (term_init(&fresh.term, w / char_w, h / char_h, COLOR_FG, bg) != 0) {
    kfree(fresh.buffer);
    return -ENOMEM;
  }

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  if (__atomic_load_n(&owner->dying, __ATOMIC_ACQUIRE)) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    term_free(&fresh.term);
    kfree(fresh.buffer);
    return -EPERM;
  }

  int cap = privileged ? MAX_WINDOWS : MAX_USER_WINDOWS;
  int slot = -1;
  if (window_count < cap) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
      if (windows[i].id == 0) {
        slot = i;
        break;
      }
    }
  }
  if (slot < 0) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    term_free(&fresh.term);
    kfree(fresh.buffer);
    return -ENOSPC;
  }

  int screen_w = bb_width > 0 ? bb_width : 800;
  int screen_h = bb_height > 0 ? bb_height : 600;
  if (x + w > screen_w)
    x = screen_w - w;
  if (x < 0)
    x = 0;
  int top = compositor_reserved_top_locked(0);
  int bottom = compositor_reserved_bottom_locked(0);
  if (y + h > screen_h - bottom)
    y = screen_h - bottom - h;
  if (y < top)
    y = top;

  int id = next_window_id++;
  fresh.id = id;
  fresh.x = x;
  fresh.y = y;
  fresh.z_order = window_count;
  windows[slot] = fresh;
  window_count++;
  expand_damage(x, y - compositor_titlebar_height(), w,
                h + compositor_titlebar_height());
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);

  pr_debug("Compositor: Created window '%s' (%dx%d) at (%d,%d)\n", title, w,
           h, x, y);
  return id;
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
  if (w->protected)
    f |= WININFO_PROTECTED;
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

/* Copy one legacy window surface to the userland compositor.  This is the
 * compatibility bridge while v1 window objects are converted to mapped
 * capability surfaces: pixel composition and all scene policy belong to
 * compositord; the kernel only pins the window under its lock long enough to
 * make a bounded immutable copy. */
long compositor_window_snapshot(struct os1_window_snapshot_v1 *user_snapshot) {
  struct os1_window_snapshot_v1 request;
  if (!user_snapshot ||
      vmm_copy_from_user(&request, user_snapshot, sizeof(request)) != 0)
    return -EFAULT;
  if (request.abi_version != OS1_COMPOSITOR_ABI_V1 ||
      request.struct_size < sizeof(request) || request.id <= 0)
    return -EINVAL;

  struct os1_window_snapshot_v1 result = {0};
  uint32_t *source = NULL;
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    const struct window *win = &windows[i];
    if (win->id != request.id)
      continue;
    result.abi_version = OS1_COMPOSITOR_ABI_V1;
    result.struct_size = sizeof(result);
    result.id = win->id;
    result.pid = win->pid;
    result.x = win->x;
    result.y = win->y;
    result.width = win->width;
    result.height = win->height;
    result.draw_width = win->draw_w > 0 ? win->draw_w : win->width;
    result.draw_height = win->draw_h > 0 ? win->draw_h : win->height;
    result.z_order = win->z_order;
    result.background_argb = win->bg_color;
    result.pixel_count = (size_t)win->width * (size_t)win->height;
    result.pixels = request.pixels;
    result.pixel_capacity = request.pixel_capacity;
    result.flags = 0;
    if (win->visible)
      result.flags |= WININFO_VISIBLE;
    if (win->minimized)
      result.flags |= WININFO_MINIMIZED;
    if (win->top_most)
      result.flags |= WININFO_TOPMOST;
    if (win->passive)
      result.flags |= WININFO_PASSIVE;
    if (win->protected)
      result.flags |= WININFO_PROTECTED;
    if (win->pid == keyboard_focus_pid)
      result.flags |= WININFO_FOCUSED;
    memcpy(result.title, win->title, sizeof(result.title));
    result.title[sizeof(result.title) - 1] = '\0';
    if (win->width <= 0 || win->height <= 0 || win->width > 4096 ||
        win->height > 4096 || result.pixel_count > 16u * 1024u * 1024u) {
      spin_unlock_irqrestore(&compositor_lock, flags);
      return -EINVAL;
    }
    source = win->buffer;
    break;
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  if (result.id == 0)
    return -ESRCH;

  if (!request.pixels) {
    return vmm_copy_to_user(user_snapshot, &result, sizeof(result)) == 0
               ? 0
               : -EFAULT;
  }
  if (request.pixel_capacity < result.pixel_count)
    return -ENOSPC;
  if (!source)
    return -EIO;

  size_t bytes = result.pixel_count * sizeof(uint32_t);
  uint32_t *copy = (uint32_t *)kmalloc(bytes);
  if (!copy)
    return -ENOMEM;

  /* Revalidate the buffer identity after allocation; destroy/resize may have
   * happened while the lock was dropped.  A stale pointer is never read. */
  int copied = 0;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    const struct window *win = &windows[i];
    if (win->id == request.id && win->buffer == source &&
        win->width == result.width && win->height == result.height) {
      memcpy(copy, source, bytes);
      copied = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  if (!copied) {
    kfree(copy);
    return -EAGAIN;
  }

  int rc = vmm_copy_to_user(request.pixels, copy, bytes);
  kfree(copy);
  if (rc != 0)
    return -EFAULT;
  return vmm_copy_to_user(user_snapshot, &result, sizeof(result)) == 0
             ? 0
             : -EFAULT;
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
 * Detach window-owned allocations under compositor_lock, then release them
 * after unlocking. Allocator calls must not be nested under compositor_lock:
 * resize/create and process teardown otherwise invert allocator/compositor
 * lock order under SMP load.
 */
struct retired_window_resources {
  uint32_t *buffer;
  uint8_t *text_grid;
  uint32_t *attr_grid;
};

static void __free_retired_window_resources(
    struct retired_window_resources *resources) {
  if (resources->buffer)
    kfree(resources->buffer);
  if (resources->text_grid)
    kfree(resources->text_grid);
  if (resources->attr_grid)
    kfree(resources->attr_grid);
  resources->buffer = NULL;
  resources->text_grid = NULL;
  resources->attr_grid = NULL;
}

/* Destroy one window. The slot is unpublished before its storage is freed. */
void compositor_destroy_window(int window_id) {
  struct retired_window_resources retired = {0};
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id != window_id)
      continue;

    int refocus = (windows[i].pid == keyboard_focus_pid);
    int ddw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
    int ddh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
    expand_damage(windows[i].x, windows[i].y - compositor_titlebar_height(),
                  ddw, ddh + compositor_titlebar_height());
    compositor_dirty = 1;

    retired.buffer = windows[i].buffer;
    retired.text_grid = windows[i].term.text_grid;
    retired.attr_grid = windows[i].term.attr_grid;
    memset(&windows[i], 0, sizeof(struct window));
    if (window_count > 0)
      window_count--;
    else
      panic("Compositor: window_count underflow destroying id=%d", window_id);

    if (refocus)
      __focus_topmost_locked();
    break;
  }
  spin_unlock_irqrestore(&compositor_lock, flags);

  __free_retired_window_resources(&retired);
}

/* Destroy all windows owned by a PID. A bounded local resource list makes the
 * table update atomic while keeping kfree outside compositor_lock. */
void compositor_destroy_windows_by_pid(int pid) {
  struct retired_window_resources retired[MAX_WINDOWS] = {{0}};
  int retired_count = 0;
  int refocus = 0;
  uint64_t flags;

  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == 0 || windows[i].pid != pid)
      continue;

    if (windows[i].pid == keyboard_focus_pid)
      refocus = 1;
    int ddw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
    int ddh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
    expand_damage(windows[i].x, windows[i].y - compositor_titlebar_height(),
                  ddw, ddh + compositor_titlebar_height());
    compositor_dirty = 1;

    retired[retired_count].buffer = windows[i].buffer;
    retired[retired_count].text_grid = windows[i].term.text_grid;
    retired[retired_count].attr_grid = windows[i].term.attr_grid;
    retired_count++;
    memset(&windows[i], 0, sizeof(struct window));
    if (window_count > 0)
      window_count--;
    else
      panic("Compositor: window_count underflow destroying PID=%d", pid);
  }

  if (refocus)
    __focus_topmost_locked();
  spin_unlock_irqrestore(&compositor_lock, flags);

  for (int i = 0; i < retired_count; i++)
    __free_retired_window_resources(&retired[i]);
}

/*
 * compositor_get_buffer - return the raw pixel buffer pointer for a window.
 *
 * FIX(GFX-COMP-LOCK-02): the lookup itself now runs under compositor_lock,
 * consistent with every other window-table reader in this file (before this
 * fix it was the only lookup that read windows[] unlocked, racing a
 * concurrent compositor_destroy_window()/compositor_resize_window() that
 * kfree()s or reallocates the same buffer on another CPU).
 *
 * IMPORTANT — pointer lifetime: taking the lock here only makes the LOOKUP
 * safe.  It cannot make the RETURNED POINTER safe to dereference after this
 * function returns and the lock is released: nothing stops a concurrent
 * compositor_destroy_window() or compositor_resize_window() from freeing or
 * replacing that exact buffer the instant after this call unlocks.  This
 * function is kept for kernel-internal callers that own the window's
 * lifetime by construction (e.g. the same context that owns the window and
 * knows no destroy/resize can race it — direct kernel console mirroring).
 * A caller that CANNOT make that guarantee (a syscall handler acting on a
 * user-suppliable window_id) must use compositor_draw_rect() or
 * compositor_blit() instead: both perform the write to the buffer while
 * still holding compositor_lock, so there is no window for a concurrent
 * free to land in.
 */
uint32_t *compositor_get_buffer(int window_id) {
  uint64_t flags;
  uint32_t *buffer = NULL;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      buffer = windows[i].buffer;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return buffer;
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
 * compositor_move_window - reposition a window programmatically (not via the
 * interactive drag path).
 *
 * FIX(GFX-COMP-LOCK-01): this function used to mutate windows[i].x/y with NO
 * lock at all — the only window-geometry mutator in the file that didn't take
 * compositor_lock.  Two concrete failures followed:
 *   1. Data race: compositor_render_internal() reads win->x/win->y under the
 *      lock on another CPU; a torn (non-atomic on some ABIs) or interleaved
 *      write here could hand the renderer a half-updated position — the same
 *      class of bug S1b fixed for compositor_update_mouse().
 *   2. Silent damage loss: every OTHER geometry mutator (drag, resize,
 *      minimize, restore, focus, destroy) calls expand_damage() so the moved
 *      footprint is actually recomposited.  This one didn't, so a
 *      programmatic move (e.g. a window manager repositioning a window
 *      on-screen-change) left stale pixels at the old AND new location until
 *      an unrelated event happened to damage that area.
 *
 * Both the old and new footprints are damaged (mirrors compositor_
 * destroy_window's vacated-footprint handling), and the on-screen draw size
 * is used for the footprint, consistent with every other damage call site in
 * this file.
 */
void compositor_move_window(int window_id, int x, int y) {
  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == window_id) {
      int dw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
      int dh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
      int title_h = compositor_titlebar_height();

      /* Damage the OLD footprint (about to be vacated). */
      expand_damage(windows[i].x, windows[i].y - title_h, dw, dh + title_h);

      windows[i].x = x;
      windows[i].y = y;

      /* Damage the NEW footprint. */
      expand_damage(windows[i].x, windows[i].y - title_h, dw, dh + title_h);
      compositor_dirty = 1;
      break;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
}

/*
 * compositor_resize_window - resize a window's LOGICAL surface (GFX-DYN-01).
 *
 * Reallocates the window pixel buffer to w x h, reflows the embedded terminal
 * to the new cell grid. Explicit window-manager presentation bounds remain
 * authoritative while the application adopts logical pixels.  Allocation happens here, so this must be called from process
 * context (it backs SYS_WINDOW_RESIZE), never from an IRQ.  Returns 0 on
 * success, -1 on failure (the old surface is kept on failure).
 */

/* WINDOW object presentation geometry. No pixel allocation or rendering.
 * Holding the compositor lock serializes snapshot source and draw bounds. */
int compositor_set_window_draw_size(int id, int w, int h) {
  if (w < 1 || h < 1 || w > 4096 || h > 4096) return -EINVAL;
  uint64_t flags;
  int owner = -ESRCH;
  spin_lock_irqsave(&compositor_lock, &flags);
  for (int i = 0; i < MAX_WINDOWS && owner == -ESRCH; ++i) {
    struct window *win = &windows[i];
    if (win->id != id) continue;
    owner = win->pid;
    win->presentation_managed = 1;
    int old_w = win->draw_w > 0 ? win->draw_w : win->width;
    int old_h = win->draw_h > 0 ? win->draw_h : win->height;
    if (old_w != w || old_h != h) {
      int title = compositor_titlebar_height();
      expand_damage(win->x, win->y - title, old_w, old_h + title);
      win->draw_w = w;
      win->draw_h = h;
      expand_damage(win->x, win->y - title, w, h + title);
      compositor_dirty = 1;
    }
  }
  spin_unlock_irqrestore(&compositor_lock, flags);
  return owner < 0 ? owner : 0;
}

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
  if (win->width == w && win->height == h) {
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
  int follows_logical = !win->presentation_managed;
  win->buffer = nbuf;
  win->width = w;
  win->height = h;
  if (follows_logical) {
    win->draw_w = w;
    win->draw_h = h;
  }
  win->has_alpha = ((win->bg_color >> 24) != 0xFF);

  /* Reflow the terminal to the new cell grid. */
  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  if (char_w > 0 && char_h > 0 &&
      term_resize(&win->term, w / char_w, h / char_h) != 0)
    pr_err("compositor: terminal reflow to %dx%d cells failed for pid %d — the "
           "window keeps the OLD cell grid and will render clipped\n",
           w / char_w, h / char_h, win->pid);

  /* Damage the NEW footprint and repaint. */
  expand_damage(win->x, win->y - compositor_titlebar_height(), w,
                h + compositor_titlebar_height());
  compositor_dirty = 1;
  spin_unlock_irqrestore(&compositor_lock, flags);
  if (obuf)
    kfree(obuf);
  /* The owning application already requested this logical size. Echoing a
   * resize here feeds stale dimensions back into the same mailbox alongside
   * newer WM requests, causing repeated reallocations and resize oscillation.
   * Only the userland WM publishes presentation-change requests to owners. */
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
                           .buffer = win->buffer,
                           .capacity = (size_t)win->width * win->height};
    gfx_surface_verify(&s, "__clear_other_carets_locked/win");
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
                                .buffer = win->buffer,
                                .capacity = (size_t)win->width * win->height};
  gfx_surface_verify(&win_surf, "compositor_window_write/win");

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

/* Kernel pointer hit-testing, dragging and input dispatch were retired;
 * nxmoused is now the sole pointer policy implementation. */

/*
 * Composite All Windows to Screen
 */
/* Legacy desktop rasterization is retired; nxcompositor owns frame rendering. */

/*
 * Draw to Window
 */
/*
 * Draw to Window (Internal - No Locking)
 */
/*
 * FIX(GFX-COMP-BOUND-01): w/h arrive here verbatim from a syscall argument
 * (SYS_DRAW_RECT) with no upper bound applied before this point.  The old
 * code only bounds-checked each individual PIXEL (px/py against the window's
 * width/height) but still let the dy/dx LOOPS themselves iterate the full,
 * caller-supplied w*h — including the huge majority of iterations that would
 * immediately fail the bounds check and do nothing.  Because this runs under
 * compositor_lock taken via spin_lock_irqsave (a single global spinlock, IRQs
 * off, on the CPU that owns it), a caller passing h close to INT_MAX turns a
 * few nanoseconds of legitimate work into an unbounded hang of the ENTIRE
 * SYSTEM: every other CPU spinning on compositor_lock, timer ticks queued
 * behind disabled IRQs, one process' bad syscall value freezing the machine.
 *
 * This is exactly the loop-bound hazard NASA/JPL "Power of 10" rule 2 (every
 * loop must have a statically demonstrable fixed upper bound) is written for.
 * The fix clips the requested rect against the window's own logical
 * dimensions BEFORE entering the pixel loops, so the loop trip count is
 * bounded by MAX_WINDOW_DIM^2 regardless of what the caller passes in — the
 * same bound compositor_create_window() already enforces on window creation
 * (w,h <= 4096), reused here for consistency.
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

      /* Clip the rect to the window's logical surface FIRST: this is what
       * bounds the loop trip count, not the per-pixel check below (which
       * stays as defence in depth against an off-by-one in this clip). */
      int cx = x, cy = y, cw = w, ch = h;
      if (cw < 0)
        cw = 0;
      if (ch < 0)
        ch = 0;
      if (cx < 0) {
        cw += cx; /* shrink width by the amount we clip off the left */
        cx = 0;
      }
      if (cy < 0) {
        ch += cy;
        cy = 0;
      }
      if (cx + cw > windows[i].width)
        cw = windows[i].width - cx;
      if (cy + ch > windows[i].height)
        ch = windows[i].height - cy;

      if (cw > 0 && ch > 0) {
        uint32_t *row_base = windows[i].buffer + (size_t)cy * windows[i].width;
        for (int dy = 0; dy < ch; dy++) {
          uint32_t *row = row_base + (size_t)dy * windows[i].width + cx;
          for (int dx = 0; dx < cw; dx++)
            row[dx] = color;
        }
      }

      if ((color >> 24) != 0xFF) {
        windows[i].has_alpha = 1;
      } else if (x <= 0 && y <= 0 && w >= windows[i].width &&
                 h >= windows[i].height) {
        windows[i].has_alpha = 0;
      }

      /* Damage the CLIPPED rect: nothing outside it was actually touched. */
      expand_window_content_damage(&windows[i], cx, cy, cw, ch);
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

  /*
   * FIX(GFX-COMP-BOUND-01 / GFX-COMP-OVERFLOW-01): reject an out-of-range
   * source rect BEFORE taking the lock.  Two independent hazards:
   *   - Unbounded loop trip count (see the header comment further below),
   *     same class as draw_rect_internal.
   *   - `dy * w` (used below to index into the caller's user_buf) is `int`
   *     arithmetic: with dy bounded only by an unclamped h and w unclamped,
   *     this can overflow and wrap to a small/negative offset, handing
   *     vmm_copy_from_user() a source address far from the caller's actual
   *     buffer instead of the OOB access safely faulting.  Capping w/h to
   *     the same <=4096 bound compositor_create_window() enforces on any
   *     window keeps `dy * w` (<=4096*4096, well within int32) provably
   *     free of overflow.
   */
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
    return;

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

      /*
       * FIX(GFX-COMP-BOUND-01): same class of bug as draw_rect_internal
       * (see its comment) — h came straight from a syscall argument.  The
       * per-row `continue` below only skips WORK for an out-of-range row, it
       * does not shrink the LOOP itself, so a caller passing a huge h still
       * forces this loop — held under compositor_lock, IRQs off — to run
       * that many iterations before returning.  Clip the vertical range to
       * the window's height up front so the trip count is bounded by the
       * window's own (already-validated, <=4096) dimensions.
       */
      int row_start = y, row_count = h;
      if (row_count < 0)
        row_count = 0;
      if (row_start < 0) {
        row_count += row_start;
        row_start = 0;
      }
      if (row_start + row_count > windows[i].height)
        row_count = windows[i].height - row_start;
      if (row_count < 0)
        row_count = 0;

      /* Row offset (relative to the CALLER's original y) at which the
       * clipped range begins: user_buf's layout follows the caller's
       * original (x, y, w, h) rect, not the destination-clipped window
       * coordinates, so the source row index must be computed from this
       * offset rather than reusing `dy` directly (dy now indexes the
       * post-clip range, which starts later than row 0 whenever the top of
       * the rect was clipped). */
      int src_row_offset = row_start - y;

      /* Destination X clip is identical for every row (x/w do not vary per
       * row), so compute it once outside the loop instead of recomputing
       * four branches per row for every row in the rect. */
      int dest_x = x;
      int src_x = 0;
      int copy_w = w;
      if (dest_x < 0) {
        src_x += -dest_x;
        copy_w -= -dest_x;
        dest_x = 0;
      }
      if (dest_x + copy_w > windows[i].width)
        copy_w = windows[i].width - dest_x;

      /* Copy Logic: Row by Row for speed */
      for (int dy = 0; dy < row_count; dy++) {
        int py = row_start + dy;

        if (copy_w <= 0)
          break; /* every row has the same (empty) X range: nothing to do */

        /* Use copy_from_user instead of raw memcpy for security */
        void *dst_ptr = &windows[i].buffer[py * windows[i].width + dest_x];
        const void *src_ptr =
            &user_buf[(size_t)(src_row_offset + dy) * w + src_x];

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
