/*
 * user/bin/capipc_child.c
 * Child half of the capability-IPC delegation test (see capipc.c).
 *
 * Spawned as PLVL_GUEST.  The parent grants it exactly one capability — a
 * WRITE handle to the parent's process object — which lands at handle slot 0
 * (the child's handle table starts empty).  The child sends a message back
 * THROUGH that delegated capability with OS1_object_write: it never names the
 * parent by PID, and its authority to send is the granted handle, not ambient
 * permission.  Retries until the grant has been installed.
 */
#include <os1.h>
#include <string.h>

#define CAPIPC_MAGIC 0xCAFE

int main(void) {
  printf("[capipc] child PID %d sending via delegated cap...\n", get_pid());

  struct ipc_message msg;
  memset(&msg, 0, sizeof(msg));
  msg.type = 1;
  msg.data1 = CAPIPC_MAGIC;

  int sent = 0;
  for (int i = 0; i < 4000 && !sent; i++) {
    long r = OS1_object_write(0, &msg, sizeof(msg)); /* slot 0 = granted cap */
    if (r == 0)
      sent = 1;
    else
      yield(); /* grant not installed yet (-EBADF) — retry */
  }
  printf("[capipc] child send %s\n", sent ? "OK" : "FAILED");

  for (int i = 0; i < 60; i++)
    yield();
  return 0;
}
