/*
 * user/sys/bin/nxexec.c
 * NEXS execution command (SRL, ASTRA §6.4) — issue #193.
 * This program owns hosted terminal/UI execution only; the dedicated
 * /sys/services/nxexecd process serves execution RPCs.
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
#include <execsvc.h>
#include <os1.h>
#include <string.h>
#include <unistd.h>

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
  /* A system command cannot silently turn itself into a privileged daemon.
   * Only the separately packaged supervised nxexecd owns the service port. */
  if (argc >= 2 && strcmp(argv[1], "--service") == 0)
    return 1;

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

  /* Phase 3: the canonical display name is the exe basename ("/bin/lua" ->
   * "lua"); title the hosted window with it and publish it in the registry
   * (below) so the bar serialises this launch by pid. */
  const char *dispname = nxexec_basename(jargv[0]);

  int win = create_window(140, 120, 560, 360, dispname);
  if (win < 0)
    return 1;

  /* Hidden during the grace probe: a graphical child never flashes an empty
   * terminal. Flag bits (compositor_set_window_flags): 4 = hide, 2 = show. */
  set_window_flags(win, 4);

  /*
   * Apply `<` `>` `>>` `2>` on the HOSTED path too (Phase 9 step 2).
   *
   * Until now redirection existed only in the shell, so the two launch paths
   * were not merely separate implementations — they had different CAPABILITIES:
   * a terminal user could redirect, a graphical caller could not.  nxlauncher
   * and nxfilem already route through this binary, so parsing here is what
   * finally lets a GUI caller pass a full command line (the nxpopup argument
   * entry) and get the same behaviour the terminal has always had.
   *
   * Errors are reported into this window, which must therefore be REVEALED
   * first: it was hidden for the grace probe, and a failure message written to
   * a hidden window would be invisible — the launch would look like it silently
   * did nothing.
   */
  struct spawn_redir redir[SPAWN_MAX_REDIR];
  int rfds[SPAWN_MAX_REDIR], nredir = 0, nrfds = 0;
  if (nxexec_strip_redirections(&jargc, jargv, redir, &nredir, rfds, &nrfds) !=
          0 ||
      jargc == 0) {
    set_window_flags(win, 2);
    set_focus(get_pid());
    if (jargc == 0)
      printf("nxexec: no command to run\r\n");
    wait_dismiss(NXEXEC_DISMISS_MS);
    for (int i = 0; i < nrfds; i++)
      close(rfds[i]);
    return 1;
  }

  char path[NXEXEC_PATH_MAX];
  int pid = nxexec_spawn_search_redir(jargc, jargv, path, /*detached=*/0, redir,
                                      nredir);
  /* The child holds its own dups; drop ours so the files close when both sides
   * are done, instead of staying open for this terminal's whole lifetime. */
  for (int i = 0; i < nrfds; i++)
    close(rfds[i]);

  if (pid <= 0) {
    set_window_flags(win, 2);
    set_focus(get_pid());
    printf("nxexec: unable to execute '%s'\r\n", jargv[0]);
    wait_dismiss(NXEXEC_DISMISS_MS);
    return 1;
  }

  /* Publish the launch identity now that we have a live pid.  A GUI app that
   * detaches (returns below) keeps it — it is still running and the bar shows
   * it; a stale key for a later-dead pid is harmless (that pid has no window). */
  nxexec_register_identity(pid, dispname);

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
     * app), not for stress's transient churn.
     *
     * Ctrl-Z suspends the job (detected, not swallowed): a standalone terminal
     * has no shell to adopt a stopped job, so instead of orphaning it or
     * self-closing, keep this window open and offer resume (Enter) / kill
     * (Ctrl-C).  Only Ctrl-C or the program exiting closes a hosted terminal. */
    int r;
    while ((r = nxexec_run_foreground(pid)) == NXEXEC_JOB_STOPPED) {
      printf("\r\n\033[90m[suspended — Enter to resume, Ctrl-C to close]"
             "\033[0m\r\n");
      if (!nxexec_wait_stopped_action(pid)) {
        job_done = 1; /* Ctrl-C: child killed -> fall through and close */
        break;
      }
      OS1_process_cont(pid); /* Enter: resume and keep watching */
    }
    if (r == NXEXEC_JOB_DETACHED)
      return 0; /* it settled on its own window -> vanish */
  }

  /* The hosted job has terminated (windowless job exited/Ctrl-C'd) — drop its
   * registry identity; a detached GUI app took the early return above and kept
   * its identity. */
  nxexec_unregister_identity(pid);

  /* Job finished: leave the output on screen, let the user read it, close on
   * Enter or after the timeout. No interactive prompt, no accumulation. */
  printf("\r\n\033[90m[process terminated — press Enter to close]\033[0m");
  wait_dismiss(NXEXEC_DISMISS_MS);
  return 0;
}