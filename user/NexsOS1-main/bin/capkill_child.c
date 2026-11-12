/*
 * user/bin/capkill_child.c
 * Kill target for the capability process-control test (see capkill.c).
 * Spawned as PLVL_GUEST; loops forever until the parent terminates it through
 * a PROCESS capability holding OS1_RIGHT_DESTROY.
 */
#include <os1.h>

int main(void) {
  printf("[capkill] child PID %d alive, looping\n", get_pid());
  while (1)
    yield();
  return 0;
}
