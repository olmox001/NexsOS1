/*
 * user/sys/bin/notification_server.c
 * System-wide Notification Server (IPC-based popup service)
 *
 * Maintains a small always-on-top compositor window in the top-right corner
 * of the screen.  The window is initially hidden; it becomes visible when an
 * IPC message of type IPC_TYPE_NOTIFY or IPC_TYPE_RAW arrives, and is
 * automatically hidden again after 5 seconds of inactivity.
 *
 * Discovery mechanism (see USR-SEC-01):
 *   On startup, notify_srv writes its own PID as a decimal string to the
 *   global registry key "srv.notify_pid".  Any caller that wants to send a
 *   notification reads this key via registry_read() and sends an IPC message
 *   to that PID.  There is no authentication: any process can overwrite
 *   "srv.notify_pid" and redirect all notifications to itself.
 *
 * Event loop design:
 *   try_recv(-1, &msg) is the NON-BLOCKING IPC check (syscall #32).
 *   Returns 0 on success (message available), negative if no message is
 *   waiting.  The loop then checks the auto-hide timer and calls yield().
 *   This is a correct, non-busy-spinning event loop.
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

  /* Set as top-most and HIDDEN initially.
   * Flag bits: 1 = top_most (always rendered above other windows),
   *            4 = hidden (window not shown until a notification arrives). */
  set_window_flags(win_id, 1 | 4); /* 1=top_most, 4=hidden */
  compositor_render();

  printf("[Notify] Server started (PID %d)\n", get_pid());

  /* Register PID in system registry for other processes to find it.
   * NOTE(USR-SEC-01): No authentication; any process can overwrite this key
   * to redirect all system notifications to itself. */
  char pid_str[16];
  snprintf(pid_str, sizeof(pid_str), "%d", get_pid());
  registry_write("srv.notify_pid", pid_str);

  struct ipc_message msg;
  long last_notify_time = 0;
  int is_visible = 0;    /* Tracks whether the window is currently shown */

  while (1) {
    /* Non-blocking receive: try_recv returns 0 if a message was dequeued,
     * negative if the queue is empty.  This avoids blocking the auto-hide
     * timer check below. */
    if (try_recv(-1, &msg) == 0) {
      if (msg.type == IPC_TYPE_NOTIFY || msg.type == IPC_TYPE_RAW) {
        /* Render notification background and payload text. */
        window_draw(win_id, 0, 0, NOTIFY_WIDTH, NOTIFY_HEIGHT, 0xEE222222);
        printf_win(win_id, "\033[H\033[1;93m [System Notification]\033[0m\n ");
        /* Limit payload to 64 bytes to avoid over-running the window. */
        printf_win(win_id, "%.64s\n", msg.payload);

        /* Show window: flag 2 = visible. */
        set_window_flags(win_id, 1 | 2); /* 1=top_most, 2=visible */
        is_visible = 1;
        last_notify_time = get_time();
        compositor_render();
      }
    }

    /* Auto-hide check: Hide after 5 seconds (5000 jiffies at 100 Hz timer). */
    if (is_visible && (get_time() - last_notify_time > 5000)) {
      set_window_flags(win_id, 1 | 4); /* 1=top_most, 4=hidden */
      is_visible = 0;
      compositor_render();
    }

    /* Yield to prevent CPU hogging; allows other processes to run between
     * non-blocking poll iterations. */
    yield();
  }

  return 0;
}
