/*
 * user/sys/bin/nxui.c
 * NEXS dock — the system taskbar (an ASTRA SRL userland service).
 *
 * nxui is a thin userland window manager UI.  It keeps the compositor as the
 * pure MECHANISM and owns the POLICY itself:
 *   - it enumerates every window with SYS_WINDOW_ENUM (OS1_window_enum);
 *   - it draws a macOS/Darwin-style dock along the bottom of the screen, one
 *     tile per app: a real per-app icon (nxicon.h,
 * /home/Pictures/icon/{dark,light}) when one is cached for that app +
 * theme, else the classic flat rounded square (GFX-NXUI-02) — the exact same
 * corner-mask code draws both, see fb_rrect_px();
 *   - a click on a tile FOCUSES that app, RESTORES it if it was sent to the
 *     background, or BACKGROUNDS it if it was the focused app (toggle);
 *   - when more apps are open than fit on screen, two scroll tiles (half-width,
 *     rounded the same way as everything else) appear at the dock's ends and
 *     PAGE through the app list, wrapping around at the last/first page
 *     (GFX-NXUI-04);
 *   - the dock remembers g_last_focus — the last app window that was REALLY
 *     focused (set both by its own act() and by reading the WININFO_FOCUSED
 *     bit each redraw, so it follows focus changes the system makes outside
 *     the dock).  When a frame has no focused app (the dock itself was just
 *     clicked, or a tile was toggled to minimized), the g_last_focus tile
 *     stays painted blue, and a click that did not act on an app (scroll
 *     buttons, empty dock space) hands the focus back via
 *     OS1_window_focus(g_last_focus) — a click on a tile already drives
 *     focus through act() so that path is unchanged.
 *
 * All window control goes through OS1_window_* — i.e. OBJ_TYPE_WINDOW
 * capabilities (ASTRA §6.7): the authority is the unforgeable handle, not
 * nxui's identity.  nxui is spawned by init at PLVL_ROOT (the /sys/bin
 * per-path preset) — window-manager authority is enough to control any
 * app's window, and ROOT keeps the dock killable/respawnable like the shell.
 *
 * Input uses the STANDARD window input API (input_poll_event / input_event_t),
 * exactly like every other windowed app — mouse buttons are the evdev codes
 * MOUSE_BTN_* the compositor forwards, NOT a 0/1/2 index.  The dock is itself a
 * top_most compositor window (chromeless: a top_most window has no
 * titlebar/buttons), drawn by blitting a self-rendered ARGB buffer.  The window
 * is inset from the screen edges (DOCK_MARGIN_SIDE/_BOTTOM) and its own
 * corners are rounded the same way the tiles are (GFX-NXUI-04).
 *
 * It re-lays-out on a desktop resolution change (polled, plus the event-driven
 * INPUT_TYPE_RESIZE path for when the system notifies it), and only re-blits
 * when the dock-relevant state (app set, focus, page, button-press) actually
 * changes — it never busy-spins.
 *
 * NOTE(GFX-NXUI-02): resolved — per-app icons (nxicon.h) now draw over the
 * flat rounded-square tile whenever one is available for the app + active
 * theme; the flat tile itself is unchanged and remains the fallback.
 * NOTE(GFX-NXUI-03): resolved — the per-path VFS capability preset
 * (/sys/bin → PLVL_ROOT) has shipped and is what governs nxui's level today.
 * NOTE(GFX-NXUI-04): rounded dock corners rely on the compositor honoring
 * per-pixel alpha on window_blit — already true for COL_DOCK_BG's 0xE8 alpha,
 * so the fully-transparent (0x00000000) corner pixels here ride the same
 * path. If the compositor ever switches to opaque blits this degrades to
 * square corners with a black fringe, not a crash — worth a quick visual
 * check after dropping this in.
 */
#include <input.h>
#include <os1.h>
#include <string.h>

#include "nxicon.h" /* nxicon_get()/nxicon_classify() — dock tile icons (GFX-NXUI-02) */
#include "nxres.h" /* nxres_theme_is_light(), IPC_LOOK_PING_MAGIC (posix_types.h) — see palette below */

#define DOCK_H 56     /* dock height in px                                   */
#define TILE 40       /* app tile size in px                                 */
#define TILE_GAP 12   /* gap between tiles                                   */
#define TILE_RADIUS 6 /* tile corner radius */
#define MARGIN                                                                 \
  16                 /* inset between the dock window edge and the first/      \
                      * last tile or scroll button (NOT the screen inset —   \
                      * see DOCK_MARGIN_SIDE below)                         */
#define MAX_TILES 32 /* matches the compositor MAX_WINDOWS                  */

/* Outer placement: detach the dock window itself from the screen edges. */
#define DOCK_MARGIN_SIDE 16  /* gap to the left/right screen edges          */
#define DOCK_MARGIN_BOTTOM 4 /* gap to the bottom screen edge               */

/* Scroll (paging) tiles: half the app tiles' width, same height/radius/style.
 */
#define SCROLL_BTN_W (TILE / 2)
#define SCROLL_BTN_GAP 8 /* gap between a scroll button and the tile row */
#define DOCK_RADIUS                                                            \
  12 /* dock outer corner radius — same rounding style as                    \
      * the tiles, just scaled up for the bigger shape   */

/* COL_DOCK_BG/COL_TILE/COL_TILE_MIN are the only dock colours that need to
 * change with theme.color (nxres.h) — background translucency and the idle/
 * minimized tile need different contrast against a light vs dark desktop.
 * Everything else below (focus blue, scroll-button red, launcher green) is
 * an accent/status colour, deliberately theme-INVARIANT — same reasoning as
 * nxbar's g_col_xbtn_glyph/g_col_badge staying fixed. */
static uint32_t g_col_dock_bg;
static uint32_t g_col_tile;
static uint32_t g_col_tile_min;
static int g_light; /* current theme, mirrored here so redraw() can pick the
                     * matching /home/Pictures/icon/{dark,light} set without
                     * a second nxres_theme_is_light() read per frame */

static void nxui_load_palette(int light) {
  g_light = light;
  if (light) {
    g_col_dock_bg = 0xE8F5F5F7u;
    g_col_tile = 0xFFAEAEB2u;
    g_col_tile_min = 0xFFD1D1D6u;
  } else {
    g_col_dock_bg = 0xE81C1C24u;
    g_col_tile = 0xFFD0CBD3u;
    g_col_tile_min = 0xFF555563u;
  }
}

#define COL_TILE_FOCUS 0xFF5E9CFFu
#define COL_BTN_RED 0xFFE53935u     /* material red 600 — idle scroll button */
#define COL_BTN_PRESSED 0xFF6B6B73u /* greys out while the button is held */

/* The /sys/bin/nxlauncher tile is the only non-generic entry in the dock: a
 * solid green when shown (material green 600), and a dimmer, less saturated
 * green when the launcher has been sent to the background.  It mirrors the
 * focus/minimised split used for every other tile (blue/grey) so the dock's
 * own design language stays consistent — only the hue differs. */
#define COL_LAUNCHER 0xFF43A047u     /* material green 600 (shown)       */
#define COL_LAUNCHER_MIN 0xFF2E5D32u /* dimmer green (backgrounded)      */

static uint32_t *g_fb;   /* dock pixel buffer (g_sw x DOCK_H, ARGB)        */
static int g_sw;         /* dock window's OWN width (desktop width minus
                          * the two DOCK_MARGIN_SIDE insets)                */
static int g_dsw, g_dsh; /* last-seen DESKTOP size, used only to detect a
                          * resolution change (distinct from g_sw now that
                          * the dock window is narrower than the desktop)   */
static int g_win = -1;   /* the dock's own window id                       */
static unsigned g_sig;   /* signature of the last drawn state (no re-blit  */
                         /* when unchanged → the dock is never a spinbomb)  */

/* current tile→window mapping for the VISIBLE PAGE, rebuilt every poll for
 * click hit-testing */
static int g_slot_id[MAX_TILES];
static int g_slot_x[MAX_TILES];
static int g_slot_n;

/* paging state */
static int g_page;               /* current page, 0-based                    */
static int g_pages = 1;          /* total pages for the current app count and
                                  * dock width — recomputed every redraw, so it
                                  * always tracks the real number actually used */
static int g_scroll_on;          /* whether the scroll buttons are shown this
                                  * frame (more apps than fit on one page)     */
static int g_btn_l_x, g_btn_r_x; /* dock-local x of the two scroll buttons */
static int g_pressed;            /* 0=none, 1=left, 2=right — currently held  */

/* g_last_focus: id of the most recent app window the dock knows to be really
 * focused.  Updated in two places: (1) every redraw, when WININFO_FOCUSED is
 * observed on some window (so focus changes the system makes outside the
 * dock — e.g. a user clicking into a window directly — are followed); (2) by
 * act() when a tile click focused or restored an app.  When this frame has
 * NO focused app, the g_last_focus tile is still painted blue, and a click
 * that did not act on an app (scroll buttons, empty dock) refocuses it via
 * OS1_window_focus(). 0 means "we have not seen focus yet". */
static int g_last_focus;

static void fb_fill(uint32_t c) {
  int total = g_sw * DOCK_H;
  for (int i = 0; i < total; i++)
    g_fb[i] = c;
}

/* fb_rrect_px - shared rounded-rect body: one corner-quarter-circle mask test,
 * one write loop, feeding EITHER a flat color (img == NULL — the classic
 * GFX-NXUI-02 tile) or an icon's own pixels (img != NULL, w x h == the icon's
 * own dimensions).  Kept as a single function so an icon tile is rounded by
 * the EXACT same test as every other shape in this file (dock panel, classic
 * tiles, scroll buttons) — no second "icon corner rounding" to keep in sync.
 * dim != 0 darkens an icon's RGB (alpha untouched) for the minimized/
 * backgrounded state, replacing the classic path's g_col_tile_min swap. */
static void fb_rrect_px(int x, int y, int w, int h, int r, uint32_t c,
                        const uint32_t *img, int dim) {
  if (w <= 0 || h <= 0)
    return;
  if (r < 0)
    r = 0;
  if (r > w / 2)
    r = w / 2;
  if (r > h / 2)
    r = h / 2;

  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int ccx = -1, ccy = 0;
      if (i < r && j < r) {
        ccx = r;
        ccy = r;
      } else if (i >= w - r && j < r) {
        ccx = w - r - 1;
        ccy = r;
      } else if (i < r && j >= h - r) {
        ccx = r;
        ccy = h - r - 1;
      } else if (i >= w - r && j >= h - r) {
        ccx = w - r - 1;
        ccy = h - r - 1;
      }

      if (ccx >= 0) {
        int dx = i - ccx, dy = j - ccy;
        if (dx * dx + dy * dy > r * r)
          continue;
      }

      int px = x + i;
      int py = y + j;
      if (px < 0 || px >= g_sw || py < 0 || py >= DOCK_H)
        continue;

      uint32_t src;
      if (img) {
        src = img[j * w + i];
        if (dim) {
          uint32_t a = src & 0xFF000000u;
          uint32_t rr = ((src >> 16) & 0xFFu) * 3u / 5u;
          uint32_t gg = ((src >> 8) & 0xFFu) * 3u / 5u;
          uint32_t bb = (src & 0xFFu) * 3u / 5u;
          src = a | (rr << 16) | (gg << 8) | bb;
        }
      } else {
        src = c;
      }

      if (img) {
        uint8_t alpha = (src >> 24) & 0xFF;
        if (alpha == 0)
          continue;

        if (alpha == 255) {
          g_fb[py * g_sw + px] = src;
        } else {
          uint32_t dst = g_fb[py * g_sw + px];

          uint32_t dr = (dst >> 16) & 0xFF;
          uint32_t dg = (dst >> 8) & 0xFF;
          uint32_t db = dst & 0xFF;

          uint32_t sr = (src >> 16) & 0xFF;
          uint32_t sg = (src >> 8) & 0xFF;
          uint32_t sb = src & 0xFF;

          uint32_t out_r = (sr * alpha + dr * (255 - alpha)) / 255;
          uint32_t out_g = (sg * alpha + dg * (255 - alpha)) / 255;
          uint32_t out_b = (sb * alpha + db * (255 - alpha)) / 255;

          g_fb[py * g_sw + px] =
              (0xFF000000u) | (out_r << 16) | (out_g << 8) | out_b;
        }
      } else {
        g_fb[py * g_sw + px] = src;
      }
    }
  }
}

/* fb_rrect - filled rounded rectangle into the dock buffer (corners clipped to
 * a quarter-circle of radius r).  The classic (icon-less) tile/panel/button
 * path — just fb_rrect_px with no image. */
static void fb_rrect(int x, int y, int w, int h, int r, uint32_t c) {
  fb_rrect_px(x, y, w, h, r, c, NULL, 0);
}

/* fb_icon_border - a subtle 3D bevel ring traced around an icon tile only
 * (the classic flat-colour tile already reads as a solid button and doesn't
 * need one). Two 1px arcs split along the tile's diagonal: a brighter one
 * along the top-left edge — the "raised" side, as if lit from above-left —
 * and a dimmer one along the bottom-right edge, the same convention a real
 * embossed bezel uses. White in dark mode (a light ring reads as a raised
 * highlight against a dark tile); black in light mode (mirrored: a dark ring
 * reads as raised against a light tile). Uses the SAME corner-quarter-circle
 * test as fb_rrect_px so the ring hugs the icon's own rounded corners exactly
 * — just restricted to a ~1px band instead of a fill. */
static void fb_icon_border(int x, int y, int w, int h, int r, int light) {
  uint32_t base = light ? 0x000000u : 0xFFFFFFu;
  if (r < 0)
    r = 0;
  if (r > w / 2)
    r = w / 2;
  if (r > h / 2)
    r = h / 2;

  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int ccx = -1, ccy = 0;
      if (i < r && j < r) {
        ccx = r;
        ccy = r;
      } else if (i >= w - r && j < r) {
        ccx = w - r - 1;
        ccy = r;
      } else if (i < r && j >= h - r) {
        ccx = r;
        ccy = h - r - 1;
      } else if (i >= w - r && j >= h - r) {
        ccx = w - r - 1;
        ccy = h - r - 1;
      }

      int on_border;
      if (ccx >= 0) {
        int dx = i - ccx, dy = j - ccy;
        int d2 = dx * dx + dy * dy;
        if (d2 > r * r)
          continue; /* outside the rounded shape entirely */
        int r_in = r - 1;
        on_border = d2 >= r_in * r_in; /* within ~1px of the outer arc */
      } else {
        /* straight edge: only the single outermost row/column is the ring */
        on_border = (i == 0 || i == w - 1 || j == 0 || j == h - 1);
      }
      if (!on_border)
        continue;

      int px = x + i, py = y + j;
      if (px < 0 || px >= g_sw || py < 0 || py >= DOCK_H)
        continue;

      /* top-left half of the tile (by the diagonal i+j) gets the brighter
       * "raised" alpha; bottom-right half gets the dimmer "recessed" one. */
      int top_left = (i + j) < (w + h) / 2;
      uint32_t alpha = top_left ? 0x60u : 0x30u;

      uint32_t dst = g_fb[py * g_sw + px];
      uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
      uint32_t sr = (base >> 16) & 0xFF, sg = (base >> 8) & 0xFF,
               sb = base & 0xFF;

      uint32_t out_r = (sr * alpha + dr * (255 - alpha)) / 255;
      uint32_t out_g = (sg * alpha + dg * (255 - alpha)) / 255;
      uint32_t out_b = (sb * alpha + db * (255 - alpha)) / 255;
      g_fb[py * g_sw + px] =
          (0xFF000000u) | (out_r << 16) | (out_g << 8) | out_b;
    }
  }
}

/* dock_reinit - (re)create the dock window + buffer for a sw x sh DESKTOP,
 * pinned to the bottom but inset by DOCK_MARGIN_SIDE/_BOTTOM on every side.
 * Used at start and on a resolution change (there is no userland window-move
 * primitive yet, so recreating at the new origin is the simplest correct
 * adaptation). */
static void dock_reinit(int sw, int sh) {
  g_dsw = sw;
  g_dsh = sh;

  int dw = sw - 2 * DOCK_MARGIN_SIDE;
  if (dw < TILE + 2 * MARGIN) /* never collapse below one tile's worth */
    dw = TILE + 2 * MARGIN;
  g_sw = dw;

  uint32_t *nb = (uint32_t *)malloc((size_t)g_sw * DOCK_H * 4);
  if (nb) {
    free(g_fb);
    g_fb = nb;
  }
  if (g_win >= 0)
    destroy_window(g_win);
  g_win = create_window(DOCK_MARGIN_SIDE, sh - DOCK_H - DOCK_MARGIN_BOTTOM,
                        g_sw, DOCK_H, "nxui");
  if (g_win >= 0)
    set_window_flags(g_win, 1); /* top_most → chromeless, always-on-top */
  g_sig = 0;                    /* force a redraw at the new size */
}

/* redraw - enumerate windows, page the list to fit the dock, and blit it —
 * but only when the dock-relevant state actually changed (force=1 overrides).
 * Updates g_slot_x[]/g_slot_id[] and g_btn_l_x/g_btn_r_x so clicks hit-test
 * against the live layout.
 *
 * Paging replaces the old hard width cutoff: every eligible window (up to
 * MAX_TILES, the compositor's own cap) is always reachable, just possibly on
 * another page.  g_pages is recomputed from the ACTUAL app count and the
 * dock's current width on every call, so it always reflects exactly how many
 * pages are really in use — never a fixed guess. */
static void redraw(int force) {
  struct window_info wi[MAX_TILES];
  int n = (int)OS1_window_enum(wi, MAX_TILES);
  if (n < 0)
    n = 0;

  /* Pass 1: collect every eligible window, unfiltered by width. */
  int ids[MAX_TILES];
  unsigned flg[MAX_TILES];
  int pids[MAX_TILES];     /* pid per window, for the sys.proc.<pid>.icon lookup
                            * (Phase 3 identity) in the draw loop below */
  char ttl[MAX_TILES][64]; /* title copy, for nxicon_classify() in the draw
                            * loop below — the id/flags this dock already
                            * tracked were never enough to pick an icon */
  int total = 0;
  int seen_focus = 0; /* 1 if some window reports WININFO_FOCUSED */
  for (int i = 0; i < n && total < MAX_TILES; i++) {
    /* our own dock + system overlays (notifications) are never tiled */
    if (wi[i].id == g_win)
      continue;
    if (wi[i].flags & (WININFO_TOPMOST | WININFO_PASSIVE))
      continue;
    /* show real apps: shown windows, or ones the user backgrounded.  Skip a
     * window an app hid itself (hidden but not user-minimized). */
    if (!(wi[i].flags & WININFO_VISIBLE) && !(wi[i].flags & WININFO_MINIMIZED))
      continue;
    ids[total] = wi[i].id;
    flg[total] = wi[i].flags;
    pids[total] = wi[i].pid;
    memcpy(ttl[total], wi[i].title, sizeof(ttl[total]));
    total++;
    /* Follow focus changes the system makes outside the dock (e.g. the user
     * clicks into a window's body).  Only update g_last_focus when we
     * actually see the flag set, so a frame where no app is focused
     * (because the dock just got focus, or a tile was toggled to
     * minimized) keeps the remembered id intact. */
    if (wi[i].flags & WININFO_FOCUSED) {
      g_last_focus = wi[i].id;
      seen_focus = 1;
    }
  }

  /* Pin the launcher to position 0 so it is always the first tile of the
   * first page (no paging needed to reach it).  Matched by basename so any
   * future title suffix — e.g. "— folder view" — still pins.  Done after
   * the collection pass so the focus update above still fires naturally. */
  int launcher_id = 0;
  for (int i = 0; i < total; i++) {
    for (int j = 0; j < n; j++) {
      if (wi[j].id == ids[i] && strncmp(wi[j].title, "nxlauncher", 10) == 0) {
        int id = ids[i];
        unsigned f = flg[i];
        char t[64];
        memcpy(t, ttl[i], sizeof(t));
        if (i != 0) {
          for (int k = i; k > 0; k--) {
            ids[k] = ids[k - 1];
            flg[k] = flg[k - 1];
            memcpy(ttl[k], ttl[k - 1], sizeof(ttl[k]));
          }
          ids[0] = id;
          flg[0] = f;
          memcpy(ttl[0], t, sizeof(ttl[0]));
        }
        launcher_id = id;
        break;
      }
    }
  }

  /* How many tiles fit per page?  Try without scroll buttons first; only pay
   * their width once paging is actually needed. */
  int inner_w = g_sw - 2 * MARGIN;
  int per_page = (inner_w + TILE_GAP) / (TILE + TILE_GAP);
  if (per_page < 1)
    per_page = 1;
  int scroll_on = total > per_page;
  if (scroll_on) {
    int inner_w2 = inner_w - 2 * (SCROLL_BTN_W + SCROLL_BTN_GAP);
    int pp2 = (inner_w2 + TILE_GAP) / (TILE + TILE_GAP);
    if (pp2 < 1)
      pp2 = 1;
    per_page = pp2;
    scroll_on = total > per_page; /* re-check: a tiny dock could still fit
                                      everything once pp2 is known */
  }

  int pages = scroll_on ? (total + per_page - 1) / per_page : 1;
  if (pages < 1)
    pages = 1;
  if (g_page >= pages)
    g_page = pages - 1;
  if (g_page < 0)
    g_page = 0;
  g_pages = pages;
  g_scroll_on = scroll_on;

  int start = scroll_on ? g_page * per_page : 0;
  int shown = total - start;
  if (shown < 0)
    shown = 0;
  if (shown > per_page)
    shown = per_page;

  int x = MARGIN + (scroll_on ? (SCROLL_BTN_W + SCROLL_BTN_GAP) : 0);
  unsigned dflags[MAX_TILES];
  int vpid[MAX_TILES];
  char vttl[MAX_TILES][64];
  unsigned sig = 2166136261u ^ (unsigned)g_sw ^ ((unsigned)g_page << 8) ^
                 ((unsigned)scroll_on << 16) ^ ((unsigned)g_pressed << 20) ^
                 ((unsigned)g_light << 24);
  int cnt = 0;
  for (int i = 0; i < shown; i++) {
    int idx = start + i;
    g_slot_id[cnt] = ids[idx];
    g_slot_x[cnt] = x;
    dflags[cnt] = flg[idx];
    vpid[cnt] = pids[idx];
    memcpy(vttl[cnt], ttl[idx], sizeof(vttl[cnt]));
    sig = (sig ^ (unsigned)ids[idx]) * 16777619u;
    sig =
        (sig ^ (flg[idx] & (WININFO_FOCUSED | WININFO_MINIMIZED))) * 16777619u;
    cnt++;
    x += TILE + TILE_GAP;
  }
  g_slot_n = cnt;

  if (scroll_on) {
    g_btn_l_x = MARGIN;
    g_btn_r_x = g_sw - MARGIN - SCROLL_BTN_W;
  }

  if (!force && sig == g_sig)
    return; /* nothing the dock shows has changed → skip the blit */
  g_sig = sig;

  /* Transparent first, then the rounded panel on top — the corner pixels
   * fb_rrect skips stay fully transparent so the dock itself reads as a
   * rounded shape (see NOTE(GFX-NXUI-04) above). */
  fb_fill(0x00000000u);
  fb_rrect(0, 0, g_sw, DOCK_H, DOCK_RADIUS, g_col_dock_bg);

  int ty = (DOCK_H - TILE) / 2;
  for (int i = 0; i < cnt; i++) {
    int is_launcher = (launcher_id != 0 && ids[start + i] == launcher_id);
    int minimized = (dflags[i] & WININFO_MINIMIZED) != 0;
    /* The "real" focus (WININFO_FOCUSED).  When the dock itself is the focused
     * window, or a tile was just toggled to minimized, no window reports
     * WININFO_FOCUSED — in that case, the remembered g_last_focus tile stays
     * blue so the user still sees which app owns the focus. */
    int focused = (dflags[i] & WININFO_FOCUSED) ||
                  (!seen_focus && ids[start + i] == g_last_focus);

    /* vttl[i] is the window's own title text (e.g. nxshell's is "NXShell PID
     * 6", not "nxshell"), so this uses nxicon_classify() — the case-
     * insensitive, prefix-matching, path-stripping classifier in nxicon.h —
     * not nxicon_classify_path()'s exact match (that one is for
     * nxlauncher's app-table entries, which are real basenames). */
    /* Phase 3: prefer nxexec's registered icon key (sys.proc.<pid>.icon) — the
     * canonical launch identity — over classifying the app's own (noisy) window
     * title; fall back to the title classifier when no identity is registered. */
    int app_id;
    if (is_launcher) {
      app_id = NXICON_LAUNCHER;
    } else {
      /* Two steps, because the icon is keyed by PROGRAM while the dock holds a
       * PID: first ask the (virtual, always-live) per-process view for the
       * program name, then look the icon up under that name.  Keying the icon
       * by pid instead was the modelling error that made these entries go
       * stale — an icon belongs to the program, not to one run of it. */
      char idkey[64], idname[40], idicon[40];
      snprintf(idkey, sizeof(idkey), "sys.proc.%d.name", vpid[i]);
      if (OS1_registry_get(idkey, idname, sizeof(idname)) == 0 && idname[0]) {
        snprintf(idkey, sizeof(idkey), "sys.appicon.%s", idname);
        if (OS1_registry_get(idkey, idicon, sizeof(idicon)) == 0 && idicon[0])
          app_id = nxicon_classify(idicon);
        else
          app_id = nxicon_classify(idname); /* the name itself classifies well */
      } else {
        app_id = nxicon_classify(vttl[i]);
      }
    }
    os1_image_t *icon = nxicon_get(app_id, g_light, TILE);

    if (icon) {
      /* Icon tiles don't recolor on focus (macOS-dock-style: the icon itself
       * plus the running dot below are enough) — only minimized dims it,
       * replacing the classic path's color swap. */
      fb_rrect_px(g_slot_x[i], ty, TILE, TILE, TILE_RADIUS, 0, icon->pixels,
                  minimized);
      /* 3D bevel ring — icon tiles only, drawn on top so it's never covered
       * by the icon's own pixels. */
      fb_icon_border(g_slot_x[i], ty, TILE, TILE, TILE_RADIUS, g_light);
    } else {
      /* Classic fallback (GFX-NXUI-02): no icon cached for this app/theme —
       * same flat rounded-square logic as before icons existed. */
      uint32_t c = is_launcher ? COL_LAUNCHER : g_col_tile;
      if (is_launcher) {
        /* Launcher stays green across both states — bright when shown/
         * focused, dimmer when backgrounded — so the dock never loses the
         * "this is the launcher" cue.  No special focused override:
         * COL_LAUNCHER already marks "active". */
        c = minimized ? COL_LAUNCHER_MIN : COL_LAUNCHER;
      } else if (focused) {
        c = COL_TILE_FOCUS;
      } else if (minimized) {
        c = g_col_tile_min;
      }
      fb_rrect(g_slot_x[i], ty, TILE, TILE, TILE_RADIUS, c);
    }
    /* running indicator: a small dot under the focused app's tile */
    if (focused)
      fb_rrect(g_slot_x[i] + TILE / 2 - 2, ty + TILE + 3, 4, 3, 1,
               COL_TILE_FOCUS);
  }

  if (scroll_on) {
    uint32_t lc = (g_pressed == 1) ? COL_BTN_PRESSED : COL_BTN_RED;
    uint32_t rc = (g_pressed == 2) ? COL_BTN_PRESSED : COL_BTN_RED;
    fb_rrect(g_btn_l_x, ty, SCROLL_BTN_W, TILE, TILE_RADIUS, lc);
    fb_rrect(g_btn_r_x, ty, SCROLL_BTN_W, TILE, TILE_RADIUS, rc);
  }

  window_blit(g_win, 0, 0, g_sw, DOCK_H, g_fb);
}

/* hit_slot - map a dock-local click x to a window id, or -1 (current page
 * only — scroll first to reach apps parked on another page). */
static int hit_slot(int rx) {
  for (int i = 0; i < g_slot_n; i++)
    if (rx >= g_slot_x[i] && rx < g_slot_x[i] + TILE)
      return g_slot_id[i];
  return -1;
}

/* act - toggle a tile: restore if backgrounded, background if focused, else
 * focus.  Re-reads the window's current state so the toggle is correct.
 * Keeps g_last_focus in sync on the focus-gaining branches (focus and
 * restore).  On the minimize branch we intentionally do NOT clear it: the
 * app that just lost focus is still the one the user was on, so a stray
 * click on the dock / scroll button should bring IT back, not nothing. */
static void act(int id) {
  struct window_info wi[MAX_TILES];
  int n = (int)OS1_window_enum(wi, MAX_TILES);
  unsigned int f = 0;
  for (int i = 0; i < n; i++)
    if (wi[i].id == id) {
      f = wi[i].flags;
      break;
    }
  if (f & WININFO_MINIMIZED) {
    OS1_window_restore(id);
    g_last_focus = id;
  } else if (f & WININFO_FOCUSED) {
    OS1_window_minimize(id);
  } else {
    OS1_window_focus(id);
    g_last_focus = id;
  }
}

int main(void) {
  /* Singleton: only one dock may run.  If a window titled "nxui" already exists
   * (another live instance), bow out.  init respawns the dock when it dies, and
   * a dead process's windows are destroyed by the compositor, so a legitimate
   * respawn finds none and proceeds. */
  {
    struct window_info wi[MAX_TILES];
    int n = (int)OS1_window_enum(wi, MAX_TILES);
    for (int i = 0; i < n; i++)
      if (strncmp(wi[i].title, "nxui", 5) == 0)
        return 0; /* another nxui is already up */
  }

  long di = OS1_display_info();
  int sw = (int)((di >> 16) & 0xFFFF);
  int sh = (int)(di & 0xFFFF);
  if (sw <= 0)
    sw = 800;
  if (sh <= 0)
    sh = 600;

  nxui_load_palette(nxres_theme_is_light());

  g_fb = (uint32_t *)malloc((size_t)sw * DOCK_H * 4);
  if (!g_fb)
    return 1;
  g_win = -1;
  dock_reinit(sw, sh);
  if (g_win < 0)
    return 1;

  redraw(1); /* force the first paint */

  for (;;) {
    /* adapt to a desktop resolution change (polled — works even before the
     * event-driven INPUT_TYPE_RESIZE path below is wired system-wide).
     * Compared against g_dsw/g_dsh (the DESKTOP size) — not g_sw, which is
     * now the dock's own narrower width. */
    long d = OS1_display_info();
    int cw = (int)((d >> 16) & 0xFFFF), ch = (int)(d & 0xFFFF);
    if (cw > 0 && ch > 0 && (cw != g_dsw || ch != g_dsh))
      dock_reinit(cw, ch);

    redraw(0);

    /* Standard window input: clicks arrive as INPUT_TYPE_MOUSE while the dock
     * holds focus (clicking a tile both focuses the dock — so the event is
     * delivered here — and carries dock-local coordinates). */
    input_event_t ev;
    while (input_poll_event(&ev) == 1) {
      if (ev.type == INPUT_TYPE_MOUSE) {
        if (ev.mouse.button == MOUSE_BTN_LEFT &&
            ev.mouse.state == KEY_PRESSED) {
          int acted = 1; /* 0 if this click did not drive any app's focus;
                          * in that case we hand the focus back to the app
                          * that owned it before the dock stole it. */
          if (g_scroll_on && ev.mouse.x >= g_btn_l_x &&
              ev.mouse.x < g_btn_l_x + SCROLL_BTN_W) {
            g_pressed = 1;
            g_page = (g_page - 1 + g_pages) % g_pages; /* loop left */
            redraw(1); /* immediate feedback: pressed colour + new page */
            acted = 0;
          } else if (g_scroll_on && ev.mouse.x >= g_btn_r_x &&
                     ev.mouse.x < g_btn_r_x + SCROLL_BTN_W) {
            g_pressed = 2;
            g_page = (g_page + 1) % g_pages; /* loop right */
            redraw(1);
            acted = 0;
          } else {
            int id = hit_slot(ev.mouse.x);
            if (id > 0)
              act(id); /* drives focus through OS1_window_focus / restore;
                        * for the toggle-to-minimize branch we keep the
                        * previous g_last_focus so the next non-act click
                        * restores this very app. */
            else
              acted = 0; /* click landed on empty dock space */
          }
          /* A click on a tile already drove focus via act().  Anything else
           * (scroll buttons, empty space) only changed dock state — hand
           * the focus back to the app the user was on so the next keystroke
           * does not silently go to the dock. */
          if (!acted && g_last_focus > 0)
            OS1_window_focus(g_last_focus);
        } else if (ev.mouse.button == MOUSE_BTN_LEFT &&
                   ev.mouse.state == KEY_RELEASED && g_pressed) {
          g_pressed = 0; /* back to material red */
          redraw(1);
        }
      } else if (ev.type == INPUT_TYPE_RESIZE) {
        /* event-driven adaptation: the system told us the geometry changed. */
        long d2 = OS1_display_info();
        int rw = (int)((d2 >> 16) & 0xFFFF), rh = (int)(d2 & 0xFFFF);
        if (rw > 0 && rh > 0)
          dock_reinit(rw, rh);
      } else if (ev.type == INPUT_TYPE_LOOK_CHANGED) {
        /* External style/theme/bg change (nxres_broadcast_look, nxres.h),
         * surfaced through this SAME input_poll_event() loop — see nxres.h's
         * header comment for why a second try_recv() loop is wrong here. */
        nxui_load_palette(nxres_theme_is_light());
        redraw(1);
      }
    }

    OS1_sleep(
        33); /* ~30 Hz poll; descheduled between ticks, never busy-spins */
  }
  return 0;
}