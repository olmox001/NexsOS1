/*
 * user/bin/capipc_child.c
 * Child half of the capability-IPC delegation test (see capipc.c).
 *
 * Spawned as PLVL_GUEST.  The parent grants it exactly one capability — a
 * WRITE handle to the parent's process object. The CHILD-local handle slot
 * is delivered through a kernel-stamped mailbox after a real cap_grant; the
 * child verifies type and attenuated rights before using the reference.
 * No assumed slot index and no ambient authority are involved.
 */
#include <os1.h>
#include <string.h>

#include "capipc_protocol.h"

int main(void) {
  printf("[capipc] child PID %d awaiting delegated cap...\n", get_pid());
  int handle = -1;
  for (unsigned i = 0U; i < CAPIPC_WAIT_STEPS; ++i) {
    struct ipc_message ready = {0};
    if (OS1low_ipc_try_recv(-1, &ready) == 0 &&
        ready.type == CAPIPC_GRANT_READY &&
        ready.data2 == CAPIPC_MAGIC &&
        ready.from > 0 && ready.data1 < 32U) {
      int candidate = (int)ready.data1;
      long found = OS1low_cap_query(candidate);
      if (found >= 0 && OS1_CAPQ_TYPE(found) == OBJ_TYPE_PROCESS &&
          OS1_CAPQ_RIGHTS(found) == OS1_RIGHT_WRITE) {
        handle = candidate;
        break;
      }
    }
    OS1_sleep(1);
  }
  struct ipc_message result = {0};
  result.type = 1;
  result.data1 = CAPIPC_MAGIC;
  int sent = handle >= 0 &&
             OS1_object_write(handle, &result, sizeof(result)) == 0;
  printf("[capipc] child delegated send %s (handle=%d)\n",
         sent ? "OK" : "FAILED", handle);
  if (handle >= 0 && OS1low_handle_close(handle) < 0)
    sent = 0;
  return sent ? 0 : 1;
}
