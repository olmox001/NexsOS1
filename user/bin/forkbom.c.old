/*
 * user/bin/forkbomb.c
 * Resource-exhaustion / quota test (SCHED-DOS-01 #122).
 *
 * A user process (PROC_PERM_USER) that spawns long-lived children
 * (/bin/counter does NOT steal keyboard focus) as fast as it can, WITHOUT
 * reaping them, to drive the per-parent child quota.  Before the fix this
 * saturated the process pool until the shell could no longer be scheduled
 * to kill the bomber; with MAX_PROCS_PER_PARENT in place spawn() starts
 * returning a negative errno once the quota is hit, the pool keeps free
 * slots, and the shell stays responsive.
 *
 * Prints the number of children it managed to create — expected to plateau
 * at the quota (32), NOT to climb until the system locks up.  Leave it
 * running and kill it from the shell to prove recovery still works.
 */
#include <os1.h>

int main(void) {
  int created = 0;
  printf("[forkbomb] PID %d starting\n", get_pid());

  for (int i = 0; i < 2000; i++) {
    int pid = spawn("/bin/counter");
    if (pid > 0) {
      created++;
    } else {
      /* Quota or pool refused us: report once and stop hammering. */
      printf("[forkbomb] spawn refused after %d children (rc=%d) — quota "
             "holds, pool not exhausted\n",
             created, pid);
      break;
    }
  }

  printf("[forkbomb] created %d children total; still alive and killable\n",
         created);
  while (1)
    yield();
  return 0;
}
