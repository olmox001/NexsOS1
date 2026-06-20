/*
 * user/bin/capipc.c
 * Capability-mediated IPC + delegation test (ASTRA §6.2/6.5 — Mach ports as
 * objects, seL4 delegation).  Parent half; child = /bin/capipc_child.
 *
 * Proves that authority to send IPC can be DELEGATED as an unforgeable handle:
 *   1. mint a send capability to ourselves: a PROCESS handle with WRITE
 *      (= a send right) and TRANSFER (so it can be delegated);
 *   2. spawn a GUEST child (no CAP_IPC_ANY of its own);
 *   3. grant (attenuated to WRITE) the handle into the child's table;
 *   4. the child sends a message back THROUGH the delegated capability
 *      (OS1_object_write on the handle — not by PID), which we receive.
 * Results tagged "[capipc]" on the serial console.
 */
#include <os1.h>
#include <string.h>

#define CAPIPC_MAGIC 0xCAFE

int main(void) {
  int win = create_window(160, 160, 440, 200, "Cap IPC");
  int self = get_pid();
  char pidbuf[16];
  sprintf(pidbuf, "%d", self);
  printf("[capipc] parent PID %d\n", self);

  /* 1. mint a send capability to ourselves (WRITE=send right, TRANSFER=delegable) */
  int h = (int)OS1low_handle_create(OS1_NS_PROC, pidbuf,
                                    OS1_RIGHT_WRITE | OS1_RIGHT_TRANSFER,
                                    OBJ_TYPE_PROCESS);
  int t1 = h >= 0;
  printf("[capipc] mint-self-send-cap: %s (h=%d)\n", t1 ? "PASS" : "FAIL", h);

  /* 2. spawn a guest child */
  long child = spawn_level("/bin/capipc_child", PLVL_GUEST);
  int t2 = child > 0;
  printf("[capipc] spawn-guest-child: %s (pid=%ld)\n", t2 ? "PASS" : "FAIL", child);

  /* 3. delegate the capability to the child, attenuated to WRITE only */
  long gslot = (t1 && t2) ? OS1low_cap_grant((int)child, h, OS1_RIGHT_WRITE) : -1;
  int t3 = gslot >= 0;
  printf("[capipc] grant-send-cap-to-child: %s (child slot=%ld)\n",
         t3 ? "PASS" : "FAIL", gslot);

  /* 4. receive the child's message, sent through the delegated capability */
  struct ipc_message msg;
  memset(&msg, 0, sizeof(msg));
  int got = 0;
  for (int i = 0; i < 4000 && !got; i++) {
    if (try_recv((int)child, &msg) == 0)
      got = 1;
    else
      yield();
  }
  int t4 = got && msg.from == (int)child &&
           (unsigned int)msg.data1 == CAPIPC_MAGIC;
  printf("[capipc] recv-via-delegated-cap: %s (from=%d data1=0x%x)\n",
         t4 ? "PASS" : "FAIL", msg.from, (unsigned int)msg.data1);

  int fails = (!t1) + (!t2) + (!t3) + (!t4);
  printf("[capipc] done: %d failure(s)\n", fails);
  if (win >= 0)
    printf_win(win, "capipc done: %d failure(s)\n", fails);

  if (h >= 0)
    OS1low_handle_close(h);
  for (int i = 0; i < 150; i++)
    yield();
  return fails ? 1 : 0;
}
