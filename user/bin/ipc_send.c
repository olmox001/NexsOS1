#include <os1.h>

int main(void) {
  int win = create_window(400, 500, 300, 100, "IPC Send");
  if (win <= 0)
    return 1;

  window_draw(win, 0, 0, 300, 100, 0xFF333333);
  compositor_render();

  struct ipc_message msg;
  msg.type = 1;
  msg.data1 = 0xDEADBEEF;
  msg.data2 = 0xCAFEBABE;
  for (int i = 0; i < 63; i++) {
    msg.payload[i] = 'A' + (i % 26);
  }
  msg.payload[63] = '\0';

  /* Get IPC recv PID from registry or use default */
  char pid_buf[16];
  int target_pid = 3; /* Default */
  if (OS1_registry_get("demo.ipc_recv_pid", pid_buf, sizeof(pid_buf)) == 0) {
    target_pid = 0;
    for (int j = 0; pid_buf[j] >= '0' && pid_buf[j] <= '9'; j++) {
      target_pid = target_pid * 10 + (pid_buf[j] - '0');
    }
  }

  printf("[IPC Send] Sending message to PID %d...\n", target_pid);

  if (send(target_pid, &msg) == 0) {
    printf("[IPC Send] Message sent successfully!\n");
    window_draw(win, 0, 0, 300, 100, 0xFF00AA00); /* Green */
    compositor_render();
    OS1_sleep(1000); /* keep the result window up ~1s (OS1_sleep() is now ms) */
  } else {
    printf("[IPC Send] Send failed.\n");
    window_draw(win, 0, 0, 300, 100, 0xFFAA0000); /* Red */
    compositor_render();
    OS1_sleep(1000); /* keep the result window up ~1s (OS1_sleep() is now ms) */
  }

  return 0;
}
