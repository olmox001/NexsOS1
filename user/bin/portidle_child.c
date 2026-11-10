#include <os1.h>
#include <string.h>

#define PORT_IDLE_NAME "OS1nx_capidle"
#define PORT_IDLE_MAGIC UINT64_C(0x504F525449444C45)
#define PORT_IDLE_DELAY_MS 100

int main(void) {
  struct ipc_message message;
  OS1_sleep(PORT_IDLE_DELAY_MS);
  int port = OS1_port_open(PORT_IDLE_NAME);
  if (port < 0)
    return 1;
  memset(&message, 0, sizeof(message));
  message.type = 0x431;
  message.data1 = PORT_IDLE_MAGIC;
  long sent = OS1_port_send(port, &message);
  if (OS1low_handle_close(port) != 0)
    return 2;
  return sent == (long)sizeof(message) ? 0 : 3;
}
