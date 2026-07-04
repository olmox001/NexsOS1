#ifndef _USER_NXEXEC_H
#define _USER_NXEXEC_H

/*
 * user/sys/bin/nxexec.h
 * NEXS stratified process-LAUNCH helper (ASTRA service layer) — issue #193.
 *
 * THE single place that encodes HOW a process is launched, separated out of
 * nxshell/nxlauncher (which used to each hand-roll a divergent copy).  Two
 * launch modes, matching the maintainer's standalone-vs-needs-shell model:
 *
 *   FOREGROUND (needs-shell, terminal-attached) — nxexec_run_foreground():
 *     the spawner IS the child's controlling terminal (default ctty
 *     inheritance).  Runtime window detection decides the outcome: the child
 *     opens its OWN window -> it detaches (graphical app, lives
 *     independently, managed via nxwins/dock); it stays windowless -> it runs
 *     as a shell job (stdout in the spawner's window, Ctrl-C kills it, the
 *     spawner blocks until it exits).
 *
 *   DETACHED (standalone, launcher-style) — nxexec_spawn_detached():
 *     the child does NOT inherit the spawner as ctty (SPAWN_FLAG_DETACHED,
 *     kernel-enforced).  A windowed child behaves identically (own-window
 *     stdout).  A windowless child fails CLOSED (no output surface) instead
 *     of silently writing into the launcher's soon-minimized window as if the
 *     launcher were a shell — the #193 bug.
 *
 * Lifecycle note: windowless children die with their parent (the kill model
 * is already window-aware, process_kill_subtree spares windowed subtrees —
 * docs/PROCESS-KILL-MODEL.md), which completes the maintainer's policy:
 * "crea una finestra? gestione window; altrimenti figlio, chiuso col padre".
 *
 * SECURITY MODEL: no ambient checks of its own (the nxproc.h pattern) —
 * spawn is CAP_SPAWN-gated kernel-side per caller; the helper neither widens
 * nor narrows that.
 *
 * Consumers: nxshell.c (foreground path), nxlauncher.c (detached path).
 */

#include <os1.h>

#define NXEXEC_PATH_MAX 96

/* A child is treated as "windowed" (a GUI app that should detach) only if it
 * owns the SAME window id across this many consecutive polls.  A transient
 * create/destroy churn — stress's --gui lane opens back-to-back windows with
 * DIFFERENT ids (docs/PROCESS-KILL-MODEL.md §5) — never accumulates a stable
 * count, so it is correctly hosted as a terminal program instead of being
 * mistaken for a persistent GUI app and wrongly detached.  ~6 polls x ~15ms
 * = ~90ms of an unchanging window; a real app holds its window far longer,
 * a churner never holds one id that long. */
#define NXEXEC_STABLE_POLLS 6

/* nxexec_run_foreground return codes. */
#define NXEXEC_JOB_EXITED   0 /* the child finished (or was Ctrl-C'd) */
#define NXEXEC_JOB_DETACHED 1 /* the child owns a stable window -> it is a GUI app */

/*
 * nxexec_run_foreground - watch a freshly-spawned child as a foreground job
 * (USR-TTY-01 #123), with a DEBOUNCED window-ownership test so a transient
 * window (stress --gui) is not mistaken for "this is a GUI app, detach".
 *
 * Returns NXEXEC_JOB_DETACHED if the child settled on its own persistent
 * window (the caller should stop hosting it), or NXEXEC_JOB_EXITED if the
 * child finished or was killed by Ctrl-C (ETX via keyboard IPC).
 */
static inline int nxexec_run_foreground(int pid) {
  if (pid <= 0)
    return NXEXEC_JOB_EXITED;
  int last_win = -1;
  int stable = 0;
  while (1) {
    int w = window_of_pid(pid);
    if (w > 0 && w == last_win) {
      if (++stable >= NXEXEC_STABLE_POLLS)
        return NXEXEC_JOB_DETACHED; /* a persistent own window -> GUI app */
    } else {
      stable = (w > 0) ? 1 : 0; /* new/transient window id: restart the count */
    }
    last_win = w;

    if (wait(pid) != -1)
      return NXEXEC_JOB_EXITED; /* child finished (dead/zombie/gone) */

    struct ipc_message m;
    if (try_recv(-1, &m) == 0 && m.type == IPC_TYPE_INPUT && m.data2 != 0 &&
        m.payload[0] == 0x03) {
      kill_process(pid);
      print("^C\n");
      return NXEXEC_JOB_EXITED;
    }
    OS1_sleep(15); /* time base for the debounce; keeps Ctrl-C responsive */
  }
}

/*
 * nxexec_spawn_search - resolve a bare program name against /bin then
 * /sys/bin (absolute paths bypass the search) and spawn it with argv.
 * out_path (NXEXEC_PATH_MAX bytes) receives the resolved path for the
 * caller's diagnostics.  Returns the PID or <= 0 on failure.
 * detached != 0 selects the DETACHED launch mode (see header comment).
 */
static inline int nxexec_spawn_search(int argc, char *argv[], char *out_path,
                                      int detached) {
  const char *name = argv[0];
  long (*do_spawn)(const char *, int, char *const[]) =
      detached ? OS1low_process_spawn_detached : OS1low_process_spawn;

  if (name[0] == '/') {
    snprintf(out_path, NXEXEC_PATH_MAX, "%s", name);
    return (int)do_spawn(out_path, argc, argv);
  }
  snprintf(out_path, NXEXEC_PATH_MAX, "/bin/%s", name);
  int pid = (int)do_spawn(out_path, argc, argv);
  if (pid > 0)
    return pid;
  snprintf(out_path, NXEXEC_PATH_MAX, "/sys/bin/%s", name);
  return (int)do_spawn(out_path, argc, argv);
}

/* nxexec_spawn_detached - launcher-style spawn of an explicit path: the child
 * never inherits the spawner as ctty (kernel-enforced, SPAWN_FLAG_DETACHED). */
static inline int nxexec_spawn_detached(const char *path, int argc,
                                        char *const argv[]) {
  return (int)OS1low_process_spawn_detached(path, argc, argv);
}

/*
 * nxexec_spawn_hosted - run PATH under the execution service /sys/bin/nxexec
 * (the launcher path for anything that might be a terminal program).  nxexec
 * is spawned DETACHED (the launcher never becomes anyone's ctty); nxexec then
 * opens a terminal window, spawns PATH as its own child (becoming its ctty),
 * and decides at runtime:
 *   - PATH opens its own window   -> nxexec closes, PATH runs independently;
 *   - PATH is windowless          -> nxexec's terminal shows PATH's output,
 *     stays readable, and closes on the user's Enter (or a timeout);
 * closing nxexec's window kills the child (kernel subtree kill).  The job is
 * spawned exactly once, by nxexec — no double execution.
 */
static inline int nxexec_spawn_hosted(const char *path) {
  char *argv[2];
  argv[0] = (char *)"/sys/bin/nxexec";
  argv[1] = (char *)path;
  return (int)OS1low_process_spawn_detached("/sys/bin/nxexec", 2, argv);
}

#endif /* _USER_NXEXEC_H */
