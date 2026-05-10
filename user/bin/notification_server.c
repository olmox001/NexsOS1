/*
 * user/notification_server.c
 * System-wide Notification Server (Microkernel Style)
 */
#include <os1.h>

#define NOTIFY_WIDTH 250
#define NOTIFY_HEIGHT 60
#define NOTIFY_PADDING 10

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

  /* Set as top-most and HIDDEN initially */
  set_window_flags(win_id, 1 | 4); /* 1=top_most, 4=hidden */
  compositor_render();

  printf("[Notify] Server started (PID %d)\n", get_pid());

  /* Register PID in system registry for other processes to find it */
  char pid_str[16];
  snprintf(pid_str, sizeof(pid_str), "%d", get_pid());
  registry_write("srv.notify_pid", pid_str);

  struct ipc_message msg;
  long last_notify_time = 0;
  int is_visible = 0;

  while (1) {
    /* Non-blocking receive for auto-hide check */
    if (try_recv(-1, &msg) == 0) {
      if (msg.type == IPC_TYPE_NOTIFY || msg.type == IPC_TYPE_RAW) {
        /* UI Drawing */
        window_draw(win_id, 0, 0, NOTIFY_WIDTH, NOTIFY_HEIGHT, 0xEE222222);
        printf_win(win_id, "\033[H\033[1;93m [System Notification]\033[0m\n ");
        printf_win(win_id, "%.64s\n", msg.payload);

        /* Show window */
        set_window_flags(win_id, 1 | 2); /* 1=top_most, 2=visible */
        is_visible = 1;
        last_notify_time = get_time();
        compositor_render();
      }
    }

    /* Auto-hide check: Hide after 5 seconds */
    if (is_visible && (get_time() - last_notify_time > 5000)) {
      set_window_flags(win_id, 1 | 4); /* 1=top_most, 4=hidden */
      is_visible = 0;
      compositor_render();
    }

    /* Yield to prevent CPU hogging */
    yield();
  }

  return 0;
}
