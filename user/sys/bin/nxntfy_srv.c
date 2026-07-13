/*
 * user/sys/bin/nxntfy_srv.c
 * System-wide Notification Server + UI (nxntfy_srv): IPC popup service + a
 * registry-visible log of recent notifications (read by the nxnotify CLI).
 *
 * Maintains a small always-on-top, PASSIVE (click-through) compositor window in
 * the top-right corner of the screen.  The window is initially hidden; it
 * becomes visible when an IPC message of type IPC_TYPE_NOTIFY arrives, and is
 * automatically hidden again after 2 seconds.
 *
 * Render gating (USR-NOTIFY-03): the popup is shown ONLY on a freshly received
 * NOTIFY (gated on the recv return code), never on msg.type alone —
 * IPC_TYPE_RAW is 0 and collides with a "nothing waiting" buffer, which used to
 * re-render the stale payload forever (the popup never auto-hid) and turned a
 * click (delivered as IPC_TYPE_MOUSE) into a stray rendered glyph.
 *
 * Discovery mechanism (see USR-SEC-01):
 *   On startup, notify_srv writes its own PID as a decimal string to the
 *   global registry key "srv.notify_pid".  Any caller that wants to send a
 *   notification reads this key via registry_read() and sends an IPC message
 *   to that PID.  There is no authentication: any process can overwrite
 *   "srv.notify_pid" and redirect all notifications to itself.
 *
 * Event loop design (USR-NOTIFY-02 #134):
 *   Idle, the loop BLOCKS on recv(-1, &msg) so it consumes ~0% CPU until a
 *   notification arrives (the old try_recv()+yield() loop busy-spun a core).
 *   While the popup is visible it polls with try_recv() and a real blocking
 *   OS1_sleep(100) so the 2 s auto-hide can still fire, then drops back to the
 *   blocking branch. A recv-with-timeout (issue #135) would unify both.
 *
 * Known issues:
 *   USR-SEC-01  (W3 SECURITY) registry_write("srv.notify_pid", ...) has no
 *               authentication; any process can overwrite this key to intercept
 *               or forge system notifications.
 *   USR-BLOAT-01/02 (W2 BAD-IMPL·PERF) The ELF is ~500KB because lib.o
 *               bundles stb_image/stb_easy_font unconditionally and debug
 *               DWARF is not stripped.
 */
#include <font_lib.h>
#include <input.h>
#include <os1.h>
#include <string.h>

#include "nxres.h" /* nxres_theme_is_light(), IPC_LOOK_PING_MAGIC (posix_types.h) — see palette below */

/* ------------------------------------------------------------------ */
/* Adaptive geometry – aligned with nxbar                              */
/* ------------------------------------------------------------------ */
#define NOTIFY_WIDTH 280
#define NOTIFY_HEIGHT 56
#define NOTIFY_RADIUS 10
/* NOTIFY-RESIZE-01: idle-loop poll cadence (see the event-loop comment below)
 * — bounded so a host display resize is noticed within ~1/3 s even with the
 * popup hidden and no notification activity, while staying near-0% CPU. */
#define IDLE_POLL_MS 300

/* Matches nxbar's BAR_MARGIN_SIDE and BAR_H */
#define BAR_MARGIN_SIDE 7
#define BAR_H 26
#define TOP_GAP 4 /* space between bar and popup */

/* ------------------------------------------------------------------ */
/* Glass colour palette (same as nxbar) — COL_BG/COL_TEXT/COL_TITLE track
 * theme.color (nxres.h); COL_ERROR_BG/COL_WARN_BG are severity colours and
 * stay fixed across both themes, same reasoning as nxbar's badge/glyph. */
/* ------------------------------------------------------------------ */
static uint32_t g_col_bg;
static uint32_t g_col_text;
static uint32_t g_col_title;
#define COL_ERROR_BG 0xD0FF3B30u /* red tint */
#define COL_WARN_BG 0xD0FF9500u  /* amber tint */
/* WARN/ERROR popups always sit on their OWN fixed, saturated background
 * (COL_WARN_BG/COL_ERROR_BG above) — independent of theme.color, same
 * reasoning as nxbar's badge/glyph staying fixed.  Their text must follow
 * THAT background, not the desktop theme: white reads on both amber and
 * red regardless of whether the desktop is in light or dark mode. Using
 * the theme-derived g_col_title/g_col_text there (this file's first light-
 * theme pass) flipped it to near-black in light mode, which is low-contrast
 * on the red/amber tint — the bug the "manca tema light" report was about. */
#define COL_SEV_TEXT 0xFFFFFFFFu

static void nxntfy_load_palette(int light) {
  if (light) {
    g_col_bg = 0xE8F5F5F7u;    /* semi-transparent light */
    g_col_text = 0xFF1C1C1Eu;
    g_col_title = 0xFF000000u;
  } else {
    g_col_bg = 0xE81C1C24u;    /* semi-transparent dark */
    g_col_text = 0xFFF0F0F5u;
    g_col_title = 0xFFFFFFFFu;
  }
}

/* ============================================================
 *           Pixel + font helpers (same technique as nxbar)
 * ============================================================ */
static uint32_t *g_fb;
static int g_bw, g_bh; /* buffer (window) size */
static struct font_ctx *g_font;

/* fb_rrect - filled rounded rectangle (corners clipped to a quarter-circle of
 * radius r).  Same technique as nxui's fb_rrect (GFX-NXBAR-04 / GFX-NXUI-04):
 * relies on the compositor honoring per-pixel alpha on window_blit. */
static void fb_rrect(int x, int y, int w, int h, int r, uint32_t c) {
  for (int j = 0; j < h; j++) {
    for (int i = 0; i < w; i++) {
      int cx = -1, cy = 0;
      if (i < r && j < r) {
        cx = r;
        cy = r;
      } else if (i >= w - r && j < r) {
        cx = w - r - 1;
        cy = r;
      } else if (i < r && j >= h - r) {
        cx = r;
        cy = h - r - 1;
      } else if (i >= w - r && j >= h - r) {
        cx = w - r - 1;
        cy = h - r - 1;
      }
      if (cx >= 0) {
        int dx = i - cx, dy = j - cy;
        if (dx * dx + dy * dy > r * r)
          continue;
      }
      int px = x + i, py = y + j;
      if (px >= 0 && px < g_bw && py >= 0 && py < g_bh)
        g_fb[py * g_bw + px] = c;
    }
  }
}

static void buf_draw_glyph(int x, int y, uint32_t cp, uint32_t color) {
  if (!g_font)
    return;
  int idx = (int)cp - g_font->header.first_char;
  if (idx < 0 || idx >= g_font->header.num_chars)
    return;
  struct font_glyph_info *gi = &g_font->glyphs[idx];
  uint8_t *bitmap = g_font->bitmap + gi->data_offset;
  int sx = x + gi->x0, sy = y + g_font->header.ascent + gi->y0;
  for (int gy = 0; gy < gi->height; gy++) {
    for (int gx = 0; gx < gi->width; gx++) {
      if (bitmap[gy * gi->width + gx] > 64) {
        int px = sx + gx, py = sy + gy;
        if (px >= 0 && px < g_bw && py >= 0 && py < g_bh)
          g_fb[py * g_bw + px] = color;
      }
    }
  }
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

/* ------------------------------------------------------------------ */
/* Warning dedup — consecutive-repeat suppression, WARNINGS ONLY        */
/* ------------------------------------------------------------------ */
/* Per-process "last warning text seen" table.  A warning (severity 1) whose
 * text is IDENTICAL to the last one already recorded for the SAME sender
 * pid is a repeat of something already visible in nxbar's panel — skipped
 * entirely (no second ring entry) so a process stuck emitting the same
 * warning in a loop cannot flood the panel with copies of one line.  A
 * DIFFERENT warning from the same process, or the same text from a
 * different process, is never suppressed — nothing distinct is ever
 * dropped.  Errors and plain notifications never go through this table. */
#define WARN_DEDUP_MAX 16
static int g_warn_pid[WARN_DEDUP_MAX];
static char g_warn_text[WARN_DEDUP_MAX][56];
static int g_warn_n;      /* slots in use */
static int g_warn_next;   /* rotating eviction cursor once the table is full */

static int warn_is_dup(int pid, const char *text) {
  for (int i = 0; i < g_warn_n; i++)
    if (g_warn_pid[i] == pid)
      return strncmp(g_warn_text[i], text, sizeof(g_warn_text[i])) == 0;
  return 0;
}

static void warn_remember(int pid, const char *text) {
  for (int i = 0; i < g_warn_n; i++) {
    if (g_warn_pid[i] == pid) {
      snprintf(g_warn_text[i], sizeof(g_warn_text[i]), "%s", text);
      return;
    }
  }
  int idx;
  if (g_warn_n < WARN_DEDUP_MAX) {
    idx = g_warn_n++;
  } else {
    idx = g_warn_next; /* table full: evict the oldest tracked process */
    g_warn_next = (g_warn_next + 1) % WARN_DEDUP_MAX;
  }
  g_warn_pid[idx] = pid;
  snprintf(g_warn_text[idx], sizeof(g_warn_text[idx]), "%s", text);
}

/* ------------------------------------------------------------------ */
/* Window management – adaptive position (like nxbar's bar_reinit)      */
/* ------------------------------------------------------------------ */
static int g_win = -1;
static int g_sw, g_sh; /* last seen screen size */

static void notif_reinit(int sw, int sh) {
  g_sw = sw;
  g_sh = sh;
  g_bw = NOTIFY_WIDTH;
  g_bh = NOTIFY_HEIGHT;

  /* Position: right edge – margin, just below the top bar */
  int wx = sw - g_bw - BAR_MARGIN_SIDE;
  int wy = BAR_MARGIN_SIDE + BAR_H +
           TOP_GAP; /* nxbar top margin + bar height + gap */

  if (g_win >= 0)
    destroy_window(g_win);
  free(g_fb);
  g_fb = (uint32_t *)malloc((size_t)g_bw * g_bh * 4);
  if (!g_fb)
    return;
  memset(g_fb, 0, (size_t)g_bw * g_bh * 4);

  g_win = create_window(wx, wy, g_bw, g_bh, "nxntfy_srv");
  if (g_win >= 0)
    set_window_flags(g_win, 1 | 4 | 8); /* top_most, hidden, passive */
}

/*
 * main - notification server entry point; does not return.
 *
 * Creates a top-most, initially hidden compositor window.  Registers this
 * process's PID in the registry so callers can discover the endpoint.
 * Enters the event loop:
 *   1. try_recv: if a NOTIFY or RAW IPC message arrives, render its payload
 *      (up to 64 bytes) in the window and show it.
 *   2. Auto-hide: if the window has been visible for >2000 ms, hide it.
 *   3. Paced sleep while visible.
 *
 * Returns 1 on window creation failure, never returns otherwise.
 *
 * Side effects: creates a compositor window; writes registry key
 *   "srv.notify_pid"; draws to the window on each notification.
 *
 * NOTE(USR-SEC-01): Writing to "srv.notify_pid" is unauthenticated; any
 *   process can overwrite it and intercept subsequent notify() calls.
 */
int main(void) {
  g_font = font_load("/fonts/Rewir-Light.off");
  nxntfy_load_palette(nxres_theme_is_light());

  long di = OS1_display_info();
  int sw = (int)((di >> 16) & 0xFFFF), sh = (int)(di & 0xFFFF);
  if (sw <= 0)
    sw = 800;
  if (sh <= 0)
    sh = 600;
  notif_reinit(sw, sh);
  if (g_win < 0) {
    printf("[Notify] Failed to create window\n");
    return 1;
  }

  printf("[Notify] Server started (PID %d)\n", get_pid());

  /* NOTE(NOTIFY-REG-01): srv.notify_pid is now published by init (parent),
   * not by the server itself.  Self-publishing broke respawn: after the
   * server died and init spawned a fresh copy, the registry key still held
   * the corpse's pid and every notify_post() lost its messages.  Init owns
   * the key (it knows the live pid on every spawn + respawn), and re-publishes
   * it after each respawn so any registry hijack between respawns is undone.
   * Discovery of this endpoint from a caller:
   * OS1_registry_get("srv.notify_pid").
   */

  struct ipc_message msg;
  long last_notify_time = 0;
  int is_visible = 0; /* Tracks whether the window is currently shown */
  int ring_idx = 0;   /* registry log ring head: sys.ntfy.log.<i>, 16 deep */
  /* Last logged record, kept so the auto-hide can flip its state to READ (the
   * read receipt): record format is "<from>|<sev>|<state>|<text>". */
  int last_log_idx = -1, last_from = 0, last_sev = 0;
  char last_text[56] = {0};

  /* Event loop (USR-NOTIFY-02 #134): never busy-spin.
   *   - Idle (window hidden): poll at a LOW rate (see IDLE_POLL_MS below) so
   *     the process is descheduled between ticks (near-0% CPU) but still
   *     notices a host display resize without needing a live notification.
   *   - Visible (auto-hide pending): we cannot block forever or the 2 s hide
   *     would never fire, so poll with try_recv() and a REAL blocking
   * OS1_sleep(100) — 10 wakeups/s, not a yield-spin — until the window hides
   * and we drop back to the idle branch. A recv-with-timeout would collapse
   * both branches; that needs the capability timer objects of issue #135. */
  while (1) {
    /* Adaptive resolution check (like nxbar) */
    long d = OS1_display_info();
    int cw = (int)((d >> 16) & 0xFFFF), ch = (int)(d & 0xFFFF);
    if (cw > 0 && ch > 0 && (cw != g_sw || ch != g_sh))
      notif_reinit(cw, ch);

    /* `got` is true ONLY when a message was actually received this round.
     * Gate the popup on the receive RETURN CODE, never on msg.type alone:
     * IPC_TYPE_RAW == 0 (posix_types.h) collides with a zeroed / "nothing
     * waiting" buffer, so the old `msg.type == IPC_TYPE_RAW` test re-rendered
     * the STALE payload on every empty poll — the popup never auto-hid (each
     * re-render reset last_notify_time), and a click on the popup delivered an
     * IPC_TYPE_MOUSE message whose coordinate bytes were then rendered as a
     * stray glyph. */
    int got;
    if (!is_visible) {
      /* FIX(NOTIFY-RESIZE-01): this used to be a plain blocking recv(-1,&msg)
       * — zero CPU while idle, but it ALSO froze the "adaptive resolution
       * check" above: that check only runs once per loop iteration, and a
       * blocking recv() means the NEXT iteration (and therefore the next
       * resize check) does not happen until a real notification arrives to
       * wake it. A host display resize that lands while idle (the common
       * state — the popup is hidden almost all the time) was invisible to
       * this process until something else woke it, leaving the popup's
       * window stuck at its OLD position/possibly off-screen until the next
       * notification "touched" it. Poll at IDLE_POLL_MS instead of blocking
       * forever: still near-0% CPU (a ~3 Hz sleep-based wait, same technique
       * nxbar/nxui already use at 30 Hz for their own resize polling), but
       * the resize check now runs on a bounded cadence regardless of
       * notification activity. */
      got = (try_recv(-1, &msg) == 0);
      if (!got) {
        OS1_sleep(IDLE_POLL_MS);
        continue;
      }
    } else {
      /* Visible: non-blocking poll so the 2 s auto-hide can still fire. */
      got = (try_recv(-1, &msg) == 0);
    }

    /* A look-changed ping (nxres_broadcast_look, nxres.h) rides the SAME
     * IPC_TYPE_NOTIFY transport as a real user notification, tagged via
     * data2 so it never gets mistaken for one — no popup, no ring log,
     * just refresh this window's own palette (it draws itself with the
     * same glass colours as nxbar/nxsettings, so it needs to react too). */
    if (got && msg.type == IPC_TYPE_NOTIFY &&
        msg.data2 == IPC_LOOK_PING_MAGIC) {
      nxntfy_load_palette(nxres_theme_is_light());
      continue;
    }

    /* Only a freshly-received NOTIFY shows the popup. Any other message type
     * (e.g. IPC_TYPE_MOUSE from a click on the window) is drained and ignored.
     */
    if (got && msg.type == IPC_TYPE_NOTIFY) {
      int is_warning = (msg.data1 == 1);

      /* Exact repeat of the last warning from THIS process: already visible
       * in nxbar's panel from its first occurrence — skip entirely (no
       * second ring entry either) instead of piling up identical lines.
       * Errors and plain notifications are never deduped. */
      if (is_warning && warn_is_dup((int)msg.from, msg.payload))
        continue;
      if (is_warning)
        warn_remember((int)msg.from, msg.payload);

      /* Colour the popup by severity (msg.data1): 2 = error (red), 1 = warning
       * (amber), else info.  The kernel posts data1=2 for a userland crash;
       * userland may post data1=1 for warnings.  WARN/ERROR sit on their own
       * fixed, saturated tint (COL_WARN_BG/COL_ERROR_BG) — text follows THAT
       * background (COL_SEV_TEXT, always legible), not the desktop theme;
       * only the neutral info background (g_col_bg) uses the theme-derived
       * title/text colours. */
      uint32_t bg = g_col_bg;
      uint32_t text_col = g_col_text;
      uint32_t title_col = g_col_title;
      const char *title = "Notification";
      if (msg.data1 == 2) {
        bg = COL_ERROR_BG;
        title = "ERROR";
        text_col = COL_SEV_TEXT;
        title_col = COL_SEV_TEXT;
      } else if (is_warning) {
        bg = COL_WARN_BG;
        title = "WARN";
        text_col = COL_SEV_TEXT;
        title_col = COL_SEV_TEXT;
      }

      /* Warnings are reported ONLY in nxbar's notification panel (the ring
       * log below) — never as an on-screen popup, so a process emitting
       * several DIFFERENT warnings in a row does not toast-spam the desktop
       * the way an error or a plain notification legitimately still does
       * (those are rarer and worth interrupting for). */
      int popup_shown = 0;
      if (!is_warning) {
        /* Non mostriamo il popup se il pannello notifiche di nxbar è già aperto.
         */
        char panel_flag[2] = {0};
        int panel_open = (OS1_registry_get("sys.ntfy.panel_open", panel_flag,
                                           sizeof(panel_flag)) == 0 &&
                          panel_flag[0] == '1');

        if (!panel_open) {
          /* Render with modern glass style */
          if (g_fb) {
            fb_rrect(0, 0, g_bw, g_bh, NOTIFY_RADIUS, bg);
            buf_draw_text(12, 10, title, title_col);
            buf_draw_text(12, 30, msg.payload, text_col);
            window_blit(g_win, 0, 0, g_bw, g_bh, g_fb);
          }

          /* Show window: 1=top_most, 2=visible, 8=passive (click-through, no
           * focus). */
          set_window_flags(g_win, 1 | 2 | 8);
          is_visible = 1;
          popup_shown = 1;
          last_notify_time = get_time();
          compositor_render();
        }
      }

      /* Log to a bounded registry ring so notifications are VISIBLE in the
       * namespace and readable by the nxnotify CLI (ASTRA: everything is a
       * registry node).  16 entries cap the buffer (anti-spam / fixed size).
       * Record = "<from>|<sev>|<state>|<text>": carries the sender PID (group
       * by process), severity, and the receipt state (U=unread/received,
       * R=read) — the registry-message model.  Always written (warnings
       * included, so nxbar's panel/badge see them) regardless of whether a
       * popup was shown. */
      {
        int idx = ring_idx & 0x0F;
        char key[24], rec[80];
        snprintf(key, sizeof(key), "sys.ntfy.log.%d", idx);
        snprintf(rec, sizeof(rec), "%d|%d|U|%.48s", (int)msg.from,
                 (int)msg.data1, msg.payload);
        OS1_registry_set(key, rec);
        ring_idx++;

        /* The pending-read-receipt bookkeeping below (flipped to 'R' by the
         * auto-hide branch when the ON-SCREEN POPUP disappears) must track
         * whichever ring entry that popup is actually showing — NOT
         * unconditionally the latest ring write.  A warning (or a
         * panel-open-suppressed notification) never shows a popup, so it
         * must never steal this slot from whatever IS currently on screen:
         * if it did, the auto-hide would mark the WRONG entry 'R' — the
         * true source of the popup would stay 'unread' forever, and this
         * unrelated entry would be marked 'read' despite never having been
         * shown. */
        if (popup_shown) {
          last_log_idx = idx;
          last_from = (int)msg.from;
          last_sev = (int)msg.data1;
          int t = 0;
          for (const char *p = msg.payload; *p && t < 55; p++)
            last_text[t++] = *p;
          last_text[t] = '\0';
        }
      }
    }

    /* Auto-hide 2 s (2000 ms; get_time() is real ms) after the last
     * notification, then the next iteration blocks on recv() again. */
    if (is_visible && (get_time() - last_notify_time >= 2000)) {
      set_window_flags(g_win, 1 | 4 | 8); /* 1=top_most, 4=hidden, 8=passive */
      is_visible = 0;
      compositor_render();
      /* The user has now seen the popup: confirm READ in the registry record
       * (the read receipt of the message model). */
      if (last_log_idx >= 0) {
        char key[24], rec[80];
        snprintf(key, sizeof(key), "sys.ntfy.log.%d", last_log_idx);
        snprintf(rec, sizeof(rec), "%d|%d|R|%.48s", last_from, last_sev,
                 last_text);
        OS1_registry_set(key, rec);
        last_log_idx = -1;
      }
    }

    /* While the popup is up, pace the auto-hide poll with a real blocking sleep
     * (descheduled, no busy-wait). Skipped once we are hidden — that path
     * blocks on recv() at the top instead. */
    if (is_visible)
      OS1_sleep(100);
  }

  return 0;
}