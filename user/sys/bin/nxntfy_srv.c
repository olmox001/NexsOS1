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
 *   OS1_sleep(100) so the 5 s auto-hide can still fire, then drops back to the
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
#include <os1.h>

/* Notification popup window geometry (pixels).
 * Positioned at the top-right corner assuming an 720x1280 display. */
#define NOTIFY_WIDTH 250
#define NOTIFY_HEIGHT 60
#define NOTIFY_PADDING 10

/*
 * main - notification server entry point; does not return.
 *
 * Creates a top-most, initially hidden compositor window.  Registers this
 * process's PID in the registry so callers can discover the endpoint.
 * Enters the event loop:
 *   1. try_recv: if a NOTIFY or RAW IPC message arrives, render its payload
 *      (up to 64 bytes) in the window and show it.
 *   2. Auto-hide: if the window has been visible for >5000 jiffies (~5 s at
 *      100 Hz), hide it.
 *   3. yield() to give other processes CPU time.
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
  /* Create a window in the top-right corner.
   * Screensize is 720 * 1280
   */
  int win_id =
      create_window(720 - NOTIFY_WIDTH - NOTIFY_PADDING, NOTIFY_PADDING,
                    NOTIFY_WIDTH, NOTIFY_HEIGHT, "Notifiche");

  if (win_id < 0) {
    printf("[Notify] Failed to create window\n");
    return 1;
  }

  /* Set as top-most, HIDDEN initially, and PASSIVE (non-interactive).
   * Flag bits: 1 = top_most (always rendered above other windows),
   *            4 = hidden (window not shown until a notification arrives),
   *            8 = passive (click-through: never takes focus or input, so a
   *                click on the popup neither steals the caret nor delivers a
   *                mouse event to this server). */
  set_window_flags(win_id, 1 | 4 | 8); /* 1=top_most, 4=hidden, 8=passive */
  compositor_render();

  printf("[Notify] Server started (PID %d)\n", get_pid());

  /* NOTE(NOTIFY-REG-01): srv.notify_pid is now published by init (parent),
   * not by the server itself.  Self-publishing broke respawn: after the
   * server died and init spawned a fresh copy, the registry key still held
   * the corpse's pid and every notify_post() lost its messages.  Init owns
   * the key (it knows the live pid on every spawn + respawn), and re-publishes
   * it after each respawn so any registry hijack between respawns is undone.
   * Discovery of this endpoint from a caller: OS1_registry_get("srv.notify_pid").
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
   *   - Idle (window hidden): BLOCK on recv() — the process is descheduled and
   *     consumes ~0% CPU until a notification actually arrives. This is the
   *     server pattern (real blocking IPC, like init's blocking sleep).
   *   - Visible (auto-hide pending): we cannot block forever or the 5 s hide
   *     would never fire, so poll with try_recv() and a REAL blocking
   * OS1_sleep(100) — 10 wakeups/s, not a yield-spin — until the window hides
   * and we drop back to the blocking branch. A recv-with-timeout would collapse
   * both branches; that needs the capability timer objects of issue #135. */
  while (1) {
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
      /* Idle: block until a message arrives — zero CPU while hidden. */
      got = (recv(-1, &msg) == 0);
    } else {
      /* Visible: non-blocking poll so the 2 s auto-hide can still fire. */
      got = (try_recv(-1, &msg) == 0);
    }

    /* Only a freshly-received NOTIFY shows the popup. Any other message type
     * (e.g. IPC_TYPE_MOUSE from a click on the window) is drained and ignored.
     */
    if (got && msg.type == IPC_TYPE_NOTIFY) {
      /* Colour the popup by severity (msg.data1): 2 = error (red), 1 = warning
       * (amber), else info.  The kernel posts data1=2 for a userland crash;
       * userland may post data1=1 for warnings. */
      uint32_t bg = 0xFFFCFCFD; /* info: near-white */
      const char *hdr = "\033[1;90m [System Notification]";
      if (msg.data1 == 2) {
        bg = 0xFFFFD6D6; /* error: light red */
        hdr = "\033[1;91m [ERROR]";
      } else if (msg.data1 == 1) {
        bg = 0xFFFFF2C8; /* warning: light amber */
        hdr = "\033[1;33m [WARNING]";
      }
      window_draw(win_id, 0, 0, NOTIFY_WIDTH, NOTIFY_HEIGHT, bg);
      printf_win(win_id, "\033[H%s\033[0m\n", hdr);
      /* Limit payload to 64 bytes to avoid over-running the window. */
      printf_win(win_id, "%.64s\n", msg.payload);

      /* Show window: 1=top_most, 2=visible, 8=passive (click-through, no
       * focus). */
      set_window_flags(win_id, 1 | 2 | 8);
      is_visible = 1;
      last_notify_time = get_time();
      compositor_render();

      /* Log to a bounded registry ring so notifications are VISIBLE in the
       * namespace and readable by the nxnotify CLI (ASTRA: everything is a
       * registry node).  16 entries cap the buffer (anti-spam / fixed size).
       * Record = "<from>|<sev>|<state>|<text>": carries the sender PID (group
       * by process), severity, and the receipt state (U=unread/received,
       * R=read) — the registry-message model. */
      {
        int idx = ring_idx & 0x0F;
        char key[24], rec[80];
        snprintf(key, sizeof(key), "sys.ntfy.log.%d", idx);
        snprintf(rec, sizeof(rec), "%d|%d|U|%.48s", (int)msg.from,
                 (int)msg.data1, msg.payload);
        OS1_registry_set(key, rec);
        last_log_idx = idx;
        last_from = (int)msg.from;
        last_sev = (int)msg.data1;
        int t = 0;
        for (const char *p = msg.payload; *p && t < 55; p++)
          last_text[t++] = *p;
        last_text[t] = '\0';
        ring_idx++;
      }
    }

    /* Auto-hide 2 s (2000 ms; get_time() is real ms) after the last
     * notification, then the next iteration blocks on recv() again. */
    if (is_visible && (get_time() - last_notify_time >= 2000)) {
      set_window_flags(win_id, 1 | 4 | 8); /* 1=top_most, 4=hidden, 8=passive */
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
