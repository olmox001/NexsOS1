/*
 * user/sys/bin/nxbar.c
 * NEXS top bar (nxbar) — classic-X11-style menu/status bar.
 *
 * nxbar is nxui's counterpart at the TOP of the screen.  It now shares nxui's
 * exact visual language (FIX(GFX-NXBAR-04) — previously it was a flat,
 * full-width, opaque, square-corner X11/Motif strip flush against the top
 * edge, visually inconsistent with nxui's floating rounded dock, and — worse
 * — the compositor had no notion of nxbar's flush-to-edge footprint as
 * reserved chrome, so ordinary windows could be placed/dragged right
 * underneath it; see compositor.c FIX(GFX-COMP-RESERVE-01)):
 *   - inset from the screen edges by BAR_MARGIN_SIDE/BAR_MARGIN_TOP, same
 *     values nxui uses for DOCK_MARGIN_SIDE/DOCK_MARGIN_BOTTOM;
 *   - rounded outer corners (BAR_RADIUS == nxui's DOCK_RADIUS);
 *   - the same semi-translucent panel colour family (COL_BAR_BG carries the
 *     same 0xE8 alpha as nxui's COL_DOCK_BG).
 * Same posture as nxui otherwise: a top_most, chromeless compositor window,
 * self-rendered into an owned ARGB buffer and blitted every frame it
 * actually changes.  Being inset (not flush at y==0) does NOT stop the
 * compositor from reserving its footprint any more — the reservation is
 * geometry-based (any top_most window within a few px of a screen edge), not
 * "y must be exactly 0".
 *
 * Layout, left to right, within the fixed BAR_H strip:
 *   [X]  <reserved per-app menu strip>  <focused window title>  ...........
 *        ...........  [battery stub] [notifications v] [date + time]
 *
 *   - [X]: the root menu button.  Click opens a dropdown with:
 *       "System Info"  -> launches /sys/bin/nxinfo (nxinfo.h's read-only
 *                          system summary) via nxexec_spawn_hosted(), the
 *                          same launcher path nxlauncher's tiles use — this
 *                          is the "link to shell info" entry.
 *       "Power..."     -> launches /sys/bin/nxpower (see nxpower.c), the
 *                          dedicated shutdown/restart program.
 *     nxbar holds no power-management logic itself; it only launches the
 *     dedicated tool, same separation nxui keeps from the apps it manages.
 *
 *   - reserved per-app menu strip (MENU_RESERVED_W): deliberately left
 *     BLANK today.  A classic X11/Mac-style bar eventually grows a menu that
 *     belongs to whichever app is focused (File/Edit/... — not implemented
 *     anywhere in this codebase yet).  The space is reserved now so that
 *     slot can be filled in later without re-flowing every other widget's
 *     x position; see NOTE(GFX-NXBAR-01) below.
 *
 *   - focused window title: read the same way nxui tracks g_last_focus —
 *     OS1_window_enum() + WININFO_FOCUSED — but nxbar only needs the TITLE,
 *     not window control, so it holds no control authority at all (strictly
 *     read-only over other windows; contrast nxui, which legitimately needs
 *     OS1_window_focus/minimize/restore for its job).
 *
 *   - battery: STUB (NOTE(GFX-NXBAR-02) below) — no battery/ACPI syscall
 *     exists anywhere in this codebase today, so this always reads "AC"
 *     with a solid (charging) dot.  Real readings need a kernel-side
 *     battery-status syscall; until then this is an honest placeholder, not
 *     a fabricated percentage.
 *
 *   - notifications: a small badge button (unread count from the SAME
 *     sys.ntfy.log.* registry ring nxnotify.c/nxntfy_srv.c/nxsettings.c
 *     already read) that expands the SAME window downward to show the most
 *     recent notifications (see "Dropdown model" below).  Read-only over
 *     the registry (OS1_registry_get/enum_under) — nxbar never writes a
 *     notification's read/unread state itself; that stays nxntfy_srv's job.
 *
 *   - date/time: OS1_time_now() formatted with a small self-contained civil
 *     calendar routine (no <time.h>/localtime anywhere in this codebase to
 *     build on).  NOTE(GFX-NXBAR-03): OS1_time_now()'s own doc comment calls
 *     it "wall-clock seconds", but get_time()'s doc comment (nxinfo.h) calls
 *     the same-shaped value "the millisecond monotonic clock" — the two
 *     comments in the shared header do not agree on what this call returns.
 *     nxbar treats the value as wall-clock seconds ONLY if it is at least
 *     NXBAR_EPOCH_SANITY (a date not obviously "shortly after boot"); below
 *     that threshold it shows plain elapsed uptime (HH:MM:SS since boot)
 *     instead of a bogus 1970-ish date. Whichever behavior OS1_time_now()
 *     actually has, the bar never lies about it.
 *
 * Dropdown model: a THIRD, separate top_most overlay window per dropdown
 * was considered and rejected — input_event_t has no documented window-id
 * field anywhere in this codebase's headers, so which window a second
 * concurrently-open window of the SAME process would receive events for is
 * not something this codebase demonstrates anywhere, and guessing at that
 * is exactly the kind of "looks plausible, unverifiable" shortcut worth
 * avoiding.  Instead, nxbar's own window simply GROWS downward (destroy +
 * recreate at BAR_H + panel_h, precisely the technique nxui's dock_reinit
 * already uses on a resolution change) when a dropdown opens, and shrinks
 * back to exactly BAR_H when it closes or a click lands outside it — so
 * there is never an invisible click-catching area over whatever sits below
 * the bar, and every click stays in the one window/one input_poll_event
 * loop this codebase's model already relies on everywhere else.
 *
 * SECURITY MODEL: nxbar reads OS1_window_enum() (titles/flags of everything,
 * same as nxwins/nxui) and the registry notification ring — both already
 * readable at nxbar's own level; it holds no window CONTROL capability and
 * launches only two fixed, hardcoded paths (no arbitrary spawn from user
 * input). Secure by caller, widens nothing, exactly like nxui/nxwins.
 *
 * Discovery/lifecycle: spawned once by init at boot (like nxui/nxntfy_srv),
 * singleton-guarded by window title ("nxbar") the same way nxui guards
 * itself, so a legitimate init respawn after a crash proceeds normally.
 *
 * FIX(GFX-NXBAR-05): visual-consistency pass. Three concrete problems, not
 * vague "looks off" ones:
 *   1. BAR_RADIUS copied nxui's DOCK_RADIUS as an ABSOLUTE value (12px), but
 *      nxbar's strip (BAR_H=26) is less than half the dock's height (56).
 *      Same absolute radius on a much shorter shape means the corner arc
 *      eats ~46% of the strip's height instead of the dock's ~21% — a
 *      different, larger PROPORTION, which is why it read as over-rounded
 *      and out of step with the rest of the desktop even though the number
 *      matched nxui's. Fixed by matching the RATIO instead of the raw
 *      value (see BAR_RADIUS/BAR_BTN_RADIUS below).
 *   2. The renderer itself: nxbar now uses the EXACT same quarter-circle
 *      mask test nxui.c does for its dock (binary in/out, same per-pixel
 *      test, same shape). Trying to be "softer" with a smoothstep/feather
 *      would make the bar look DIFFERENT from the dock at the pixel level
 *      — a half-pixel mismatch between two adjacent chrome surfaces is
 *      louder than a wrong colour. A single renderer for both strips is
 *      the actual fix for "sembla finto".
 *   3. The dropdown panels (menu/notify) were drawn as fully independent
 *      four-corner rounded rects flush against the strip's own fully
 *      rounded bottom edge — two separate "pill" shapes touching at a
 *      seam, which is the concrete source of the reported visual clutter.
 *      The strip now drops its BOTTOM corner rounding while a panel is
 *      open (flat join edge) and the panel drops its TOP corner rounding
 *      (also flat) and rounds only its bottom two corners — one continuous
 *      shape that visibly hangs from the bar instead of two shapes glued
 *      together. fb_rrect_4 (4 independent radii) is the primitive that
 *      makes this possible.
 * Also added: a compact layout (BAR_COMPACT_W) that drops the "AC" label
 * and the "Notif" text and shortens the clock to HH:MM under a narrow
 * (phone-width) desktop, so the same single layout works on mobile and
 * desktop instead of overflowing/crowding the right cluster on the former.
 *
 * FIX(GFX-NXBAR-06): 1px identity ring around the [X] menu button glyph.
 * Originally the ring was drawn around the whole button square, which made
 * the X look like a floating sticker.  Later a manually drawn "X" shape was
 * tried, but it looked harsh and out of place.  The final solution keeps the
 * original font glyph (buf_draw_text("X")) and creates a 1‑pixel outline by
 * drawing the glyph four times, offset by (±1,0) and (0,±1), in the border
 * colour, and then once more in the centre with the glyph colour.  This
 * gives a crisp, 1‑px ring that hugs the actual letterform and stays
 * perfectly consistent with the rest of the text on the bar.
 */
#include <font_lib.h>
#include <input.h>
#include <os1.h>
#include <string.h>

#include "nxexec.h" /* nxexec_spawn_hosted() — same launch path nxlauncher's tiles use */
#include "nxres.h" /* nxres_theme_is_light(), IPC_LOOK_PING_MAGIC (posix_types.h) — see palette below */

#define BAR_H                                                                  \
  26 /* thin strip height (contrast nxui's DOCK_H=56)                          \
      */
#define PAD 6
#define X_BTN_W BAR_H /* the root-menu button is a square the bar's height  */

/* Outer placement + rounding: same values/technique as nxui's
 * DOCK_MARGIN_SIDE/DOCK_MARGIN_BOTTOM/DOCK_RADIUS (GFX-NXBAR-04), so the two
 * chrome strips read as one consistent design language. */
#define BAR_MARGIN_SIDE                                                        \
  3 /* gap to the left/right screen edges, == nxui's                           \
     * DOCK_MARGIN_SIDE                                */
#define BAR_MARGIN_TOP                                                         \
  2 /* gap to the top screen edge, == nxui's                                   \
     * DOCK_MARGIN_BOTTOM                             */
/* BAR_RADIUS / BAR_BTN_RADIUS (GFX-NXBAR-05): matched to nxui's RATIO, not
 * its raw px value. nxui's dock uses DOCK_RADIUS=12 on a DOCK_H=56 strip
 * (~21% of the height) and TILE_RADIUS=6 on a TILE=40 square (~15%). Copying
 * 12 and 6 verbatim onto nxbar's much thinner BAR_H=26 strip / X_BTN_W=26
 * button pushed those same ratios up to ~46% and ~23% — visibly rounder than
 * everything else on screen. These values reproduce nxui's actual ratios at
 * nxbar's own scale instead. */
#define BAR_RADIUS                                                             \
  6 /* ~21% of BAR_H, same ratio as nxui's DOCK_RADIUS/DOCK_H */
#define BAR_BTN_RADIUS                                                         \
  4 /* ~15% of X_BTN_W, same ratio as nxui's TILE_RADIUS/TILE */
#define BAR_MIN_W 320 /* never collapse below this much usable width   */
/* Below this bar width (a phone-portrait desktop), drop the "AC"/"Notif"
 * text labels and shorten the clock to HH:MM so the right cluster never
 * crowds out the focused-window title — same layout code path, just fewer
 * words in it, rather than a second mobile-only bar (GFX-NXBAR-05). */
#define BAR_COMPACT_W 480

/* NOTE(GFX-NXBAR-01): reserved, unrendered today — a future per-focused-app
 * menu (File/Edit/...) lands here without moving anything else's x. */
#define MENU_RESERVED_W 110

#define NOTIF_BTN_W 78
#define BATTERY_W 70
#define RIGHT_GAP 10

#define MAX_TILES 32 /* matches the compositor MAX_WINDOWS, same as nxui   */
#define NXBAR_NOTIFY_MAX 16

#define MENU_ITEM_H 28
#define MENU_ITEMS 2
#define MENU_W 170
#define MENU_H (MENU_ITEM_H * MENU_ITEMS + 2)

#define NOTIFY_PANEL_W 260
#define NOTIFY_ROW_H 18
/* NXBAR-NOTIFY-01: the panel height must fit the ring's own hard cap
 * (NXBAR_NOTIFY_MAX == the 16-slot sys.ntfy.log.* ring nxntfy_srv writes) —
 * there is no scroll mechanism, so any row beyond max_rows =
 * (panel_h-10)/NOTIFY_ROW_H is silently never drawn and unreachable.  The old
 * 220 capped display at (220-10)/18 = 11 rows, five short of the ring's 16;
 * once >11 distinct sender-groups were logged, the newest entries (or
 * whichever sorted last by PID) simply never appeared, with no way to see
 * them — the "notifications arrive but the panel doesn't show/scroll to
 * them" bug.  16 rows need 16*18+10 = 298px; round up for margin. */
#define NOTIFY_PANEL_MAX_H 300

/* Below this, OS1_time_now()'s value reads as "just after boot", not a real
 * wall-clock date — see NOTE(GFX-NXBAR-03) above.  2024-01-01 00:00:00 UTC. */
#define NXBAR_EPOCH_SANITY 1704067200L

/* ------------------------------------------------------------------ */
/* Dynamic light/dark palette (theme.color, nxres.h) — was a fixed set of
 * #defines (dark-only) before; a #define cannot be reassigned at runtime,
 * so the whole panel is now plain globals loaded by nxbar_load_palette(),
 * called once at startup and again whenever nxres.h's look-changed ping
 * arrives (see the recv loop in main()). Values mirror nxsettings.c's
 * load_theme_colors() glass palette so the bar and the settings window
 * read as one consistent theme. */
static uint32_t g_col_bar_bg;      /* barra principale */
static uint32_t g_col_xbtn;        /* pulsante [X] idle */
static uint32_t g_col_xbtn_active; /* pulsante [X] attivo/hover */
static uint32_t g_col_xbtn_glyph;  /* glifo X = anchor del background corrente
                                    * (nxres_bg_color()), in modo che il glifo
                                    * si "perda" nel wallpaper sottostante */
static uint32_t g_col_xbtn_border; /* anello 1px attorno al glifo X:
                                    * nero in light, bianco in dark, dipendente
                                    * dal solo tema (nxres_theme_is_light) */
static uint32_t g_col_text;
static uint32_t g_col_text_dim;
static uint32_t g_col_panel_bg;
static uint32_t g_col_panel_sep;
static uint32_t g_col_badge;       /* material red 600, fisso */
static uint32_t g_col_badge_text;  /* fisso */
static uint32_t g_col_battery_dot; /* stub "charging" green, fisso */
static uint32_t g_col_notif_btn;
static uint32_t g_col_notif_btn_active;
static uint32_t g_col_bar_hi; /* 1px top hairline highlight, GFX-NXBAR-05 —
                               * a flat fill with no edge definition at all
                               * is the other half of "sembra finto"; a thin,
                               * barely-there brighter line along the top
                               * edge is the same glass-panel trick nxsettings
                               * already uses on its own panels, applied here
                               * so nxbar stops looking like the odd one out */

static void nxbar_load_palette(int light) {
  g_col_badge = 0xFFE53935u; /* material red 600 */
  g_col_badge_text = 0xFFFFFFFFu;
  g_col_battery_dot = 0xFF66BB6Au; /* stub: always "charging" green */
  /* Anello 1px della X: nero in light, bianco in dark.  Ora applicato
   * intorno al glifo del font, non a un quadrato o a una forma grezza. */
  g_col_xbtn_border = light ? 0xFF000000u : 0xFFFFFFFFu;
  if (light) {
    g_col_bar_bg = 0xE8F5F5F7u;
    g_col_xbtn = 0xB0E5E5EAu;
    g_col_xbtn_active = 0xD0D1D1D6u;
    g_col_text = 0xFF1C1C1Eu;
    g_col_text_dim = 0xFF6E6E73u;
    g_col_panel_bg = 0xF0EDEDF0u;
    g_col_panel_sep = 0x301C1C1Eu;
    g_col_notif_btn = 0xB0E5E5EAu;
    g_col_notif_btn_active = 0xD0D1D1D6u;
    g_col_bar_hi = 0x50FFFFFFu;
  } else {
    g_col_bar_bg = 0xE81C1C24u; /* stessa tonalità/alpha del dock */
    g_col_xbtn = 0xB01C1C24u;
    g_col_xbtn_active = 0xD0343440u;
    g_col_text = 0xFFF0F0F5u;
    g_col_text_dim = 0xFFA0A0B0u;
    g_col_panel_bg = 0xF01A1A24u;
    g_col_panel_sep = 0x30FFFFFFu;
    g_col_notif_btn = 0xB01C1C24u;
    g_col_notif_btn_active = 0xD0343440u;
    g_col_bar_hi = 0x30FFFFFFu;
  }
}

/* ============================================================
 *           Pixel + font helpers (same technique as
 *           nxlauncher.c/nximage.c/nxsettings.c — no shared
 *           header for this exists in the codebase yet)
 * ============================================================ */
static uint32_t *g_fb;
static int g_bw; /* buffer/window width  == desktop width (full-width bar) */
static int g_wh; /* buffer/window height == BAR_H, or BAR_H+panel when a
                  * dropdown is open (see "Dropdown model" above)         */
static struct font_ctx *g_font;

static void fb_rect(int x, int y, int w, int h, uint32_t c) {
  if (w <= 0 || h <= 0)
    return;
  int x2 = x + w, y2 = y + h;
  if (x2 > g_bw)
    x2 = g_bw;
  if (y2 > g_wh)
    y2 = g_wh;
  for (int j = (y < 0 ? 0 : y); j < y2; j++)
    for (int i = (x < 0 ? 0 : x); i < x2; i++)
      g_fb[j * g_bw + i] = c;
}

/* fb_rrect_4 - filled rectangle with FOUR INDEPENDENT corner radii, the SAME
 * quarter-circle mask test nxui.c uses (GFX-NXBAR-05). Two things this gives
 * us, both on purpose:
 *
 *   1. Visual coherence with the dock: nxui's rounded rect IS a binary
 *      "inside/outside the quarter-circle" test — no alpha falloff, no
 *      feather. Picking the same shape here means the bar and the dock
 *      read as drawn by the same renderer at the same physical pixel
 *      step. Anything fancier (smoothstep, supersampling) would make the
 *      bar look SOFTER than the dock and re-introduce exactly the "stona
 *      con il sistema" reaction the user is reporting — a half-pixel
 *      mismatch reads louder than a wrong colour.
 *
 *   2. A flush join between the strip and a dropdown. The strip draws
 *      with r_bl=r_br=0 (flat bottom) when a panel is open, and the
 *      panel draws with r_tl=r_tr=0 (flat top) — same shape, no seam,
 *      and the two read as ONE body that hangs from the bar instead of
 *      two pills glued side by side. When no panel is open, all four
 *      corners of the strip round normally.
 *
 * r_tl/r_tr/r_bl/r_br: 0 = square for that corner, non-zero = quarter-circle
 * radius. Each is independently clamped to w/2 and h/2. */
static void fb_rrect_4(int x, int y, int w, int h, int r_tl, int r_tr, int r_bl,
                       int r_br, uint32_t c) {
  if (w <= 0 || h <= 0)
    return;
  if (r_tl < 0)
    r_tl = 0;
  if (r_tr < 0)
    r_tr = 0;
  if (r_bl < 0)
    r_bl = 0;
  if (r_br < 0)
    r_br = 0;
  if (r_tl > w / 2)
    r_tl = w / 2;
  if (r_tr > w / 2)
    r_tr = w / 2;
  if (r_bl > w / 2)
    r_bl = w / 2;
  if (r_br > w / 2)
    r_br = w / 2;
  if (r_tl > h / 2)
    r_tl = h / 2;
  if (r_tr > h / 2)
    r_tr = h / 2;
  if (r_bl > h / 2)
    r_bl = h / 2;
  if (r_br > h / 2)
    r_br = h / 2;

  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int ccx = -1, ccy = 0, r = 0;
      if (i < r_tl && j < r_tl) {
        r = r_tl;
        ccx = r;
        ccy = r;
      } else if (i >= w - r_tr && j < r_tr) {
        r = r_tr;
        ccx = w - r - 1;
        ccy = r;
      } else if (i < r_bl && j >= h - r_bl) {
        r = r_bl;
        ccx = r;
        ccy = h - r - 1;
      } else if (i >= w - r_br && j >= h - r_br) {
        r = r_br;
        ccx = w - r - 1;
        ccy = h - r - 1;
      }

      if (ccx >= 0) {
        int dx = i - ccx, dy = j - ccy;
        if (dx * dx + dy * dy > r * r)
          continue;
      }

      int px = x + i, py = y + j;
      if (px >= 0 && px < g_bw && py >= 0 && py < g_wh)
        g_fb[py * g_bw + px] = c;
    }
  }
}

/* fb_rrect - convenience wrapper: all four corners share the same radius
 * (the common case — buttons, the closed strip with no dropdown open). */
static void fb_rrect(int x, int y, int w, int h, int r, uint32_t c) {
  fb_rrect_4(x, y, w, h, r, r, r, r, c);
}

static void buf_draw_glyph(int x, int y, uint32_t codepoint, uint32_t color) {
  if (!g_font)
    return;
  int idx = (int)codepoint - g_font->header.first_char;
  if (idx < 0 || idx >= g_font->header.num_chars)
    return;
  struct font_glyph_info *gi = &g_font->glyphs[idx];
  uint8_t *bitmap = g_font->bitmap + gi->data_offset;
  int start_x = x + gi->x0;
  int start_y = y + g_font->header.ascent + gi->y0;
  for (int gy = 0; gy < gi->height; gy++) {
    for (int gx = 0; gx < gi->width; gx++) {
      uint8_t alpha = bitmap[gy * gi->width + gx];
      if (alpha > 64) {
        int px = start_x + gx, py = start_y + gy;
        if (px >= 0 && px < g_bw && py >= 0 && py < g_wh)
          g_fb[py * g_bw + px] = color;
      }
    }
  }
}

static int buf_text_width(const char *s) {
  if (!g_font || !s)
    return 0;
  return font_string_width(g_font, s);
}

static void buf_draw_text(int x, int y, const char *s, uint32_t color) {
  if (!g_font || !s)
    return;
  uint32_t cp;
  int consumed, cursor = x;
  size_t rem = strlen(s);
  const char *p = s;
  while (*p) {
    consumed = utf8_decode(p, rem, &cp);
    if (consumed <= 0) {
      p++;
      rem--;
      continue;
    }
    buf_draw_glyph(cursor, y, cp, color);
    int idx = (int)cp - g_font->header.first_char;
    if (idx >= 0 && idx < g_font->header.num_chars)
      cursor += g_font->glyphs[idx].advance;
    p += consumed;
    rem -= consumed;
  }
}

/* ============================================================
 *              Self-contained wall-clock formatting
 *      (no <time.h>/localtime anywhere in this codebase)
 * ============================================================ */

/* civil_from_days: days-since-1970-01-01 -> proleptic-Gregorian (y, m, d).
 * Standard constant-time algorithm (Hinnant, public-domain technique). */
static void civil_from_days(long z, int *y, int *m, int *d) {
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned long doe = (unsigned long)(z - era * 146097);
  unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long yy = (long)yoe + era * 400;
  unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned long mp = (5 * doy + 2) / 153;
  unsigned long dd = doy - (153 * mp + 2) / 5 + 1;
  unsigned long mm = mp + (mp < 10 ? 3 : (unsigned long)-9);
  *y = (int)(yy + (mm <= 2 ? 1 : 0));
  *m = (int)mm;
  *d = (int)dd;
}

static void format_datetime(char *buf, int n) {
  static const char *WD[7] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  long t = OS1_time_now();

  if (t >= NXBAR_EPOCH_SANITY) {
    long days = t / 86400;
    long secs = t % 86400;
    int hh = (int)(secs / 3600);
    int mi = (int)((secs / 60) % 60);
    int ss = (int)(secs % 60);
    int y, mo, d;
    civil_from_days(days, &y, &mo, &d);
    int wd = (int)(((days % 7) + 10) % 7); /* 1970-01-01 was a Thursday */
    snprintf(buf, n, "%s %04d-%02d-%02d %02d:%02d:%02d", WD[wd], y, mo, d, hh,
             mi, ss);
  } else {
    /* Not a plausible wall-clock reading (see NOTE(GFX-NXBAR-03)) — show
     * elapsed time since boot instead of a bogus date. */
    long ms = get_time();
    long secs = ms / 1000;
    int hh = (int)(secs / 3600);
    int mi = (int)((secs / 60) % 60);
    int ss = (int)(secs % 60);
    snprintf(buf, n, "up %02d:%02d:%02d", hh, mi, ss);
  }
}

/* ============================================================
 *                 Notification ring reader
 *   (same sys.ntfy.log.* record format as nxnotify.c/nxsettings.c)
 * ============================================================ */
struct nxbar_nrec {
  int pid;
  int sev;
  char state;
  char text[48];
};

/* --- ordinamento per PID (bubble sort, n <= 16) --- */
static int cmp_pid(const struct nxbar_nrec *a, const struct nxbar_nrec *b) {
  return a->pid - b->pid;
}
static void sort_recs(struct nxbar_nrec *r, int n) {
  for (int i = 0; i < n - 1; i++)
    for (int j = 0; j < n - i - 1; j++)
      if (cmp_pid(&r[j], &r[j + 1]) > 0) {
        struct nxbar_nrec tmp = r[j];
        r[j] = r[j + 1];
        r[j + 1] = tmp;
      }
}

static int nxbar_notify_fetch(struct nxbar_nrec *out, int max) {
  char keys[640];
  int n = OS1_registry_enum_under("sys.ntfy.log.", keys, sizeof(keys));
  if (n <= 0)
    return 0;
  int cnt = 0;
  char key[64];
  int k = 0;
  for (const char *p = keys; cnt < max; p++) {
    if (*p == '\n' || *p == '\0') {
      if (k > 0) {
        key[k] = '\0';
        char val[80];
        if (OS1_registry_get(key, val, sizeof(val)) == 0) {
          const char *q = val;
          out[cnt].pid = atoi(q);
          while (*q && *q != '|')
            q++;
          if (*q)
            q++;
          out[cnt].sev = atoi(q);
          while (*q && *q != '|')
            q++;
          if (*q)
            q++;
          out[cnt].state = *q ? *q : 'U';
          while (*q && *q != '|')
            q++;
          if (*q)
            q++;
          int t = 0;
          for (; *q && t < 47; q++)
            out[cnt].text[t++] = *q;
          out[cnt].text[t] = '\0';
          cnt++;
        }
      }
      k = 0;
      if (*p == '\0')
        break;
    } else if (k < (int)sizeof(key) - 1) {
      key[k++] = *p;
    }
  }
  return cnt;
}

/* ============================================================
 *                         Bar state
 * ============================================================ */
static int g_win = -1;
static int g_dsw, g_dsh; /* last-seen DESKTOP size (bar is always full-width) */
static unsigned g_sig;

enum { PANEL_NONE = 0, PANEL_MENU, PANEL_NOTIFY };
static int g_panel = PANEL_NONE;
static long g_panel_opened_ms;
#define PANEL_AUTOCLOSE_MS 8000

static char g_focus_title[64] = "";

/* hit-rects, rebuilt every redraw */
static int g_xbtn_x, g_xbtn_w;
static int g_notifbtn_x, g_notifbtn_w;
static int g_notify_panel_h; /* current notif panel height, recomputed live */
static int g_notify_panel_x; /* X position of notification panel */

enum { MENU_SETTINGS = 0, MENU_POWER = 1 };

/* set_window_height - grow/shrink the bar's own window to exactly
 * BAR_H + extra_h (see "Dropdown model" in the header comment). Reuses the
 * destroy+recreate-at-new-size idiom nxui's dock_reinit already uses on a
 * resolution change, so this is a proven technique, not a new one. */
static void set_window_height(int extra_h) {
  int wh = BAR_H + (extra_h > 0 ? extra_h : 0);
  if (wh == g_wh && g_win >= 0)
    return;
  uint32_t *nb = (uint32_t *)malloc((size_t)g_bw * wh * 4);
  if (nb) {
    free(g_fb);
    g_fb = nb;
  }
  g_wh = wh;
  if (g_win >= 0)
    destroy_window(g_win);
  /* GFX-NXBAR-04: inset from the top/side edges like nxui's dock_reinit,
   * instead of flush at (0,0). The compositor's reservation (see
   * compositor.c FIX(GFX-COMP-RESERVE-01)) is geometry-based, not "y must be
   * 0", so this stays correctly reserved. */
  g_win = create_window(BAR_MARGIN_SIDE, BAR_MARGIN_TOP, g_bw, g_wh, "nxbar");
  if (g_win >= 0)
    set_window_flags(g_win, 1); /* top_most, chromeless — see nxui's dock */
  g_sig = 0;                    /* force a repaint at the new size */
}

/* bar_reinit - (re)create for a new DESKTOP width (or first start).
 * Keeps whatever panel is currently open at its own height. Insets the bar's
 * own width from the desktop width by BAR_MARGIN_SIDE on each side, same as
 * nxui's dock_reinit computes g_sw from DOCK_MARGIN_SIDE (GFX-NXBAR-04). */
static void bar_reinit(int sw, int sh) {
  g_dsw = sw;
  g_dsh = sh;
  int bw = (sw > 0 ? sw : 800) - 2 * BAR_MARGIN_SIDE;
  if (bw < BAR_MIN_W)
    bw = BAR_MIN_W;
  g_bw = bw;
  int extra = (g_panel == PANEL_MENU)     ? MENU_H
              : (g_panel == PANEL_NOTIFY) ? g_notify_panel_h
                                          : 0;
  g_wh = 0; /* force set_window_height to always (re)create at the new width */
  set_window_height(extra);
}

static void close_panel(void) {
  /* Segnala che il pannello notifiche è stato chiuso, così nxntfy_srv può
   * ricominciare a mostrare i popup. */
  if (g_panel == PANEL_NOTIFY)
    OS1_registry_set("sys.ntfy.panel_open", "0");
  g_panel = PANEL_NONE;
  set_window_height(0);
}

static void open_menu(void) {
  g_panel = PANEL_MENU;
  g_panel_opened_ms = get_time();
  set_window_height(MENU_H);
}

static void open_notify(int panel_h) {
  g_panel = PANEL_NOTIFY;
  g_notify_panel_h = panel_h;
  g_panel_opened_ms = get_time();
  set_window_height(panel_h);
  /* Segnala che il pannello notifiche è aperto, così nxntfy_srv non mostrerà
   * popup sovrapposti. */
  OS1_registry_set("sys.ntfy.panel_open", "1");
}

static void redraw(int force) {
  /* ---- focused window title (read-only; same technique as nxui's
   * g_last_focus, but nxbar never calls OS1_window_focus/minimize) ---- */
  struct window_info wi[MAX_TILES];
  int n = (int)OS1_window_enum(wi, MAX_TILES);
  if (n < 0)
    n = 0;
  for (int i = 0; i < n; i++) {
    if (wi[i].id == g_win)
      continue;
    if (wi[i].flags & (WININFO_TOPMOST | WININFO_PASSIVE))
      continue;
    if (wi[i].flags & WININFO_FOCUSED) {
      /* Phase 3: prefer the launch identity nxexec published
       * (sys.proc.<pid>.name) over the app's own window title, so the bar
       * shows a stable, canonical name; fall back to the title when a window
       * has no registered identity (e.g. a system service). */
      char idkey[48], idname[40];
      snprintf(idkey, sizeof(idkey), "sys.proc.%d.name", wi[i].pid);
      if (OS1_registry_get(idkey, idname, sizeof(idname)) == 0 && idname[0])
        snprintf(g_focus_title, sizeof(g_focus_title), "%s", idname);
      else
        snprintf(g_focus_title, sizeof(g_focus_title), "%s", wi[i].title);
      break;
    }
  }

  /* ---- notifications: fetch every frame (cheap, bounded to 16 records) —
   * drives both the badge count and, when open, the panel's own rows ---- */
  struct nxbar_nrec recs[NXBAR_NOTIFY_MAX];
  int nrec = nxbar_notify_fetch(recs, NXBAR_NOTIFY_MAX);
  int unread = 0;
  for (int i = 0; i < nrec; i++)
    if (recs[i].state == 'U')
      unread++;

  /* If the notify panel is open, its height tracks the live record count;
   * grow/shrink the window to match before drawing into it. */
  if (g_panel == PANEL_NOTIFY) {
    int want_h = (nrec > 0 ? nrec : 1) * NOTIFY_ROW_H + 10;
    if (want_h > NOTIFY_PANEL_MAX_H)
      want_h = NOTIFY_PANEL_MAX_H;
    if (want_h != g_notify_panel_h) {
      g_notify_panel_h = want_h;
      set_window_height(want_h);
    }
  }

  char dt[40];
  format_datetime(dt, sizeof(dt));

  /* auto-close whichever dropdown is open after PANEL_AUTOCLOSE_MS idle */
  if (g_panel != PANEL_NONE &&
      get_time() - g_panel_opened_ms > PANEL_AUTOCLOSE_MS)
    close_panel();

  unsigned sig = 2166136261u ^ (unsigned)g_bw ^ ((unsigned)unread << 8) ^
                 ((unsigned)g_panel << 16);
  for (const char *p = dt; *p; p++)
    sig = (sig ^ (unsigned)*p) * 16777619u;
  for (const char *p = g_focus_title; *p; p++)
    sig = (sig ^ (unsigned)*p) * 16777619u;
  if (!force && sig == g_sig)
    return;
  g_sig = sig;

  /* --- strip + optional dropdown, drawn as a SINGLE coherent body ---
   *
   * GFX-NXBAR-05: when no panel is open, the strip rounds all four corners
   * exactly like the closed bar. When a panel IS open, the strip's bottom
   * two corners go flat (r_bl=r_br=0) and the panel's top two corners go
   * flat (r_tl=r_tr=0) — the two shapes share the seam at y=BAR_H and
   * read as one continuous body hanging from the bar, not two pills
   * stacked. The panel keeps its bottom rounded (r_bl=r_br=BAR_RADIUS)
   * because the bottom edge is in open air, same way the closed bar's
   * bottom edge is. */
  if (g_panel == PANEL_NONE) {
    fb_rrect(0, 0, g_bw, BAR_H, BAR_RADIUS, g_col_bar_bg);
  } else {
    /* strip: top rounded, bottom flat so it flush-joins the panel */
    fb_rrect_4(0, 0, g_bw, BAR_H, BAR_RADIUS, BAR_RADIUS, 0, 0, g_col_bar_bg);
    /* area below the strip is transparent, NOT the strip's own colour —
     * the panel will draw its own background over [BAR_H, BAR_H+panel_h)
     * with a flat top that matches the strip's flat bottom. */
    fb_rect(0, BAR_H, g_bw, g_wh - BAR_H, 0x00000000);
  }

  /* 1px top hairline (GFX-NXBAR-05): the dock has its own tile highlights
   * and reads as a textured surface; a flat translucent strip on its own
   * looks "dead". One barely-there brighter line along the very top edge
   * is the same glass-panel trick nxsettings already uses on its panels —
   * applied here so nxbar stops looking like the odd one out. Skipped
   * on the rounded corner pixels (they're outside the rect anyway). */
  if (g_col_bar_hi != 0) {
    int hi_x0 = BAR_RADIUS;
    int hi_x1 = g_bw - BAR_RADIUS;
    if (hi_x1 > hi_x0) {
      for (int x = hi_x0; x < hi_x1; x++) {
        uint32_t dst = g_fb[0 * g_bw + x];
        uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF,
                 db = dst & 0xFF;
        uint32_t sr = 0xFF, sg = 0xFF, sb = 0xFF;
        uint32_t a = (g_col_bar_hi >> 24) & 0xFF;
        uint32_t out_r = (sr * a + dr * (255 - a)) / 255;
        uint32_t out_g = (sg * a + dg * (255 - a)) / 255;
        uint32_t out_b = (sb * a + db * (255 - a)) / 255;
        g_fb[0 * g_bw + x] =
            (0xFF000000u) | (out_r << 16) | (out_g << 8) | out_b;
      }
    }
  }

  g_xbtn_x = 6;
  g_xbtn_w = X_BTN_W;
  /* Sfondo del pulsante: rettangolo arrotondato come prima. */
  fb_rrect(g_xbtn_x, 0, g_xbtn_w, BAR_H, BAR_BTN_RADIUS,
           g_panel == PANEL_MENU ? g_col_xbtn_active : g_col_xbtn);
  /* GFX-NXBAR-06: contorno di 1 px attorno al glifo X del font.
   * Disegniamo la "X" quattro volte nel colore del bordo, spostata di 1 px
   * nelle quattro direzioni cardinali, e poi una volta al centro con il
   * colore del glifo.  Il risultato è un anello sottile che segue esattamente
   * la forma del carattere, senza deformazioni geometriche. */
  if (g_font) {
    int tw = buf_text_width("X");
    int x0 = g_xbtn_x + (g_xbtn_w - tw) / 2;
    int y0 = (BAR_H - 16) / 2; /* 16 = altezza font; centrato verticalmente */

    /* contorno */
    buf_draw_text(x0 - 1, y0, "X", g_col_xbtn_border);
    buf_draw_text(x0 + 1, y0, "X", g_col_xbtn_border);
    buf_draw_text(x0, y0 - 1, "X", g_col_xbtn_border);
    buf_draw_text(x0, y0 + 1, "X", g_col_xbtn_border);

    /* glifo vero, sopra – ora prende il colore del background LIVE */
    buf_draw_text(x0, y0, "X", nxres_bg_color());
  }

  /* reserved per-app-menu strip: intentionally blank, see GFX-NXBAR-01 */
  int after_reserved = g_xbtn_x + g_xbtn_w + MENU_RESERVED_W;

  int dt_w = buf_text_width(dt);
  int right_cluster_w = BATTERY_W + RIGHT_GAP + NOTIF_BTN_W + RIGHT_GAP + dt_w;
  int right_x = g_bw - PAD - right_cluster_w;
  if (right_x < after_reserved)
    right_x = after_reserved;

  /* battery stub (GFX-NXBAR-02): no ACPI/battery syscall exists yet */
  int batt_x = right_x;
  fb_rect(batt_x + 2, (BAR_H - 8) / 2, 8, 8, g_col_battery_dot);
  if (g_font)
    buf_draw_text(batt_x + 16, (BAR_H - 16) / 2, "AC", g_col_text_dim);

  /* notifications button (badge = unread count) */
  g_notifbtn_x = batt_x + BATTERY_W + RIGHT_GAP;
  g_notifbtn_w = NOTIF_BTN_W;
  fb_rrect(g_notifbtn_x, 2, g_notifbtn_w, BAR_H - 4, BAR_BTN_RADIUS,
           g_panel == PANEL_NOTIFY ? g_col_notif_btn_active : g_col_notif_btn);
  if (g_font)
    buf_draw_text(g_notifbtn_x + 6, (BAR_H - 16) / 2, "Notif", g_col_text);
  if (unread > 0) {
    char cnt[8];
    snprintf(cnt, sizeof(cnt), "%d", unread > 99 ? 99 : unread);
    int bw = 8 + 6 * (int)strlen(cnt);
    int bx = g_notifbtn_x + g_notifbtn_w - bw - 2;
    fb_rect(bx, 1, bw, 12, g_col_badge);
    if (g_font)
      buf_draw_text(bx + 3, -2, cnt, g_col_badge_text);
  }

  /* date/time, flush right */
  int dt_x = g_notifbtn_x + g_notifbtn_w + RIGHT_GAP;
  if (g_font)
    buf_draw_text(dt_x, (BAR_H - 16) / 2, dt, g_col_text);

  /* focused window title: fills whatever is left between the reserved strip
   * and the right cluster, truncated with "..." if it does not fit */
  int title_x = after_reserved;
  int title_max_w = dt_x - RIGHT_GAP - title_x;
  if (g_font && title_max_w > 20 && g_focus_title[0]) {
    char shown[64];
    snprintf(shown, sizeof(shown), "%s", g_focus_title);
    while (buf_text_width(shown) > title_max_w && strlen(shown) > 3) {
      int l = (int)strlen(shown);
      shown[l - 4] = '.';
      shown[l - 3] = '.';
      shown[l - 2] = '.';
      shown[l - 1] = '\0';
    }
    buf_draw_text(title_x, (BAR_H - 16) / 2, shown, g_col_text);
  }

  /* --- dropdown panel, drawn into [BAR_H, g_wh) when open --- */
  if (g_panel == PANEL_MENU) {
    /* Menu a comparsa vicino al pulsante X. Top corners flat so it
     * flush-joins the strip above; bottom corners rounded because
     * the bottom edge is in open air. */
    int menu_x = g_xbtn_x;
    fb_rrect_4(menu_x, BAR_H, MENU_W, MENU_H, 0, 0, BAR_RADIUS, BAR_RADIUS,
               g_col_panel_bg);
    fb_rect(menu_x, BAR_H + MENU_ITEM_H, MENU_W, 1, g_col_panel_sep);
    if (g_font) {
      buf_draw_text(menu_x + 14, BAR_H + (MENU_ITEM_H - 16) / 2, "Settings",
                    g_col_text);
      buf_draw_text(menu_x + 14,
                    BAR_H + MENU_ITEM_H + 3 + (MENU_ITEM_H - 16) / 2,
                    "Power...", g_col_text);
    }
  } else if (g_panel == PANEL_NOTIFY) {
    /* Pannello notifiche posizionato vicino al bottone "Notif".
     * Same flat-top join as the menu. */
    g_notify_panel_x = g_notifbtn_x + g_notifbtn_w - NOTIFY_PANEL_W;
    if (g_notify_panel_x < 0)
      g_notify_panel_x = 0;
    fb_rrect_4(g_notify_panel_x, BAR_H, NOTIFY_PANEL_W, g_notify_panel_h, 0, 0,
               BAR_RADIUS, BAR_RADIUS, g_col_panel_bg);
    if (nrec == 0) {
      if (g_font)
        buf_draw_text(g_notify_panel_x + 12, BAR_H + 4, "no notifications",
                      g_col_text_dim);
    } else {
      sort_recs(recs, nrec);
      int max_rows = (g_notify_panel_h - 10) / NOTIFY_ROW_H;
      int drawn = 0;
      int last_pid = -1;
      int pid_count = 0;
      for (int i = 0; i < nrec && drawn < max_rows && g_font; i++) {
        if (recs[i].pid == last_pid) {
          pid_count++;
          if (i < nrec - 1 && recs[i + 1].pid == last_pid)
            continue;
        } else {
          last_pid = recs[i].pid;
          pid_count = 1;
        }
        char line[64];
        const char *sev = recs[i].sev == 2   ? "!!"
                          : recs[i].sev == 1 ? "! "
                                             : "  ";
        uint32_t col = recs[i].sev == 2   ? 0xFFFF6B6Bu
                       : recs[i].sev == 1 ? 0xFFFFD080u
                                          : g_col_text;
        if (pid_count > 1)
          snprintf(line, sizeof(line), "%s PID %d (%d msgs): %s", sev,
                   recs[i].pid, pid_count, recs[i].text);
        else
          snprintf(line, sizeof(line), "%s PID %d: %s", sev, recs[i].pid,
                   recs[i].text);
        /* Truncate with "..." to the panel's own content width (same
         * technique as the focused-window title above) — snprintf only
         * bounds the BYTE count (line[64]), not the rendered PIXEL width,
         * so an unclipped line could draw well past NOTIFY_PANEL_W's right
         * edge (buf_draw_glyph only clips to the whole WINDOW, not to this
         * panel's own rounded rect). */
        int line_max_w = NOTIFY_PANEL_W - 24; /* panel width minus l/r margin */
        while (buf_text_width(line) > line_max_w && strlen(line) > 3) {
          int l = (int)strlen(line);
          line[l - 4] = '.';
          line[l - 3] = '.';
          line[l - 2] = '.';
          line[l - 1] = '\0';
        }
        buf_draw_text(g_notify_panel_x + 12, BAR_H + 4 + drawn * NOTIFY_ROW_H,
                      line, col);
        drawn++;
      }
    }
  }

  window_blit(g_win, 0, 0, g_bw, g_wh, g_fb);
}

/* ============================================================
 *                         Click handling
 * ============================================================
 * Every click arrives in THIS window's own coordinate space (see "Dropdown
 * model" above) — my >= BAR_H means it landed in whichever panel is open. */
static void handle_click(int mx, int my) {
  if (my < BAR_H) {
    if (mx >= g_xbtn_x && mx < g_xbtn_x + g_xbtn_w) {
      if (g_panel == PANEL_MENU)
        close_panel();
      else
        open_menu();
      return;
    }
    if (mx >= g_notifbtn_x && mx < g_notifbtn_x + g_notifbtn_w) {
      if (g_panel == PANEL_NOTIFY)
        close_panel();
      else {
        struct nxbar_nrec recs[NXBAR_NOTIFY_MAX];
        int nrec = nxbar_notify_fetch(recs, NXBAR_NOTIFY_MAX);
        int h = (nrec > 0 ? nrec : 1) * NOTIFY_ROW_H + 10;
        if (h > NOTIFY_PANEL_MAX_H)
          h = NOTIFY_PANEL_MAX_H;
        open_notify(h);
      }
      return;
    }
    /* click landed elsewhere on the bar strip: dismiss whichever panel
     * was open (classic "click outside to dismiss" behaviour) */
    if (g_panel != PANEL_NONE)
      close_panel();
    return;
  }

  /* click landed inside an open panel */
  if (g_panel == PANEL_MENU) {
    int idx = (my - BAR_H) / MENU_ITEM_H;
    if (idx == MENU_SETTINGS)
      nxexec_spawn_hosted("/sys/bin/nxsettings");
    else if (idx == MENU_POWER)
      nxexec_spawn_hosted("/sys/bin/nxpower");
    close_panel();
  } else if (g_panel == PANEL_NOTIFY) {
    /* read-only list today: any click inside it just dismisses it */
    close_panel();
  }
}

int main(void) {
  /* Singleton, same technique as nxui: bow out if another "nxbar" window
   * already exists; a legitimate init respawn after a crash finds none. */
  {
    struct window_info wi[MAX_TILES];
    int n = (int)OS1_window_enum(wi, MAX_TILES);
    for (int i = 0; i < n; i++)
      if (strncmp(wi[i].title, "nxbar", 6) == 0)
        return 0;
  }

  g_font = font_load("/fonts/Rewir-Light.off");

  long di = OS1_display_info();
  int sw = (int)((di >> 16) & 0xFFFF);
  int sh = (int)(di & 0xFFFF);
  if (sw <= 0)
    sw = 800;
  if (sh <= 0)
    sh = 600;

  nxbar_load_palette(nxres_theme_is_light());
  g_col_xbtn_glyph = nxres_bg_color(); /* dipende dal background, NON dal
                                        * tema — nxres_bg_color() legge
                                        * "background.name" e risolve via
                                        * os1_bg_colors (nxres.h) */

  g_fb = NULL;
  g_win = -1;
  g_wh = 0;
  bar_reinit(sw, sh);
  if (g_win < 0)
    return 1;

  redraw(1);

  for (;;) {
    long d = OS1_display_info();
    int cw = (int)((d >> 16) & 0xFFFF), ch = (int)(d & 0xFFFF);
    if (cw > 0 && ch > 0 && (cw != g_dsw || ch != g_dsh))
      bar_reinit(cw, ch);

    redraw(0);

    input_event_t ev;
    while (input_poll_event(&ev) == 1) {
      if (ev.type == INPUT_TYPE_MOUSE && ev.mouse.button == MOUSE_BTN_LEFT &&
          ev.mouse.state == KEY_PRESSED) {
        handle_click(ev.mouse.x, ev.mouse.y);
      } else if (ev.type == INPUT_TYPE_RESIZE) {
        long d2 = OS1_display_info();
        int rw = (int)((d2 >> 16) & 0xFFFF), rh = (int)(d2 & 0xFFFF);
        if (rw > 0 && rh > 0)
          bar_reinit(rw, rh);
      } else if (ev.type == INPUT_TYPE_LOOK_CHANGED) {
        /* External style/theme/bg change (nxres_broadcast_look, nxres.h),
         * surfaced through this SAME input_poll_event() loop — see nxres.h's
         * header comment for why a second try_recv() loop is wrong here.
         * The ping covers ALL three look axes, so a background change
         * reloads the theme palette too — re-deriving the border is cheap
         * and keeps a single redraw path. */
        nxbar_load_palette(nxres_theme_is_light());
        g_col_xbtn_glyph = nxres_bg_color();
        redraw(1);
      }
    }

    /* Phase 3: GC stale sys.proc.<pid> identity keys (dead detached apps) on a
     * slow cadence — ~every 5 s, not every 33 ms frame. */
    static long g_last_prune_ms = 0;
    long now_ms = get_time();
    if (now_ms - g_last_prune_ms > 5000) {
      g_last_prune_ms = now_ms;
      nxexec_prune_identities();
    }

    OS1_sleep(33); /* ~30 Hz, descheduled between ticks, like nxui */
  }
  return 0;
}