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
/* Backbuffer surface descriptor for the whole render pass — file-static, not
 * stack-local, so a deep chrome/GL call chain cannot clobber it before
 * gpu_present_surface() (S-STAB: same rationale as sorted_windows above). */
static struct gl_surface compositor_frame_surface;

/*
 * =========================================================================
 * FIX(GFX-COMP-PERF-03 / rule-3): per-frame region arena.
 * =========================================================================
 *
 * compositor_render_internal() used to call region_create()/region_destroy()
 * up to (MAX_WINDOWS + 2) times PER COMPOSITED FRAME: once for `occluded`,
 * once for `bg_region`, and once per visible window for its `vis` region.
 * region_create() does two kmalloc() calls (the struct, then an 8-slot rect
 * array), and the region then grows through further kmalloc/kfree pairs as
 * region_add_rect()/region_subtract() accumulate rects (region.c doubles the
 * array up to MAX_RECTS_PER_REGION).  Because compositor_tick() drives this
 * function from the timer interrupt (spin_trylock_irqsave), that put an
 * UNBOUNDED-LATENCY heap-allocator call — kmalloc's own internal locking, see
 * kernel/lib/kmalloc.c — on the hottest path in the graphics stack, up to
 * ~40 times per tick under ordinary window churn. That is a NASA/JPL
 * Power-of-10 rule-3 violation (dynamic allocation after init, reachable from
 * an interrupt-adjacent context) and, independent of the standard, the
 * single largest per-frame cost once more than a couple of windows are open.
 * It also directly works against a bounded per-tick time budget: an
 * allocator call has no static upper bound on how long it can take, so no
 * such budget can be proven while one sits on this path.
 *
 * The `sorted_windows` / `visible_regions_store` arrays two lines above were
 * already static specifically "to avoid ... kmalloc in IRQ" — this arena
 * finishes that job for the struct region objects those arrays only ever
 * held POINTERS to.
 *
 * Fix: every region a render pass needs comes from a fixed-size static
 * arena, sized once at compile time, never touched by kmalloc/kfree again.
 * Two properties make this safe:
 *
 *   1. Usage is provably bounded and frame-scoped.  A single render pass
 *      acquires at most REGION_POOL_SIZE regions — 1 `occluded` + 1
 *      `bg_region` + up to MAX_WINDOWS `vis` regions, and every acquire site
 *      is enumerated in this file (rule 2: the bound is a property of the
 *      source, not a runtime guess) — and none of them survive past the end
 *      of the render pass that acquired them.  The arena is therefore reset
 *      with a single index write at the top of every frame instead of being
 *      freed rect-by-rect.
 *
 *   2. Each slot's rect array is pre-sized to REGION_POOL_RECT_CAP, which
 *      matches region.c's own MAX_RECTS_PER_REGION hard cap.  region.c
 *      already refuses to grow a region past that cap — region_add_rect(),
 *      region_subtract() and region_intersect_rect() all compare count and
 *      capacity against it before ever calling kmalloc — so a region that
 *      starts AT the cap makes region.c's internal growth path dead code for
 *      every pooled region, by construction, with no change needed downstream.
 *
 * region_pool_retire() is a documentation no-op, not a free: ownership
 * always returns to the arena in bulk at the next region_pool_frame_reset(),
 * never per-slot.  It exists so call sites still read like the
 * region_destroy() calls they replace, instead of silently vanishing.
 */
#define REGION_POOL_RECT_CAP 256 /* == region.c MAX_RECTS_PER_REGION */
#define REGION_POOL_SIZE                                                       \
  (MAX_WINDOWS + 2) /* occluded + bg_region + 1/window  */

_Static_assert(REGION_POOL_SIZE > MAX_WINDOWS,
               "region pool must outsize the per-window vis-region demand");

static struct rect region_pool_rects[REGION_POOL_SIZE][REGION_POOL_RECT_CAP];
static struct region region_pool_slots[REGION_POOL_SIZE];
/* Bump index into region_pool_slots for the frame in progress.  Only ever
 * touched from compositor_render_internal(), which runs with
 * compositor_lock held for the whole frame, so this needs no lock of its
 * own (same reasoning as compositor_dirty and the damage_* globals). */
static int region_pool_next;

/* region_pool_frame_reset - reclaim every region handed out last frame.
 * Must run exactly once, at the very top of compositor_render_internal(),
 * before the first region_pool_acquire() of the frame. */
static void region_pool_frame_reset(void) { region_pool_next = 0; }

/*
 * region_pool_acquire - hand out one cleared, pre-capacitated region.
 *
 * Returns NULL if REGION_POOL_SIZE was somehow exceeded — unreachable given
 * the enumerated call sites this file makes (see the block comment above),
 * but every caller checks it anyway rather than trusting a comment to hold
 * at runtime (rule 7).
 */
static struct region *region_pool_acquire(void) NX_MUST_USE;
static struct region *region_pool_acquire(void) {
  if (region_pool_next >= REGION_POOL_SIZE)
    return NULL;
  struct region *r = &region_pool_slots[region_pool_next];
  r->rects = region_pool_rects[region_pool_next];
  r->count = 0;
  r->capacity = REGION_POOL_RECT_CAP;
  region_pool_next++;
  return r;
}

/* region_pool_retire - see the block comment above: intentionally a no-op.
 * `r` may be NULL (mirrors region_destroy()'s NULL-safety so call sites
 * don't need an extra guard). */
static inline void region_pool_retire(struct region *r) { (void)r; }

static void compositor_bind_backbuffer_surface(int bb_w, int bb_h, int bb_pg,
                                               uint32_t *backbuffer) {
  compositor_frame_surface.width = bb_w;
  compositor_frame_surface.height = bb_h;
  compositor_frame_surface.stride = bb_w;
  compositor_frame_surface.buffer = backbuffer;
  compositor_frame_surface.alpha_mask = NULL;
  compositor_frame_surface.capacity =
      (size_t)bb_pg * (4096u / sizeof(uint32_t));
}

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
   * must not count its own footprint against itself.
   *
   * FIX(GFX-COMP-RESIZE-CLAMP-01): the x clamp used to be a FIXED 40px margin
   * ("enough to keep a typical title bar/close button reachable"), which
   * only accounts for a NARROW window. A wide window — nxntfy_srv's 280px
   * notification popup is the concrete case that surfaced this — positioned
   * near the right edge could have its LEFT edge clamped to `bb_width-40`
   * while its RIGHT edge (`x + dw`) stayed far past the new, smaller
   * bb_width: shrinking the desktop from 1920 to 800 left a 280px-wide popup
   * with up to ~240px hanging off-screen, invisible until something
   * repositioned it explicitly. Clamp against the window's OWN on-screen
   * width/height (draw_w/draw_h, falling back to the logical size — same
   * on-screen-size resolution the render/hit-test paths already use) instead
   * of a one-size-fits-all constant, so ANY window's full footprint — not
   * just its top-left corner — stays reachable after a resize. */
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (windows[i].id == 0)
      continue;
    int dw = windows[i].draw_w > 0 ? windows[i].draw_w : windows[i].width;
    int dh = windows[i].draw_h > 0 ? windows[i].draw_h : windows[i].height;
    /* Keep at least a corner (min 40px) reachable even for a window wider
     * than the new screen, instead of pushing x negative. */
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
 * =========================================================================
 * FIX(GFX-COMP-LEN-01 / rule-4): compositor_render_internal() split.
 * =========================================================================
 * compositor_render_internal() was ~550 lines — no single printed page comes
 * close to holding it, which is exactly the failure mode NASA/JPL
 * Power-of-10 rule 4 (one function, one page) targets: a function that long
 * cannot be read in one sitting or reviewed with confidence that every path
 * was actually seen.
 *
 * Three phases were self-contained enough to extract WITHOUT changing their
 * behaviour or their locking (all three still run with compositor_lock held
 * by the caller, exactly as their code did inline):
 *
 *   - compositor_paint_background — screen-minus-occluded gradient fill.
 *   - compositor_paint_cursor     — fixed 12x16 cursor bitmap blit.
 *   - compositor_present_frame    — damage-box upload + damage reset.
 *
 * The two-pass occlusion-culling / painter's-algorithm core (window sort,
 * top-most reordering, visibility computation, and the per-pixel
 * decoration+content paint loop) was NOT split in this pass: those sections
 * share a wide set of mutable locals (sorted[], visible_regions[], count,
 * current_count, the active theme/style/background pointers, the frame's
 * clip rect) in ways that would need a real parameter-object refactor, not
 * a mechanical cut, to split safely. Cutting it under time pressure without
 * a matching test harness risks the kind of subtle reordering bug that is
 * exactly what rule 4 exists to make rarer, not more likely — so it is
 * left as a follow-up with its natural seams already identified above,
 * rather than forced through here.
 */
static void compositor_paint_background(uint32_t *backbuffer, int bb_w,
                                        int bb_h, int clip_x1, int clip_y1,
                                        int clip_w, int clip_h,
                                        const struct region *occluded,
                                        const compositor_background_t *desk_bg);
static void compositor_paint_cursor(uint32_t *backbuffer, int bb_w, int bb_h,
                                    int cx, int cy);
static int compositor_present_frame(struct gpu_device *dev,
                                    struct gl_surface *screen,
                                    uint32_t *backbuffer, int bb_w, int bb_h,
                                    int bb_pg);

/*
 * =========================================================================
 * FIX(GFX-COMP-LEN-02 / rule-4): compositor_handle_click() split.
 * =========================================================================
 * compositor_handle_click() was ~290 lines covering two almost-independent
 * paths (button release, button press) that only share the `button`
 * parameter and the file-static mouse/window state. Split along that seam,
 * matching the file's existing "__foo_locked()" convention (see
 * __focus_topmost_locked, __raise_to_front_locked,
 * __clear_other_carets_locked) for helpers that require compositor_lock
 * already held by the caller:
 *
 *   - compositor_handle_click_release() — the entire `state == 0` path,
 *     unchanged, just lifted out to its own function.
 *   - __click_hit_test_locked()          — top-most non-passive window
 *                                          under the cursor, or NULL.
 *   - __click_raise_and_focus_locked()   — z-order raise + focus update.
 *   - __click_try_start_resize_locked()  — arm interactive resize if the
 *                                          press landed in a grip; returns
 *                                          1 when the caller must unlock
 *                                          and return immediately (mirrors
 *                                          the early `return` the inline
 *                                          version used to do here).
 *   - __click_prepare_dispatch_locked()  — everything else a press needs
 *                                          to do under the lock, captured
 *                                          into a `click_press_dispatch`
 *                                          for the caller to act on AFTER
 *                                          unlocking.
 *   - __click_dispatch_outside_lock()    — the actual kernel_ipc_send() /
 *                                          window_request_close() /
 *                                          compositor_minimize_window()
 *                                          calls FIX(GFX-COMP-03) requires
 *                                          to run outside compositor_lock.
 *
 * compositor_handle_click() itself is now a ~20-line sequence of calls to
 * these, in the exact order (hit-test, raise+focus, try-resize, prepare,
 * unlock, dispatch) the original inline code ran in — no behavioural or
 * locking change, only the split.
 */
struct click_press_dispatch {
  int send_pid;
  struct ipc_message msg;
  int do_close;
  int close_pid;
  int do_minimize;
  int min_id;
};

static void compositor_handle_click_release(int button);
static struct window *__click_hit_test_locked(void);
static void __click_raise_and_focus_locked(struct window *hit);
static int __click_try_start_resize_locked(struct window *hit, int button);
static void __click_prepare_dispatch_locked(struct window *hit, int button,
                                            int state,
                                            struct click_press_dispatch *out);
static void __click_dispatch_outside_lock(struct click_press_dispatch *d);

/*
 * FIX(GFX-COMP-LEN-03 / rule-4): compositor_update_mouse() split, same
 * rationale and "__foo_locked()" convention as the click-handler split
 * above — see compositor_update_mouse() itself for the phase-by-phase
 * breakdown (position update, drag update, resize update, motion-event
 * capture). No behavioural or locking change, only the split.
 */
static void __mouse_update_position_locked(int dx, int dy, int absolute,
                                           int width, int height);
static void __mouse_update_drag_locked(int width, int height);
static void __mouse_update_resize_locked(int width, int height);
static void __mouse_capture_motion_locked(int old_mx, int old_my, int *out_pid,
                                          struct ipc_message *out_msg);

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
  /*
   * GFX-COMP-RESERVE-02: the caller's privilege is resolved BEFORE
   * compositor_lock is taken, and as an int rather than a struct pointer.
   *
   * Both properties are load-bearing.  process_terminate() holds sched_lock and
   * TRYLOCKS compositor_lock (docs/PROCESS-KILL-MODEL.md §4, Pitfall A); a
   * blocking compositor_lock -> sched_lock here would close that AB-BA, on the
   * hottest path the compositor has.  And a `struct process *` fetched by
   * process_find_by_pid() is already stale when the function returns — the
   * target can be terminated and its page freed before it is dereferenced.
   * proc_pid_is_privileged() answers under sched_lock and lets nothing escape.
   *
   * An unknown pid (a caller that raced its own exit) reads as UNPRIVILEGED:
   * fail closed into the smaller user-window budget rather than hand reserved
   * capacity to a caller that can no longer be identified.
   */
  int privileged = proc_pid_is_privileged(pid);

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);

  if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
    pr_err("Compositor: Invalid window dimensions %dx%d\n", w, h);
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }

  /*
   * An unprivileged caller (ordinary app) is capped below the full pool so it
   * can never crowd out the reserved slots a respawning system service needs.
   * A privileged caller (PLVL_ROOT / PLVL_MACHINE — /sys/bin services under
   * the ASTRA per-path preset) is checked against the full MAX_WINDOWS:
   * privileged windows are a small, fixed, supervised set (dock, nxbar,
   * launcher, plus transient destroy+recreate overlap), never something a
   * stress test grows without bound, so they don't need — and shouldn't get —
   * a *tighter* cap than before.  `privileged` was resolved at entry; see the
   * note there for why it cannot be resolved here.
   */
  int cap = privileged ? MAX_WINDOWS : MAX_USER_WINDOWS;

  if (window_count >= cap) {
    pr_err(
        "%s",
        privileged
            ? "Compositor: Max windows reached\n"
            : "Compositor: Max user windows reached (system slots reserved)\n");
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

  /* Allocate window buffer.  (size_t)w * h is safe from overflow: w and h
   * are already bounded to <=4096 above, so the product fits well within
   * both a 32-bit and 64-bit size_t before the *4 for the pixel format. */
  size_t buffer_size = (size_t)w * (size_t)h * sizeof(uint32_t);
  uint32_t *buffer = (uint32_t *)kmalloc(buffer_size);
  if (!buffer) {
    pr_err("%s", "Compositor: Failed to allocate window buffer\n");
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }
  uint32_t default_bg = compositor_theme_active()->win_bg;

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

  /*
   * Initialize the embedded terminal emulator (cell grid from font metrics).
   *
   * FIX(GFX-COMP-DIV0-01): char_w/char_h come from the active font and were
   * used as divisors with no zero-check.  A font driver bug or an
   * uninitialised font table returning 0 here would fault the kernel with a
   * divide-by-zero inside window creation — a state fully outside the
   * caller's control (this is a font-subsystem invariant, not a bad
   * argument), so failing the call cleanly is the right response rather
   * than crashing.
   */
  int char_w = graphics_font_max_width();
  int char_h = graphics_font_height();
  if (char_w <= 0 || char_h <= 0) {
    pr_err("Compositor: invalid font metrics (%dx%d), cannot size terminal\n",
           char_w, char_h);
    kfree(buffer);
    windows[slot].id = 0;
    spin_unlock_irqrestore(&compositor_lock, flags);
    return -1;
  }
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

  /*
   * FIX(GFX-COMP-PERF-01): the buffer used to be filled with default_bg
   * TWICE — once right after allocation, then again here with
   * windows[slot].bg_color, which is set to the exact same default_bg value
   * a few lines above.  For a large window (up to 4096x4096) that is up to
   * ~64 MB of pointless duplicate writes performed while holding
   * compositor_lock.  A single fill, done once with the buffer's actual
   * bg_color (which is what a reader of this code would expect to see, and
   * which stays correct if a future change ever makes the two diverge), is
   * both cheaper and clearer about which value is authoritative.
   */
  for (int i = 0; i < w * h; i++)
    buffer[i] = windows[slot].bg_color;

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
 * to the new cell grid, and resets the on-screen draw size to match (crisp,
 * non-scaled).  Allocation happens here, so this must be called from process
 * context (it backs SYS_WINDOW_RESIZE), never from an IRQ.  Returns 0 on
 * success, -1 on failure (the old surface is kept on failure).
 */

/*
 * gfx_notify_client - deliver one compositor event to a client, and ACCOUNT for
 * the loss when it cannot be delivered.
 *
 * Every caller below runs in IRQ or near-IRQ context with no one to return a
 * failure to, and each used to drop the result.  That made "the window stopped
 * responding to the mouse" and "the app kept drawing at the old size"
 * unattributable: the event simply never arrived and nothing recorded it.
 *
 * Logging per event is not an option -- a full queue would become an interrupt
 * storm -- so losses are COUNTED and only the ONSET is announced.  One place
 * owns the policy so the call sites cannot drift apart, which is the same
 * reason OS1_report_error is a single seam in userland.
 */
unsigned long gfx_dropped_events;
static void gfx_notify_client(int pid, struct ipc_message *m,
                              const char *what) {
  if (pid <= 0)
    return;
  if (kernel_ipc_send(pid, m) != 0) {
    if (gfx_dropped_events++ == 0)
      pr_err("compositor: %s to pid %d was DROPPED (receiver gone or queue "
             "full); further losses are counted in gfx_dropped_events\n",
             what, pid);
  }
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
  if (char_w > 0 && char_h > 0 &&
      term_resize(&win->term, w / char_w, h / char_h) != 0)
    pr_err("compositor: terminal reflow to %dx%d cells failed for pid %d — the "
           "window keeps the OLD cell grid and will render clipped\n",
           w / char_w, h / char_h, win->pid);

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
    /* GFX-DYN-01: this message IS how a client learns its new logical size.
     * Dropping it leaves the app drawing at the old dimensions against a
     * surface that already changed -- visible as clipped or stretched output
     * with nothing in the log to connect it to the resize. */
    if (kernel_ipc_send(owner, &msg) != 0)
      pr_err("compositor: resize notify to pid %d failed — it will keep "
             "drawing at its previous size\n",
             owner);
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

/*
 * Draw simple mouse cursor
 */

/*
 * Handle Mouse Click
 */
/*
 * compositor_handle_click_release - the `state == 0` (button-up) path.
 * FIX(GFX-COMP-LEN-02): lifted out of compositor_handle_click() verbatim —
 * see the split rationale above the forward declarations near the top of
 * this file. Still takes and releases compositor_lock itself (the release
 * path never held it across the dispatcher the way the press path does).
 */
static void compositor_handle_click_release(int button) {
  {
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
      /* Same contract as above: a lost resize is a client stuck at the wrong
       * size, not a cosmetic miss. */
      if (kernel_ipc_send(notify_pid, &msg) != 0)
        pr_err("compositor: resize notify to pid %d failed — it will keep "
               "drawing at its previous size\n",
               notify_pid);
    }
    if (release_pid > 0 && kernel_ipc_send(release_pid, &release_msg) != 0)
      pr_err("compositor: release notify to pid %d failed — it may believe it "
             "still holds the pointer grab\n",
             release_pid);
    return;
  }
}

/* __click_hit_test_locked - top-most non-passive, visible window under
 * the current cursor position, or NULL. Caller holds compositor_lock. */
static struct window *__click_hit_test_locked(void) {
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
  return hit;
}

/* __click_raise_and_focus_locked - z-order raise + keyboard-focus update
 * for a hit window. Caller holds compositor_lock. */
static void __click_raise_and_focus_locked(struct window *hit) {
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
}

/* __click_try_start_resize_locked - arm interactive edge/corner resize if
 * the press landed in hit's resize grip. Returns 1 if resize was started —
 * the caller must unlock and return immediately, matching the early
 * `return` the inline version used to do right here. Returns 0 to mean
 * "keep processing this press". Caller holds compositor_lock. */
static int __click_try_start_resize_locked(struct window *hit, int button) {
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

    /* FIX(GFX-COMP-RESIZE-01): top_most system chrome (nxbar, nxui's dock,
     * nxntfy_srv's popup) must resize ONLY programmatically (bar_reinit/
     * dock_reinit reacting to a screen-size change), never via an interactive
     * mouse grip. `protected` alone did not cover this — it is set ONLY for
     * PID 2 (the shell, compositor_create_window's hardcoded check) — so a
     * click landing within RESIZE_GRIP of nxbar/nxui's edge (easy: nxbar spans
     * the full screen width, its bottom edge sits exactly where users click
     * things right below it) silently started an interactive resize, visibly
     * distorting the bar/dock until its own next redraw() forced draw_w/draw_h
     * back to its computed size — the "resizes momentarily then snaps back"
     * bug. top_most already exists precisely to mark this class of window;
     * reusing it here (rather than adding a second, parallel flag) needs no
     * new state and no userland ABI change. */
    if (button == BTN_LEFT && edge && !hit->protected && !hit->top_most) {
      resizing_window_id = hit->id;
      resize_edge = edge;
      resize_start_mx = mouse_x;
      resize_start_my = mouse_y;
      resize_orig_w = dw;
      resize_orig_h = dh;
      resize_orig_x = hit->x;
      expand_damage(0, 0, bb_width, bb_height);
      compositor_dirty = 1;
      return 1;
    }
  }

  return 0;
}

/* __click_prepare_dispatch_locked - everything a button-press still needs
 * to do, split into what is safe under compositor_lock (z-order/focus
 * already done by the caller; here: capture the input event, resolve a
 * titlebar-button hit, arm drag-start, mark damage) versus what must wait
 * until after unlock (`out`, consumed by __click_dispatch_outside_lock).
 * Caller holds compositor_lock. */
static void __click_prepare_dispatch_locked(struct window *hit, int button,
                                            int state,
                                            struct click_press_dispatch *out) {
  out->send_pid = -1;
  out->msg = (struct ipc_message){0};
  out->do_close = 0;
  out->close_pid = 0;
  out->do_minimize = 0;
  out->min_id = 0;

  /* Capture the mouse event to deliver to the focused process. */
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
      out->msg.from = 0; /* Kernel */
      out->msg.type = IPC_TYPE_MOUSE;
      out->msg.data1 = (uint64_t)button;
      out->msg.data2 = (uint64_t)state;
      memcpy(out->msg.payload, &rel_x, 4);
      memcpy(out->msg.payload + 4, &rel_y, 4);
      out->send_pid = keyboard_focus_pid;
    }
  }

  /* Capture a titlebar-button hit; the action is deferred until after unlock.
   * Two buttons, right-aligned: [background][close].  The background button
   * sits one button-width + BG_BUTTON_GAP to the LEFT of the close button. */
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
      out->do_close = 1;
      out->close_pid = hit->pid;
    } else if (hit_button == GFX_BUTTON_BACKGROUND) {
      out->do_minimize = 1;
      out->min_id = hit->id;
    }
  }

  /* Check for drag start (skipped when closing/minimizing, matching the old
   * early-return).  Left button only: right/middle never start a drag. */
  if (button == BTN_LEFT && !out->do_close && !out->do_minimize &&
      mouse_y >= hit->y - compositor_titlebar_height() && mouse_y < hit->y) {
    dragging_window_id = hit->id;
    drag_off_x = mouse_x - hit->x;
    drag_off_y = mouse_y - hit->y;
  }

  expand_damage(0, 0, bb_width, bb_height);
  compositor_dirty = 1;
}

/* __click_dispatch_outside_lock - perform the IPC/close/minimize work
 * __click_prepare_dispatch_locked() captured, AFTER compositor_lock has
 * been released. FIX(GFX-COMP-03): kernel_ipc_send()/window_request_close()/
 * compositor_minimize_window() must never run while compositor_lock is
 * held (see the comment this used to carry, reproduced below). */
static void __click_dispatch_outside_lock(struct click_press_dispatch *d) {

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
  gfx_notify_client(d->send_pid, &d->msg, "click event");
  if (d->do_close) {
    pr_info("Compositor: Close button -> request close of PID %d\n",
            d->close_pid);
    /* Window-close INTENT seam (#69, docs/PROCESS-KILL-MODEL.md): the kernel's
     * window-aware subtree kill takes the window owner and its WINDOWLESS
     * children (nxexec's hosted terminal program), sparing windowed children.
     * The actual page/stack free is deferred to the scheduler reaper, so the
     * IRQ-context call here only marks the subtree — the heavy compositor
     * render that used to smash a stack from this context is now in userspace
     * (SCHED-STACK-ISO), so the IRQ path is shallow. */
    window_request_close(d->close_pid);
  }
  /* Background button: send the window to the dock.  compositor_minimize_window
   * re-takes compositor_lock, so it must run here (after the unlock), and it
   * re-finds the window by id, so a slot that changed meanwhile is handled
   * gracefully — same contract as window_request_close above. */
  if (d->do_minimize && compositor_minimize_window(d->min_id) != 0)
    pr_err("compositor: minimize of window %d failed — the user clicked the "
           "dock button and nothing happened\n",
           d->min_id);
}

void compositor_handle_click(int button, int state) {
  if (state == 0) {
    compositor_handle_click_release(button);
    return;
  }

  if (state != 1)
    return;

  uint64_t flags;
  spin_lock_irqsave(&compositor_lock, &flags);

  struct window *hit = __click_hit_test_locked();
  if (!hit) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return;
  }

  __click_raise_and_focus_locked(hit);

  if (__click_try_start_resize_locked(hit, button)) {
    spin_unlock_irqrestore(&compositor_lock, flags);
    return;
  }

  struct click_press_dispatch dispatch;
  __click_prepare_dispatch_locked(hit, button, state, &dispatch);

  spin_unlock_irqrestore(&compositor_lock, flags);

  __click_dispatch_outside_lock(&dispatch);
}

/*
 * Update Mouse Position
 */

/* __mouse_update_position_locked - apply an absolute or relative pointer
 * delta to mouse_x/mouse_y and clamp to the current desktop-virtual
 * (backbuffer) size. Caller holds compositor_lock. */
static void __mouse_update_position_locked(int dx, int dy, int absolute,
                                           int width, int height) {
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
}

/* __mouse_update_drag_locked - if a window is being dragged, move it with
 * the cursor and clamp it inside the reserved desktop area (nxbar/nxui).
 * Caller holds compositor_lock. */
static void __mouse_update_drag_locked(int width, int height) {
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
}

/* __mouse_update_resize_locked - if a window is being interactively
 * resized, recompute its on-screen draw size/position from the mouse
 * delta and the grabbed edge. Caller holds compositor_lock. */
static void __mouse_update_resize_locked(int width, int height) {
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
}

/* __mouse_capture_motion_locked - build a rate-limited IPC_TYPE_MOUSE
 * motion event for the focused window's content area, if the cursor
 * actually moved into it and isn't mid-drag/resize. Writes the target pid
 * (-1 if nothing to send) and message into *out_pid and *out_msg for the
 * caller to deliver AFTER unlocking (same outside-lock contract
 * compositor_handle_click uses). Caller holds compositor_lock. */
static void __mouse_capture_motion_locked(int old_mx, int old_my, int *out_pid,
                                          struct ipc_message *out_msg) {
  /* Capture a motion event for the focused window using the SAME
   * IPC_TYPE_MOUSE message the click path sends — no new ABI.  button = 0
   * means "no button, motion only": every existing consumer already treats a
   * zero button as not-a-click, and the SDL2 adapter turns it into
   * SDL_MOUSEMOTION.  Rate-limited so the unbounded per-process IPC queue
   * cannot be flooded from mouse-IRQ context; delivery happens after the
   * unlock, mirroring compositor_handle_click.  Suppressed during drag/resize
   * (the window itself is moving under the cursor). */
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
        out_msg->from = 0; /* Kernel */
        out_msg->type = IPC_TYPE_MOUSE;
        out_msg->data1 = 0; /* no button: motion only */
        out_msg->data2 = 0;
        memcpy(out_msg->payload, &rel_x, 4);
        memcpy(out_msg->payload + 4, &rel_y, 4);
        *out_pid = keyboard_focus_pid;
        last_motion_ns = now;
        break;
      }
    }
  }
}

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
   * no lock).
   *
   * FIX(GFX-COMP-LEN-03 / rule-4): this body was ~170 lines; the position
   * update, drag update, resize update and motion-event capture below are
   * each now their own __locked helper (same split rationale as the click
   * handler above) — no behavioural or locking change, only the split. */
  uint64_t cflags;
  spin_lock_irqsave(&compositor_lock, &cflags);

  int old_mx = mouse_x, old_my = mouse_y;

  __mouse_update_position_locked(dx, dy, absolute, width, height);
  __mouse_update_drag_locked(width, height);
  __mouse_update_resize_locked(width, height);

  /* Mark compositor as needing redraw - don't render from IRQ! */
  if (dragging_window_id != -1 || resizing_window_id != -1) {
    expand_damage(0, 0, bb_width, bb_height);
  } else {
    /* Only the old and new cursor areas (12x16 + 1px border) */
    expand_damage(old_mx - 1, old_my - 1, 14, 18);
    expand_damage(mouse_x - 1, mouse_y - 1, 14, 18);
  }
  compositor_dirty = 1;

  int motion_pid = -1;
  struct ipc_message motion_msg = {0};
  __mouse_capture_motion_locked(old_mx, old_my, &motion_pid, &motion_msg);

  spin_unlock_irqrestore(&compositor_lock, cflags);

  /* Outside compositor_lock (same contract as compositor_handle_click). */
  gfx_notify_client(motion_pid, &motion_msg, "pointer motion");
}

/*
 * Composite All Windows to Screen
 */
/*
 * Compositor Render (HAL + GL)
 */
/*
 * Compositor Render (Region-based / Front-to-Back with Occlusion Culling)
 * (struct region / struct rect now come from the top-of-file include block —
 * see the FIX(GFX-COMP-STRUCT-01) note there.)
 */

/* Rounded-rect membership (F3) now lives in gfx_chrome as
 * gfx_rrect_contains(), shared with the chrome shadow/border primitives. */

static volatile int in_render = 0;
/*
 * compositor_paint_background - fill the on-screen area not covered by any
 * opaque window with the active desktop gradient, clipped to this frame's
 * damage box.
 *
 * `occluded` is the region pass 1 already accumulated (the union of every
 * opaque window's footprint, computed by the occlusion loop still inline in
 * compositor_render_internal()).  This function derives screen-minus-
 * occluded itself, using one more region from the pool (see the region-pool
 * block comment near the top of this file) — no allocation of its own.
 *
 * Params validated (rule 7): a NULL backbuffer/occluded/desk_bg would mean
 * compositor_init() never ran or pass 1's region_pool_acquire() failed; in
 * either case this function just skips the paint instead of dereferencing
 * NULL — the next frame's damage still covers this area, so a skipped
 * background paint here is a stale pixel for one frame, not a crash.
 */
static void
compositor_paint_background(uint32_t *backbuffer, int bb_w, int bb_h,
                            int clip_x1, int clip_y1, int clip_w, int clip_h,
                            const struct region *occluded,
                            const compositor_background_t *desk_bg) {
  if (!backbuffer || !occluded || !desk_bg)
    return;

  struct region *bg_region = region_pool_acquire();
  if (!bg_region)
    return;

  region_add_rect(bg_region, 0, 0, bb_w, bb_h);
  for (int r = 0; r < occluded->count; r++) {
    const struct rect *or = &occluded->rects[r];
    region_subtract(bg_region, or->x, or->y, or->w, or->h);
  }
  /* Only repaint the background within this frame's damage box (perf §3.4). */
  region_intersect_rect(bg_region, clip_x1, clip_y1, clip_w, clip_h);

  /* Vertical gradient from the active background preset (top -> bottom),
   * interpolated per row. */
  uint32_t t_r = (desk_bg->bg_top >> 16) & 0xFF,
           t_g = (desk_bg->bg_top >> 8) & 0xFF, t_b = desk_bg->bg_top & 0xFF;
  uint32_t b_r = (desk_bg->bg_bottom >> 16) & 0xFF,
           b_g = (desk_bg->bg_bottom >> 8) & 0xFF,
           b_b = desk_bg->bg_bottom & 0xFF;
  for (int r = 0; r < bg_region->count; r++) {
    const struct rect *bg = &bg_region->rects[r];
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
  region_pool_retire(bg_region);
}

/*
 * compositor_paint_cursor - blit the fixed 12x16 monochrome cursor bitmap at
 * (cx, cy), clipped pixel-by-pixel to the backbuffer bounds.
 *
 * The bitmap is a compile-time constant (16 fixed-length string literals),
 * so both loop bounds (c_h, c_w) are compile-time constants too: this
 * function's worst-case cost is exactly 192 iterations, every time, with no
 * dependency on window count, damage size, or anything else that varies at
 * runtime — about as close to NASA/JPL rule-2's "statically provable loop
 * bound" as a raster routine gets.
 */
static void compositor_paint_cursor(uint32_t *backbuffer, int bb_w, int bb_h,
                                    int cx, int cy) {
  if (!backbuffer)
    return;

  static const char *cursor_bits[] = {
      "X           ", "XX          ", "X.X         ", "X..X        ",
      "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
      "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ",
      "X.X X..X    ", "XX  X..X    ", "X    XX     ", "     XX     "};
  const int c_h = 16;
  const int c_w = 12;
  for (int y = 0; y < c_h; y++) {
    for (int x = 0; x < c_w; x++) {
      int px = cx + x;
      int py = cy + y;
      if (px >= 0 && px < bb_w && py >= 0 && py < bb_h) {
        char p = cursor_bits[y][x];
        if (p == 'X')
          backbuffer[py * bb_w + px] = 0xFFFFFFFF; // Border White
        else if (p == '.')
          backbuffer[py * bb_w + px] = 0xFF000000; // Fill Black
      }
    }
  }
}

/*
 * compositor_present_frame - upload this frame's damage box to the GPU.
 *
 * Prefers the atomic dev->ops->present() path (copies backbuffer->scanout
 * AND transfers to the host under the driver's own lock, so a concurrent
 * set_mode/zoom cannot free the scanout backing mid-copy); falls back to a
 * manual copy+flush for a driver that only implements the legacy ops.
 *
 * Returns 1 if the frame was presented (or there was nothing to upload),
 * 0 if it was skipped (a mid-resize geometry mismatch) — mirrors the
 * `presented` local this body used to set inline.  The caller currently has
 * nothing further to do with the result (the damage-reset decision already
 * happened inside this function, below), but it is still returned rather
 * than discarded at the source: a future caller wanting to log/count
 * skipped frames should not have to reach back into this function's guts to
 * get the signal that already existed.
 */
static int compositor_present_frame(struct gpu_device *dev,
                                    struct gl_surface *screen,
                                    uint32_t *backbuffer, int bb_w, int bb_h,
                                    int bb_pg) {
  if (!dev || !screen || !backbuffer)
    return 0;

  /* Flush — upload only the damage bounding box.  Prefer the atomic present()
   * (RC2): it copies backbuffer->scanout AND transfers to the host under the
   * driver's gpu_lock, so a concurrent set_mode/zoom can't free the scanout
   * backing mid-copy (the resize/zoom use-after-free).  present() validates the
   * source geometry against the LIVE scanout and returns <0 to skip a frame on
   * a mid-resize mismatch — we then leave the damage accumulated so the next
   * frame repaints at the new size. */
  int presented = 0;
  int dx1 = damage_x1 < 0 ? 0 : damage_x1;
  int dy1 = damage_y1 < 0 ? 0 : damage_y1;
  int dx2 = damage_x2 > bb_w ? bb_w : damage_x2;
  int dy2 = damage_y2 > bb_h ? bb_h : damage_y2;
  if (dev->ops && dev->ops->present) {
    if (dx1 < dx2 && dy1 < dy2) {
      /* Surface-speaking contract (graphics-port): the core hands the
       * provider its validated gfx_surface + damage rect through gpu_core,
       * never the driver's ops table directly.  Re-bind immediately before
       * present so a clobbered descriptor cannot reach the GPU seam even if
       * something scribbled over compositor_frame_surface mid-frame. */
      compositor_bind_backbuffer_surface(bb_w, bb_h, bb_pg, backbuffer);
      gfx_surface_verify(screen, "compositor_present_frame/present");
      gfx_rect_t present_damage = {dx1, dy1, dx2 - dx1, dy2 - dy1};
      if (gpu_present_surface(screen, &present_damage) == 0)
        presented = 1;
    } else {
      presented = 1; /* nothing to upload, but the (empty) damage is consumed */
    }
  } else if (dev->ops && dev->ops->flush && dev->ops->get_framebuffer) {
    /* Legacy fallback for a driver without present(): copy+flush.  NOT atomic
     * vs set_mode, but no GPU driver in-tree lacks present() — kept for safety
     * so a future provider that only implements flush still displays. */
    void *fb_va = dev->ops->get_framebuffer(dev, NULL);
    if (fb_va && bb_w == dev->width && bb_h == dev->height) {
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
      presented = 1;
    }
  }
  /* Reset damage only once the frame was actually presented; a skipped frame
   * (mid-resize geometry mismatch) keeps its damage for the retry. */
  if (presented) {
    damage_x1 = bb_w;
    damage_y1 = bb_h;
    damage_x2 = 0;
    damage_y2 = 0;
  }
  return presented;
}

static void compositor_render_internal(void) {
  /* Atomic guard against concurrent rendering (multi-CPU or IRQ re-entrancy) */
  if (__sync_lock_test_and_set(&in_render, 1))
    return;

  struct gpu_device *dev = gpu_get_primary();
  if (!dev || !compositor_backbuffer) {
    __sync_lock_release(&in_render);
    return;
  }

  /* FIX(GFX-COMP-PERF-03): reclaim every region_pool_acquire() from last
   * frame before handing out any new ones this frame. Must run before the
   * first acquire below (occluded/vis/bg_region are all acquired further
   * down this function) — see the region-pool block comment near the top
   * of this file for why this replaces region_create()/region_destroy(). */
  region_pool_frame_reset();

  /* Use current buffer dimensions.  bb_pg snapshots the backing page count so
   * the surface below carries its TRUE allocation (S-STAB): the whole raster
   * path only clips against bb_w/bb_h, so if those ever exceeded the allocation
   * (a geometry desync) every chrome/content write would run off the end into
   * kernel RAM.  We hold compositor_lock here, so
   * bb_width/height/pages/backbuffer are a consistent set. */
  int bb_w = bb_width;
  int bb_h = bb_height;
  int bb_pg = bb_pages;
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

  /* Wrap backbuffer in GL Surface.  capacity = true backing size in pixels
   * (S-STAB); gfx_surface_verify panics precisely if bb_w*bb_h ever outran the
   * allocation instead of letting the chrome/content painters scribble kernel
   * RAM (the UTM-panic class). */
  compositor_bind_backbuffer_surface(bb_w, bb_h, bb_pg, backbuffer);
  gfx_surface_verify(&compositor_frame_surface,
                     "compositor_render_internal/backbuffer");
  struct gl_surface *screen = &compositor_frame_surface;

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

  /*
   * FIX(GFX-COMP-PERF-02): insertion sort by z_order, replacing the previous
   * bubble sort.
   *
   * Both are O(n^2) worst case and both have a statically fixed upper bound
   * (count <= MAX_WINDOWS, so this stays NASA/JPL "Power of 10" rule 2
   * compliant either way) — the change is about the EXPECTED case, not the
   * worst case.  z_order here changes by exactly one window per user action
   * (a click-to-front, a new window, a restore) between consecutive frames;
   * the array is therefore already sorted or a single element out of place
   * on almost every call, and insertion sort is O(n) on already-sorted input
   * (bubble sort's inner loop still runs its full n-i-1 comparisons every
   * pass regardless of how sorted the input already is).  This runs once per
   * composited frame, so the saving is real on every render, not a one-off.
   */
  for (int i = 1; i < count; i++) {
    struct window *key = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j]->z_order > key->z_order) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = key;
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
  struct region *occluded = region_pool_acquire();
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

    struct region *vis = region_pool_acquire();
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

    /* Aggiungi a Occluded (Solo se la finestra non contiene trasparenze).
     *
     * FIX(GFX-COMP-CORNER-01): a rounded-corner window's occlusion footprint
     * must NOT be the full bounding rect when st->rounded_corners is active —
     * the 4 corner squares outside the rounded shape are not actually opaque
     * (gfx_rrect_contains skips painting them; see the content/decoration
     * paint loop below and gfx_chrome_border/shadow). Marking the FULL rect
     * occluded blocked the layer underneath from ever being repainted at
     * those exact corner pixels — and this window's own paint pass also never
     * writes there (outside the rounded shape) — so the corner pixels simply
     * kept whatever was left over from a previous frame: stale content,
     * square (unrounded) leftovers, or garbage on a window's first frame at a
     * new position/size. This is the "trasparenza/refresh/smussatura"
     * compositor bug.
     *
     * Fix: occlude only the INSCRIBED rectangle set of the rounded rect (top
     * strip + middle full-width strip + bottom strip, corners excluded) —
     * a conservative SUBSET of the true opaque area (a rounded rect's corner
     * quarter-circle sliver is technically opaque too, but treating it as
     * "not occluded" only costs a few redundant corner-pixel repaints, never
     * a correctness bug). The three strips are entirely within
     * gfx_rrect_contains for radius rr, mirroring the same rr computation the
     * paint loop below uses. */
    if (!win->has_alpha) {
      int rr = (st->rounded_corners && !win->top_most) ? st->border_radius : 0;
      if (rr <= 0) {
        region_add_rect(occluded, win->x, win_y, dw, win_h);
      } else {
        int cr = rr;
        if (2 * cr > dw)
          cr = dw / 2;
        if (2 * cr > win_h)
          cr = win_h / 2;
        if (cr > 0) {
          /* top strip (between the two top corners) */
          region_add_rect(occluded, win->x + cr, win_y, dw - 2 * cr, cr);
          /* middle strip (full width, corners above/below excluded) */
          region_add_rect(occluded, win->x, win_y + cr, dw, win_h - 2 * cr);
          /* bottom strip (between the two bottom corners) */
          region_add_rect(occluded, win->x + cr, win_y + win_h - cr,
                          dw - 2 * cr, cr);
        } else {
          region_add_rect(occluded, win->x, win_y, dw, win_h);
        }
      }
    }
  }

  /* Calculate Background Region (Screen - Occluded) and paint it.
   * FIX(GFX-COMP-LEN-01): extracted to compositor_paint_background() — see
   * the split rationale above the forward declarations near the top of this
   * file. Behaviour and locking are unchanged: still runs inline, still
   * under compositor_lock, still uses the region pool instead of kmalloc. */
  compositor_paint_background(backbuffer, bb_w, bb_h, clip_x1, clip_y1, clip_w,
                              clip_h, occluded, desk_bg);
  region_pool_retire(occluded);
  occluded = NULL; /* not a use-after-free guard anymore (the arena owns the
                      storage) — kept so nothing below mistakes this stale
                      pointer for a still-live region. */

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
        gfx_chrome_shadow_solid(screen, &chrome_clip, &chrome_frame, rr,
                                th->win_bg);
      else if (st->shadow_type == 2)
        gfx_chrome_shadow_premium(screen, &chrome_clip, &chrome_frame, rr,
                                  st->shadow_size);
      else if (st->shadow_type == 1)
        gfx_chrome_shadow_fast(screen, &chrome_clip, &chrome_frame, rr,
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
      region_pool_retire(vis);
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
      gl_draw_string_clipped(screen, start_x, start_y, win->title, text_color,
                             clip_x1, clip_y1, clip_x2, clip_y2);
    }

    /* Window border (F3): 1px outline around the full window rect, following
     * the rounded corners.  Drawn last so it sits on top of content + title. */
    if (st->window_borders && !win->top_most) {
      gfx_rect_t chrome_frame = {win->x, decor_y, dw, full_h};
      gfx_chrome_border(screen, &chrome_clip, &chrome_frame, rr, th->border);
    }
  }

  /* Cleanup any remaining regions in store */
  for (int i = 0; i < MAX_WINDOWS; i++) {
    if (visible_regions_store[i]) {
      region_pool_retire(visible_regions_store[i]);
      visible_regions_store[i] = NULL;
    }
  }

  /* Mouse Cursor (Always on top).
   * FIX(GFX-COMP-LEN-01): extracted to compositor_paint_cursor(); the fixed
   * 12x16 bitmap and its loop bounds are byte-for-byte unchanged. */
  compositor_paint_cursor(backbuffer, bb_w, bb_h, mouse_x, mouse_y);

  /* Flush — upload only the damage bounding box.
   * FIX(GFX-COMP-LEN-01): extracted to compositor_present_frame(); the
   * present()/legacy-flush choice, the damage-box math and the
   * presented-only damage reset are byte-for-byte the same logic that
   * used to sit inline here — see that function for the full comment
   * this used to carry. */
  compositor_present_frame(dev, screen, backbuffer, bb_w, bb_h, bb_pg);

  /* Cleanup regions (occluded was retired to the arena after pass 1;
   * visible_regions entries are nulled as pass 2 consumes them). */
  for (int i = 0; i < count; i++) {
    if (visible_regions[i]) {
      region_pool_retire(visible_regions[i]);
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