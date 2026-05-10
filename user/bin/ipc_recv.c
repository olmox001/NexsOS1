#include <os1.h>

#include <os1.h>

int main(void) {
  int win = create_window(50, 500, 300, 100, "IPC Recv");
  if (win <= 0)
    return 1;

  window_draw(win, 0, 0, 300, 100, 0xFF333333);
  flush();

  struct ipc_message msg;
  printf("[IPC Recv] Waiting for any message...\n");

  if (recv(-1, &msg) == 0) {
    printf("[IPC Recv] Received from PID %d: type=%d data1=%lx\n", msg.from,
           msg.type, msg.data1);

    /* Visual Indication */
    window_draw(win, 0, 0, 300, 100,
                0xFF00AA00); /* Green background on success */
    flush();

    // Keep window open for a bit
    sleep(200);
  } else {
    printf("[IPC Recv] Receive failed.\n");
    window_draw(win, 0, 0, 300, 100, 0xFFAA0000); /* Red on failure */
    flush();
    sleep(100);
  }

  return 0;
}
