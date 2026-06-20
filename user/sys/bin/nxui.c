/*
 * user/sys/bin/nxui.c
 * NEXS dock — the system taskbar (an ASTRA SRL userland service).
 *
 * nxui is a thin userland window manager UI.  It keeps the compositor as the
 * pure MECHANISM and owns the POLICY itself:
 *   - it enumerates every window with SYS_WINDOW_ENUM (OS1_window_enum);
 *   - it draws a macOS/Darwin-style dock of rounded squares along the bottom of
 *     the screen, one tile per app (icon-ready: a plain rounded square today,
 *     a real icon later);
 *   - a click on a tile FOCUSES that app, RESTORES it if it was sent to the
 *     background, or BACKGROUNDS it if it was the focused app (toggle).
 *
 * All window control goes through OS1_window_* — i.e. OBJ_TYPE_WINDOW
 * capabilities (ASTRA §6.7): the authority is the unforgeable handle, not
 * nxui's identity.  nxui is spawned by init at machine level (it is a window
 * manager), so it may acquire a control capability to any app's window.
 *
 * Input uses the STANDARD window input API (input_poll_event / input_event_t),
 * exactly like every other windowed app — mouse buttons are the evdev codes
 * MOUSE_BTN_* the compositor forwards, NOT a 0/1/2 index.  The dock is itself a
 * top_most compositor window (chromeless: a top_most window has no
 * titlebar/buttons), drawn by blitting a self-rendered ARGB buffer.
 *
 * It re-lays-out on a desktop resolution change (polled, plus the event-driven
 * INPUT_TYPE_RESIZE path for when the system notifies it), and only re-blits
 * when the app set actually changes — it never busy-spins.
 *
 * NOTE(GFX-NXUI-02): tiles are blank rounded squares — icon/initial rendering
 * needs the compositor font exposed to userland buffers (future).
 * NOTE(GFX-NXUI-03): runs at PLVL_MACHINE; PLVL_ROOT is tighter once the
 * per-path VFS capability preset (/sys/bin) lands.
 */
#include <os1.h>
#include <input.h>
#include <string.h>

#define DOCK_H      56  /* dock height in px                                   */
#define TILE        40  /* app tile size in px                                 */
#define TILE_GAP    12  /* gap between tiles                                   */
#define TILE_RADIUS 9   /* tile corner radius                                  */
#define MARGIN      16  /* left/right inset                                    */
#define MAX_TILES   32  /* matches the compositor MAX_WINDOWS                  */

#define COL_DOCK_BG    0xE81C1C24u /* translucent dark slab                    */
#define COL_TILE       0xFF3A3A46u /* idle app                                 */
#define COL_TILE_FOCUS 0xFF5E9CFFu /* focused app                             */
#define COL_TILE_MIN   0xFF2A2A32u /* backgrounded (minimized) app             */

static uint32_t *g_fb;       /* dock pixel buffer (g_sw x DOCK_H, ARGB)        */
static int g_sw, g_sh;       /* desktop size                                   */
static int g_win = -1;       /* the dock's own window id                       */
static unsigned g_sig;       /* signature of the last drawn state (no re-blit  */
                             /* when unchanged → the dock is never a spinbomb)  */

/* current tile→window mapping, rebuilt every poll for click hit-testing */
static int g_slot_id[MAX_TILES];
static int g_slot_x[MAX_TILES];
static int g_slot_n;

static void fb_fill(uint32_t c) {
  int total = g_sw * DOCK_H;
  for (int i = 0; i < total; i++)
    g_fb[i] = c;
}

/* fb_rrect - filled rounded rectangle into the dock buffer (corners clipped to
 * a quarter-circle of radius r). */
static void fb_rrect(int x, int y, int w, int h, int r, uint32_t c) {
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
      int px = x + i, py = y + j;
      if (px >= 0 && px < g_sw && py >= 0 && py < DOCK_H)
        g_fb[py * g_sw + px] = c;
    }
  }
}

/* dock_reinit - (re)create the dock window + buffer for a w x h desktop, pinned
 * to the bottom.  Used at start and on a resolution change (there is no userland
 * window-move primitive yet, so recreating at the new origin is the simplest
 * correct adaptation). */
static void dock_reinit(int sw, int sh) {
  g_sw = sw;
  g_sh = sh;
  uint32_t *nb = (uint32_t *)malloc((size_t)sw * DOCK_H * 4);
  if (nb) {
    free(g_fb);
    g_fb = nb;
  }
  if (g_win >= 0)
    destroy_window(g_win);
  g_win = create_window(0, sh - DOCK_H, sw, DOCK_H, "nxui");
  if (g_win >= 0)
    set_window_flags(g_win, 1); /* top_most → chromeless, always-on-top */
  g_sig = 0;                    /* force a redraw at the new size */
}

/* redraw - enumerate windows, rebuild the tile→window map, and blit the dock —
 * but only when the dock-relevant state actually changed (force=1 overrides).
 * Returns nothing; updates g_slot_* so clicks hit-test against the live layout. */
static void redraw(int force) {
  struct window_info wi[MAX_TILES];
  int n = (int)OS1_window_enum(wi, MAX_TILES);
  if (n < 0)
    n = 0;

  unsigned flags[MAX_TILES];
  int cnt = 0;
  int x = MARGIN;
  unsigned sig = 2166136261u ^ (unsigned)g_sw;
  for (int i = 0; i < n && cnt < MAX_TILES; i++) {
    /* our own dock + system overlays (notifications) are never tiled */
    if (wi[i].id == g_win)
      continue;
    if (wi[i].flags & (WININFO_TOPMOST | WININFO_PASSIVE))
      continue;
    /* show real apps: shown windows, or ones the user backgrounded.  Skip a
     * window an app hid itself (hidden but not user-minimized). */
    if (!(wi[i].flags & WININFO_VISIBLE) && !(wi[i].flags & WININFO_MINIMIZED))
      continue;
    if (x + TILE > g_sw - MARGIN)
      break; /* dock full */

    g_slot_id[cnt] = wi[i].id;
    g_slot_x[cnt] = x;
    flags[cnt] = wi[i].flags;
    sig = (sig ^ (unsigned)wi[i].id) * 16777619u;
    sig = (sig ^ (wi[i].flags & (WININFO_FOCUSED | WININFO_MINIMIZED))) *
          16777619u;
    cnt++;
    x += TILE + TILE_GAP;
  }
  g_slot_n = cnt;

  if (!force && sig == g_sig)
    return; /* nothing the dock shows has changed → skip the blit */
  g_sig = sig;

  fb_fill(COL_DOCK_BG);
  int ty = (DOCK_H - TILE) / 2;
  for (int i = 0; i < cnt; i++) {
    uint32_t c = COL_TILE;
    if (flags[i] & WININFO_FOCUSED)
      c = COL_TILE_FOCUS;
    else if (flags[i] & WININFO_MINIMIZED)
      c = COL_TILE_MIN;
    fb_rrect(g_slot_x[i], ty, TILE, TILE, TILE_RADIUS, c);
    /* running indicator: a small dot under the focused app's tile */
    if (flags[i] & WININFO_FOCUSED)
      fb_rrect(g_slot_x[i] + TILE / 2 - 2, ty + TILE + 3, 4, 3, 1,
               COL_TILE_FOCUS);
  }
  window_blit(g_win, 0, 0, g_sw, DOCK_H, g_fb);
}

/* hit_slot - map a dock-local click x to a window id, or -1. */
static int hit_slot(int rx) {
  for (int i = 0; i < g_slot_n; i++)
    if (rx >= g_slot_x[i] && rx < g_slot_x[i] + TILE)
      return g_slot_id[i];
  return -1;
}

/* act - toggle a tile: restore if backgrounded, background if focused, else
 * focus.  Re-reads the window's current state so the toggle is correct. */
static void act(int id) {
  struct window_info wi[MAX_TILES];
  int n = (int)OS1_window_enum(wi, MAX_TILES);
  unsigned int f = 0;
  for (int i = 0; i < n; i++)
    if (wi[i].id == id) {
      f = wi[i].flags;
      break;
    }
  if (f & WININFO_MINIMIZED)
    OS1_window_restore(id);
  else if (f & WININFO_FOCUSED)
    OS1_window_minimize(id);
  else
    OS1_window_focus(id);
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

  long di = _sys_display_info();
  int sw = (int)((di >> 16) & 0xFFFF);
  int sh = (int)(di & 0xFFFF);
  if (sw <= 0)
    sw = 800;
  if (sh <= 0)
    sh = 600;

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
     * event-driven INPUT_TYPE_RESIZE path below is wired system-wide). */
    long d = _sys_display_info();
    int cw = (int)((d >> 16) & 0xFFFF), ch = (int)(d & 0xFFFF);
    if (cw > 0 && ch > 0 && (cw != g_sw || ch != g_sh))
      dock_reinit(cw, ch);

    redraw(0);

    /* Standard window input: clicks arrive as INPUT_TYPE_MOUSE while the dock
     * holds focus (clicking a tile both focuses the dock — so the event is
     * delivered here — and carries dock-local coordinates). */
    input_event_t ev;
    while (input_poll_event(&ev) == 1) {
      if (ev.type == INPUT_TYPE_MOUSE) {
        if (ev.mouse.state == KEY_PRESSED && ev.mouse.button == MOUSE_BTN_LEFT) {
          int id = hit_slot(ev.mouse.x);
          if (id > 0)
            act(id);
        }
      } else if (ev.type == INPUT_TYPE_RESIZE) {
        /* event-driven adaptation: the system told us the geometry changed. */
        long d2 = _sys_display_info();
        int rw = (int)((d2 >> 16) & 0xFFFF), rh = (int)(d2 & 0xFFFF);
        if (rw > 0 && rh > 0)
          dock_reinit(rw, rh);
      }
    }

    OS1_sleep(33); /* ~30 Hz poll; descheduled between ticks, never busy-spins */
  }
  return 0;
}
