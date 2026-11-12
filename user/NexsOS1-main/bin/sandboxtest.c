/*
 * user/bin/sandboxtest.c
 * Privilege-level / capability test driver (USR-SEC-03 #79).
 *
 * Runs as a normal PLVL_USER process (spawned by the shell, full caps) and
 * spawns /bin/sandboxchild at PLVL_GUEST.  The child reports, on the serial
 * console tagged "[sandbox]", that spawn / file-write / non-relative IPC are
 * all denied while window creation is allowed.
 *
 * This single child proves two things at once:
 *   - the per-surface capability gates work (the denials), and
 *   - the level ceiling clamp works: spawn_level() requests CAP_ALL, but the
 *     kernel clamps it to level_ceiling[GUEST] = CAP_WINDOW, so the guest can
 *     ONLY draw.  (The "never more privileged than the creator" clamp shares
 *     the same expression in process_create_caps; observing it from userland
 *     would need a caps-readback syscall, out of scope for B3.)
 *
 * The headless harness greps "[sandbox]".
 */
#include <os1.h>

int main(void) {
  int win = create_window(140, 140, 360, 160, "Sandbox Test");
  printf("[sandbox] driver PID %d (user)\n", get_pid());

  /* Spawning is allowed for us (we hold CAP_SPAWN); the CHILD is stripped of
   * everything but CAP_WINDOW by the guest ceiling. */
  long pid = spawn_level("/bin/sandboxchild", PLVL_GUEST);
  if (pid > 0) {
    printf("[sandbox] spawned guest child PID %ld -> PASS\n", pid);
    if (win >= 0)
      printf_win(win, "spawned guest PID %ld\n", pid);
  } else {
    printf("[sandbox] spawn_level FAILED rc=%ld\n", pid);
    if (win >= 0)
      printf_win(win, "spawn_level FAILED rc=%ld\n", pid);
  }

  printf("[sandbox] driver done\n");

  while (1)
    yield();
  return 0;
}
