#include <os1.h>

int main(void) {
  int win = create_window(50, 500, 300, 100, "IPC Recv");
  if (win <= 0)
    return 1;

  window_draw(win, 0, 0, 300, 100, 0xFF333333);
  flush();

  /* Publish our PID so ipc_send can find us: it reads demo.ipc_recv_pid and
   * falls back to PID 3 (an idle task) when unset, which made the pair
   * useless as an end-to-end test. */
  {
    char pid_buf[16];
    int pid = get_pid();
    int n = 0;
    char rev[16];
    do {
      rev[n++] = '0' + (pid % 10);
      pid /= 10;
    } while (pid > 0 && n < 15);
    for (int j = 0; j < n; j++)
      pid_buf[j] = rev[n - 1 - j];
    pid_buf[n] = '\0';
    OS1_registry_set("demo.ipc_recv_pid", pid_buf);
  }

  struct ipc_message msg;
  printf("[IPC Recv] Waiting for any message...\n");

  if (recv(-1, &msg) == 0) {
    printf("[IPC Recv] Received from PID %d: type=%d data1=%lx\n", msg.from,
           msg.type, msg.data1);

    /* Visual Indication */
    window_draw(win, 0, 0, 300, 100,
                0xFF00AA00); /* Green background on success */
    flush();

    // Keep window open for a bit (~2s; OS1_sleep() is now milliseconds)
    OS1_sleep(2000);
  } else {
    printf("[IPC Recv] Receive failed.\n");
    window_draw(win, 0, 0, 300, 100, 0xFFAA0000); /* Red on failure */
    flush();
    OS1_sleep(1000); /* ~1s; OS1_sleep() is now milliseconds */
  }

  return 0;
}
