/*
 * user/sys/bin/nxproc.c
 * NEXS process-management frontend (ASTRA stratified service model).
 *
 * Usage:
 *   nxproc                 open a window and show the live process list,
 *                          refreshing only when the table actually changes
 *   nxproc kill <pid>      terminate a process by pid
 *
 * This is the THIN CLI/windowed frontend over the reusable helper nxproc.h.
 * It contains no policy of its own: the windowed list defers all snapshot +
 * render + change-detection to the helper, and `kill` is a direct pass-through
 * to nxproc_kill().  Because the helper only wraps syscalls the kernel already
 * gates per caller, nxproc's reach is exactly the caller's reach — no more.
 */
#include "nxproc.h"
#include <os1.h>

/*
 * cmd_kill - `nxproc kill <pid>` handler.
 *
 * Parses the pid argument, calls the helper's kill wrapper, and reports
 * success/failure.  Returns a process exit code (0 ok, 1 error).  The kernel
 * decides whether this caller may actually kill the target; we only relay the
 * outcome.
 */
static int cmd_kill(const char *pid_arg) {
  int pid = atoi(pid_arg);
  if (pid <= 0) {
    printf("nxproc: invalid pid '%s'\n", pid_arg);
    return 1;
  }
  int r = nxproc_kill(pid);
  if (r == 0) {
    printf("nxproc: killed pid %d\n", pid);
    return 0;
  }
  printf("nxproc: failed to kill pid %d (err %d)\n", pid, r);
  return 1;
}

/*
 * cmd_window - `nxproc` (no args) handler: live windowed process list.
 *
 * Opens a window and refreshes the list at ~1Hz, but delegates to
 * nxproc_render_if_changed() so the window is only rewritten when the visible
 * table changes.  Between checks it blocks on the real kernel timer
 * (OS1_sleep) rather than busy-spinning, so an idle list costs no CPU.
 */
static int cmd_window(void) {
  int win = _sys_create_window(100, 100, 520, 600, "NEXS Processes");
  if (win < 0)
    return 1;

  unsigned long sig = 0; /* Seed sentinel: forces the first render. */
  while (1) {
    nxproc_render_if_changed(win, &sig);
    OS1_sleep(1000); /* ~1Hz cadence; redraw inside only fires on change. */
  }

  return 0;
}

int main(int argc, char **argv) {
  /* nxproc kill <pid> */
  if (argc >= 3 && strncmp(argv[1], "kill", 5) == 0)
    return cmd_kill(argv[2]);

  /* nxproc (no args): live windowed list. */
  if (argc < 2)
    return cmd_window();

  printf("nxproc: bad arguments\n");
  printf("usage: nxproc | nxproc kill <pid>\n");
  return 1;
}
