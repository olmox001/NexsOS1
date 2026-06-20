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
 * The dock is itself a top_most compositor window (chromeless: a top_most
 * window has no titlebar/buttons), drawn by blitting a self-rendered ARGB
 * buffer.
 *
 * NOTE(GFX-NXUI-01, non-blocking): live re-layout on a resolution change is not
 * wired yet (no userland window-move primitive); the dock sizes to the desktop
 * at start.  NOTE(GFX-NXUI-02): tiles are blank rounded squares — icon/initial
 * rendering needs the compositor font exposed to userland buffers (future).
 */
#include <os1.h>
#include <string.h>

#define DOCK_H 56     /* dock height in px                                   */
#define TILE 40       /* app tile size in px                                 */
#define TILE_GAP 12   /* gap between tiles                                   */
#define TILE_RADIUS 9 /* tile corner radius */
#define MARGIN 16     /* left/right inset                                    */
#define MAX_TILES 32  /* matches the compositor MAX_WINDOWS                  */

#define COL_DOCK_BG 0xE81C1C24u /* translucent dark slab                    */
#define COL_TILE 0xFF3A3A46u    /* idle app                                 */
#define COL_TILE_FOCUS 0xFF5E9CFFu /* focused app */
#define COL_TILE_MIN 0xFF2A2A32u   /* backgrounded (minimized) app   */

static uint32_t *g_fb; /* dock pixel buffer (g_sw x DOCK_H, ARGB)        */
static int g_sw, g_sh; /* desktop size                                   */
static int g_win = -1; /* the dock's own window id                       */

/* current tile→window mapping, rebuilt every redraw for click hit-testing */
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

/* redraw - enumerate windows, lay out one tile per app, blit the dock. */
static void redraw(void) {
  struct window_info wi[MAX_TILES];
  int n = (int)OS1_window_enum(wi, MAX_TILES);
  if (n < 0)
    n = 0;

  fb_fill(COL_DOCK_BG);

  g_slot_n = 0;
  int x = MARGIN;
  int ty = (DOCK_H - TILE) / 2;
  for (int i = 0; i < n && g_slot_n < MAX_TILES; i++) {
    /* skip our own dock window and other system overlays (notifications). */
    if (wi[i].id == g_win)
      continue;
    if (wi[i].flags & (WININFO_TOPMOST | WININFO_PASSIVE))
      continue;
    /* show real apps: shown windows, or ones the user sent to the background.
     * Skip windows an app hid itself (hidden but not user-minimized), e.g. the
     * notification server's window while it has nothing to show. */
    if (!(wi[i].flags & WININFO_VISIBLE) && !(wi[i].flags & WININFO_MINIMIZED))
      continue;
    if (x + TILE > g_sw - MARGIN)
      break; /* dock full */

    uint32_t c = COL_TILE;
    if (wi[i].flags & WININFO_FOCUSED)
      c = COL_TILE_FOCUS;
    else if (wi[i].flags & WININFO_MINIMIZED)
      c = COL_TILE_MIN;
    fb_rrect(x, ty, TILE, TILE, TILE_RADIUS, c);

    /* running indicator: a small dot under the focused app's tile. */
    if (wi[i].flags & WININFO_FOCUSED)
      fb_rrect(x + TILE / 2 - 2, ty + TILE + 3, 4, 3, 1, COL_TILE_FOCUS);

    g_slot_id[g_slot_n] = wi[i].id;
    g_slot_x[g_slot_n] = x;
    g_slot_n++;
    x += TILE + TILE_GAP;
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
  long di = _sys_display_info();
  g_sw = (int)((di >> 16) & 0xFFFF);
  g_sh = (int)(di & 0xFFFF);
  if (g_sw <= 0)
    g_sw = 800;
  if (g_sh <= 0)
    g_sh = 600;

  g_fb = (uint32_t *)malloc((size_t)g_sw * DOCK_H * 4);
  if (!g_fb)
    return 1;

  g_win = create_window(0, g_sh - DOCK_H, g_sw, DOCK_H, "nxui");
  if (g_win < 0)
    return 1;
  set_window_flags(g_win, 1); /* top_most → chromeless, always-on-top dock */

  for (;;) {
    redraw();

    /* Clicks arrive as IPC_TYPE_MOUSE while the dock holds focus — clicking a
     * tile both focuses the dock (so the event is delivered here) and carries
     * the dock-local coordinates in the payload. */
    struct ipc_message msg;
    while (try_recv(-1, &msg) == 0) {
      if (msg.type != IPC_TYPE_MOUSE)
        continue;
      int button = (int)msg.data1;
      int state = (int)msg.data2;
      if (state != 1 || button != 0) /* act on left-button press only */
        continue;
      int rx = 0, ry = 0;
      memcpy(&rx, msg.payload, 4);
      memcpy(&ry, msg.payload + 4, 4);
      int id = hit_slot(rx);
      if (id > 0)
        act(id);
    }

    OS1_sleep(120); /* ~8 Hz refresh; low CPU, snappy enough for a dock */
  }
  return 0;
}
