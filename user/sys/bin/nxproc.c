/*
 * user/sys/bin/nxproc.c
 * NEXS process-list ELF (ASTRA stratified service model).
 *
 * Usage (run from the shell, like /bin/stress):
 *   nxproc                 snapshot the process table once and print it to
 *                          stdout (the caller's TTY) — the canonical `ps`
 *                          inline runtime.  No compositor window is created;
 *                          every row goes through printf, the same fd 1 path
 *                          the shell uses for its own prompt.
 *   nxproc kill <pid>      terminate a process by pid (passes through the
 *                          helper's thin kill wrapper).
 *
 * This is the INLINE counterpart to nxtop.c: nxtop opens a window and
 * refreshes at ~1Hz, this ELF is one-shot and exits.  Both share the snapshot
 * + signature + render logic via the header-only nxproc.h helper, so the
 * displayed format and colours are identical regardless of which entry point
 * the user picks.
 */
#include "nxproc.h"
#include <os1.h>
#include <string.h>

/*
 * cmd_print - `nxproc` (no args) handler: one-shot snapshot + inline render.
 *
 * Fills a local array via nxproc_snapshot() and feeds it to
 * nxproc_render_inline(), which writes the formatted, ANSI-coloured table to
 * the caller's stdout.  No window is created; the shell's own terminal
 * (which routed fd 1 here via the kernel's spawn-time fd inheritance)
 * receives the bytes.  Returns 0 on success, 1 on syscall error.
 */
static int cmd_print(void) {
  struct ps_info procs[NXPROC_MAX];
  int count = nxproc_snapshot(procs, NXPROC_MAX);
  if (count < 0) {
    printf("nxproc: failed to fetch process list (err %d)\n", count);
    return 1;
  }
  nxproc_render_inline(procs, count);
  return 0;
}

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

int main(int argc, char **argv) {
  /* nxproc kill <pid> */
  if (argc >= 3 && strncmp(argv[1], "kill", 5) == 0)
    return cmd_kill(argv[2]);

  /* nxproc (no args): one-shot inline render on stdout. */
  if (argc < 2)
    return cmd_print();

  printf("nxproc: bad arguments\n");
  printf("usage: nxproc | nxproc kill <pid>\n");
  return 1;
}