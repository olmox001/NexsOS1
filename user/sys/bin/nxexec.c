/*
 * user/sys/bin/nxexec.c
 * NEXS execution service (SRL, ASTRA §6.4) — issue #193.
 *
 * nxexec is THE launcher-side executor: the one process that decides HOW a
 * program is run and gives a terminal program the terminal it needs. The
 * dock/launcher spawns `nxexec <program> [args]` DETACHED (the launcher is
 * never anyone's controlling terminal); nxexec then:
 *
 * 1. opens ONE terminal window (hidden during the grace probe below);
 * 2. spawns <program> EXACTLY ONCE as its own child, so nxexec is the
 *    child's controlling terminal (its stdout lands in nxexec's window);
 * 3. detects the child's KIND at runtime — using the existing window-ownership
 *    signal (SYS_WINDOW_OF_PID), no new mechanism:
 *    - the child opens its OWN window -> it is a graphical app; nxexec
 *      closes (its hidden window is never shown) and the app runs alone,
 *      managed like any other windowed app (nxwins/dock);
 *    - the child is windowless -> it is a terminal program;
 *      nxexec SHOWS its terminal (the output written while hidden is
 *      already in the window buffer) so the output is visible;
 * 4. after a windowless child EXITS, keeps its output on screen and waits
 *    for the user to press Enter (or a 10 s timeout), THEN closes — so the
 *    output is readable but nothing accumulates;
 * 5. while a windowless child is still RUNNING, stays open watching it
 *    (Ctrl-C kills it); if the user closes nxexec's window the kernel's
 *    window-aware subtree kill takes the windowless child with it
 *    (docs/PROCESS-KILL-MODEL.md — "child closed with parent").
 *
 * Capability posture (the child is the most-exercised spawn path, so it is
 * the one to keep tight): nxexec adds NO ambient authority of its own. The
 * child goes through the same CAP_SPAWN-gated, per-path-preset, monotonic
 * creator-clamped kernel spawn as everything else (secure-by-caller): a
 * child under /bin lands at PLVL_USER even though nxexec runs from /sys/bin.
 * The spawn ABI rejects unknown flag bits (kernel, SPAWN_FLAGS_ALL).
 */
#include "nxexec.h"
#include <os1.h>

#define NXEXEC_GRACE_MS 500     /* window-vs-windowless decision window */
#define NXEXEC_DISMISS_MS 10000 /* auto-close after a finished job */

/*
 * wait_dismiss - block until the USER presses Enter or timeout_ms elapses.
 *
 * Called only AFTER the child has exited, so any keyboard input now is
 * unambiguously the user's (not a newline consumed by the running program) —
 * this fulfills the "distinguish internal newlines from user newlines"
 * requirement: we simply do not read the keyboard while the child runs.
 */
static void wait_dismiss(int timeout_ms) {
  int elapsed = 0;
  while (elapsed < timeout_ms) {
    struct ipc_message m;
    if (try_recv(-1, &m) == 0 && m.type == IPC_TYPE_INPUT && m.data2 != 0) {
      char c = m.payload[0];
      if (c == '\r' || c == '\n')
        return;
    }
    OS1_sleep(50);
    elapsed += 50;
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2)
    return 1; /* nothing to run */

  int jargc = argc - 1; /* drop our own argv[0] */
  char **jargv = &argv[1];

  /*
   * Normalize executable name before using it for window titles,
   * diagnostics, and spawn resolution.
   *
   * This prevents shell-style quoted paths such as:
   *   "\"/bin/lua\""
   *
   * from appearing as literal filenames or UI strings.
   */
  char clean_exec[NXEXEC_PATH_MAX];

  jargv[0] = (char *)nxexec_strip_path_quotes(jargv[0], clean_exec,
                                              sizeof(clean_exec));

  int win = create_window(140, 120, 560, 360, jargv[0]);
  if (win < 0)
    return 1;

  /* Hidden during the grace probe: a graphical child never flashes an empty
   * terminal. Flag bits (compositor_set_window_flags): 4 = hide, 2 = show. */
  set_window_flags(win, 4);

  char path[NXEXEC_PATH_MAX];
  int pid = nxexec_spawn_search(jargc, jargv, path, /*detached=*/0);

  if (pid <= 0) {
    set_window_flags(win, 2);
    set_focus(get_pid());
    printf("nxexec: unable to execute '%s'\r\n", jargv[0]);
    wait_dismiss(NXEXEC_DISMISS_MS);
    return 1;
  }

  /* Grace probe, DEBOUNCED (docs/PROCESS-KILL-MODEL.md §5): a GUI app settles
   * on a PERSISTENT window (same id for NXEXEC_STABLE_POLLS polls) -> vanish;
   * a fast CLI exits -> host + dismiss; a terminal program that only churns
   * transient windows (stress --gui) never accumulates a stable count and
   * falls through to being hosted, so its output stays visible in the
   * terminal instead of nxexec wrongly detaching and dropping it. */
  int job_done = 0, last_win = -1, stable = 0;
  for (int i = 0; (i * 15) < NXEXEC_GRACE_MS; i++) {
    if (nxexec_window_stable(pid, &last_win, &stable))
      return 0; /* persistent own window -> GUI app, vanish */

    if (wait(pid) != -1) {
      job_done = 1; /* fast CLI finished within the probe */
      break;
    }
    OS1_sleep(15);
  }

  /* Windowless: reveal the terminal (its output is already in the buffer). */
  set_window_flags(win, 2);
  set_focus(get_pid());

  if (!job_done) {
    /* Long-running windowless job: watch it (Ctrl-C kills; if the user closes
     * this window the kernel subtree-kill takes the child). The debounced
     * watch returns DETACHED only for a stable own window (a slow-to-open GUI
     * app), not for stress's transient churn. */
    if (nxexec_run_foreground(pid) == NXEXEC_JOB_DETACHED)
      return 0; /* it settled on its own window -> vanish */
  }

  /* Job finished: leave the output on screen, let the user read it, close on
   * Enter or after the timeout. No interactive prompt, no accumulation. */
  printf("\r\n\033[90m[process terminated — press Enter to close]\033[0m");
  wait_dismiss(NXEXEC_DISMISS_MS);
  return 0;
}