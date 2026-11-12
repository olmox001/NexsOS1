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

/* execsvc.h is the client-facing WIRE CONTRACT (structs + version only); this
 * header is the executor's POLICY IMPLEMENTATION.  They stay separate so a
 * client that merely asks for an execution links the protocol alone, instead of
 * dragging in window probing, identity registration and the foreground watch
 * loop below (Phase 12: "i servizi vanno divisi dalla libc e integrati in
 * maniera modulare").  Included here so nxexec.c and the existing consumers get
 * it transitively — one definition, no duplication. */
#include <execsvc.h>
#include <os1.h>
#include <stdlib.h> /* getenv() — see the dynamic HOME resolution in
                      * nxexec_resolve_path()'s tier 2 below. */
#include <string.h>
#include <unistd.h> /* close() */

#define NXEXEC_PATH_MAX 96
#define NXEXEC_ARGV_MAX                                                        \
  16 /* matches OS1low_process_spawn_detached's argv cap                       \
      * (kernel-enforced); nxexec itself takes argc-1                          \
      * of these and forwards them to the child. */

/* A child is treated as "windowed" (a GUI app that should detach) only if it
 * owns the SAME window id across this many consecutive polls.  A transient
 * create/destroy churn — stress's --gui lane opens back-to-back windows with
 * DIFFERENT ids (docs/PROCESS-KILL-MODEL.md §5) — never accumulates a stable
 * count, so it is correctly hosted as a terminal program instead of being
 * mistaken for a persistent GUI app and wrongly detached.  ~6 polls x ~15ms
 * = ~90ms of an unchanging window; a real app holds its window far longer,
 * a churner never holds one id that long. */
#define NXEXEC_STABLE_POLLS EXECSVC_STABLE_POLLS

/*
 * nxexec_window_stable - one step of the "has this pid settled on its own
 * persistent window?" debounce, factored out because the exact same state
 * machine used to be hand-copied in three places (nxexec_run_foreground()
 * below, nxexec.c main()'s grace probe, and nxjobs.h's nxjobs_poll() for
 * background jobs) — three chances for the threshold or the reset rule to
 * quietly drift apart between callers that all need to agree on what
 * "windowed" means.
 *
 * Caller owns the debounce state (*last_win, *stable) across calls — one
 * call per poll tick, whatever that caller's tick source is (a blocking
 * OS1_sleep loop, a probe loop, or an outer shell tick). Returns 1 once
 * window_of_pid(pid) has reported the SAME positive window id for
 * NXEXEC_STABLE_POLLS consecutive calls (a persistent own window -> GUI
 * app), 0 otherwise. A transient/changing window id (create/destroy churn,
 * see the NXEXEC_STABLE_POLLS comment above) resets the run instead of
 * accumulating, so it never falsely reaches the threshold.
 */
static inline int nxexec_window_stable(int pid, int *last_win, int *stable) {
  return execsvc_window_stable(pid, last_win, stable);
}

/* nxexec_run_foreground return codes. */
#define NXEXEC_JOB_EXITED 0 /* the child finished (or was Ctrl-C'd) */
#define NXEXEC_JOB_DETACHED                                                    \
  1 /* the child owns a stable window -> it is a GUI app */
#define NXEXEC_JOB_STOPPED 2 /* Ctrl-Z: the child was suspended (PROC_STOPPED);
                              * the caller should register it as a stopped job */

/*
 * NXEXEC path-resolution tiers, aligned with POSIX/execvp (previously
 * duplicated ad hoc across system(), nxassoc, and nxlauncher, each with
 * its own resolution logic):
 *
 *   1. ABSOLUTE ROOT PATH   "/foo/bar" -> used exactly as provided.
 *   2. HOME-RELATIVE PATH   "~" / "~/x" -> "~" is expanded to "/home"
 *                                         (single-user system, no getpwuid()).
 *   3. PROCESS-VFS-RELATIVE PATH   any other path containing a '/'
 *      (e.g. "./x", "sub/dir/x") -> resolved against the CALLING PROCESS'
 *      current working directory (getcwd()) — the cwd is maintained
 *      per process, never as a global prefix.
 *   4. BARE NAME (no '/') -> searched, in order, in /bin and then /sys/bin
 *      (existing behavior, unchanged).
 *
 * This exactly matches the execvp() rule: a name containing a slash is
 * treated as a path (resolved, never searched); a bare name is searched.
 */
#define NXEXEC_HOME_DIR "/home"

/*
 * Remove surrounding quotes from an executable path.
 *
 * Shell-like callers may provide quoted executable paths
 * (for example "\"/bin/lua\"" or "'/bin/lua'").
 * Path resolution must operate on the real path and ignore
 * only matching outer quotes.
 *
 * Internal quotes are preserved.
 */
static inline const char *nxexec_strip_path_quotes(const char *name, char *out,
                                                   size_t size) {
  return execsvc_strip_path_quotes(name, out, size);
}

/*
 * Phase 3 — process identity (PLAN-2026-07-17): nxexec is the launch authority,
 * so it assigns the ONE canonical display name for a program (the exe's
 * basename, quotes already stripped: "/bin/lua" -> "lua") and publishes it two
 * ways, per the maintainer's "window-title convention + registry namespace"
 * decision:
 *   - it titles the window it hosts with that name (the bar reads titles);
 *   - it no longer registers any identity key: sys.proc.<pid>.* is computed by
 *     the kernel (Phase 5b) and the icon follows from the program name (17a)
 *     can serialise ANY pid's identity by number, independent of an app's own
 *     (possibly colliding or changing) window title.
 * The icon key is the same canonical name; nxicon classifies it.
 */
static inline const char *nxexec_basename(const char *path) {
  return execsvc_basename(path);
}

static inline void nxexec_register_identity(int pid, const char *name) {
  /* NOTHING to publish (Phase 5b, corrected in 17a).
   *
   * `sys.proc.<pid>.name` is VIRTUAL — the kernel computes it from the process
   * table on every read — so writing it here would recreate exactly the second,
   * best-effort copy whose staleness needed a garbage collector.
   *
   * The ICON does not need a key either.  5b published `sys.appicon.<name>`
   * mapping a name to ITSELF: zero information.  Worse, it keyed on the
   * BASENAME while the only reader (nxui) builds the key from the kernel's FULL
   * path, so it could never even hit.  `nxicon_classify` already strips the
   * path and classifies the program name directly, which is the whole job. */
  (void)pid;
  (void)name;
}

static inline void nxexec_unregister_identity(int pid) {
  /* Nothing to unregister: the per-process view is virtual, so a dead pid has
   * no keys by construction.  Kept as a no-op so callers read naturally. */
  (void)pid;
}

/* nxexec_prune_identities - REMOVED in Phase 5b.  It existed to sweep
 * `sys.proc.<pid>` keys that outlived their processes; virtualising that view
 * deleted the staleness class, so the collector has nothing left to collect.
 * Retained as a no-op only so existing callers keep compiling until they are
 * updated. */
static inline void nxexec_prune_identities(void) {}

static inline int nxexec_resolve_path(const char *name, char *out_path,
                                      size_t size) {
  return execsvc_resolve_path(name, out_path, size);
}

/*
 * nxexec_run_foreground - watch a freshly-spawned child as a foreground job
 * (USR-TTY-01 #123), with a DEBOUNCED window-ownership test so a transient
 * window (stress --gui) is not mistaken for "this is a GUI app, detach".
 *
 * Returns NXEXEC_JOB_DETACHED if the child settled on its own persistent
 * window (the caller should stop hosting it), or NXEXEC_JOB_EXITED if the
 * child finished or was killed by Ctrl-C (ETX via keyboard IPC).
 */
static inline int nxexec_run_foreground_ex(int pid, int *out_status) {
  if (pid <= 0)
    return NXEXEC_JOB_EXITED;
  int last_win = -1;
  int stable = 0;
  while (1) {
    if (nxexec_window_stable(pid, &last_win, &stable))
      return NXEXEC_JOB_DETACHED; /* a persistent own window -> GUI app */

    /* Reap WITH the exit status (Phase 2 exit_code channel).  *out_status gets
     * the shell-style status: the exit code for a normal exit, 128+signal for a
     * killed child — this is what lets `nxshell -c` exit with the command's
     * status, which system()/os.execute() test. */
    int code = 0;
    if (OS1low_process_wait_status(pid, &code) != -1) {
      if (out_status)
        *out_status = (code >= 0) ? (code & 0xff) : (128 + (-code));
      return NXEXEC_JOB_EXITED; /* child finished (dead/zombie/gone) */
    }

    /* Relay keyboard input to the child (USR-TTY-01 #123 problem 2): a
     * windowless child can never hold compositor focus (focus is per-window),
     * so it never sees its own keystrokes unless we forward them here -
     * without this, any program that reads stdin interactively (a REPL, an
     * editor, ...) blocks forever on its first read() after printing a
     * prompt, indistinguishable from a hang. Ctrl-C stays a local kill
     * instead of being forwarded, matching the existing job-control model.
     *
     * No termios/cooked-mode layer exists anywhere in this OS - every
     * reader echoes its OWN input explicitly (nxshell.c's read(0,...) loop
     * is the reference). We still hold the window/focus while hosting, so
     * echo has to happen here too, in the same style, or the cursor never
     * advances even though the child is receiving every keystroke. */
    struct ipc_message m;
    if (try_recv(-1, &m) == 0 && m.type == IPC_TYPE_INPUT && m.data2 != 0) {
      char c = m.payload[0];
      if (c == 0x03) {
        kill_process(pid);
        print("^C\n");
        if (out_status)
          *out_status = 130; /* 128 + SIGINT, the shell convention */
        return NXEXEC_JOB_EXITED;
      }
      if (c == 0x1a) { /* Ctrl-Z: suspend the foreground job and hand back to
                        * the caller (NXEXEC_JOB_STOPPED). nxshell adopts it as
                        * a job; nxexec.c keeps its terminal open with a
                        * resume/kill prompt (never orphaning it). */
        if (OS1_process_stop(pid) == 0) {
          print("^Z\n");
          return NXEXEC_JOB_STOPPED;
        }
        /* stop refused (e.g. windowed/own-terminal child): relay as a key */
      }
      send(pid, &m);
      if (c == '\n' || c == '\r')
        print("\r\n");
      else if (c == '\b' || c == 127)
        print("\b \b");
      else if (c >= 32 && c < 127)
        print(m.payload);
    }
    OS1_sleep(15); /* time base for the debounce; keeps Ctrl-C responsive */
  }
}

/* nxexec_run_foreground - status-less form, for callers that only need the
 * NXEXEC_JOB_* outcome (nxexec.c's hosted terminal). */
static inline int nxexec_run_foreground(int pid) {
  return nxexec_run_foreground_ex(pid, 0);
}

/*
 * nxexec_wait_stopped_action - the hosted job was Ctrl-Z-suspended
 * (NXEXEC_JOB_STOPPED).  A standalone terminal has no shell to hand a stopped
 * job to, so instead of orphaning the process or closing the window it keeps
 * the window open and blocks here until the user chooses: Enter -> resume
 * (return 1), Ctrl-C -> kill the child (return 0).  Only these two, or the
 * program exiting, ever close a hosted terminal.
 */
static inline int nxexec_wait_stopped_action(int pid) {
  while (1) {
    struct ipc_message m;
    if (try_recv(-1, &m) == 0 && m.type == IPC_TYPE_INPUT && m.data2 != 0) {
      char c = m.payload[0];
      if (c == '\r' || c == '\n')
        return 1; /* resume */
      if (c == 0x03) {
        kill_process(pid);
        print("^C\n");
        return 0; /* killed */
      }
    }
    OS1_sleep(15);
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
  return execsvc_launch_search(argc, argv, out_path, NXEXEC_PATH_MAX,
      detached ? SPAWN_FLAG_DETACHED : 0u, NULL, 0, 0);
}

/*
 * nxexec_lookup_identity - read back the canonical display name nxexec
 * published for a pid (sys.proc.<pid>.name, Phase 3 identity).  The consumer
 * side of nxexec_register_identity, shared so the bar, the dock and the shell's
 * job table all name a process the SAME way instead of re-deriving it.
 * Returns 1 and fills out[] on success, 0 if the pid has no registered
 * identity (e.g. a system service that never went through nxexec).
 */
static inline int nxexec_lookup_identity(int pid, char *out, int outsz) {
  char key[48];
  snprintf(key, sizeof(key), "sys.proc.%d.name", pid);
  if (OS1_registry_get(key, out, (size_t)outsz) != 0 || !out[0])
    return 0;
  return 1;
}

/*
 * nxexec_strip_redirections - pull shell redirection operators (`<` `>` `>>`
 * `2>`) and their targets out of argv, open the targets, and build the
 * spawn_redir list the kernel dups into the child.  Both the spaced form
 * (`> out`) and the attached form (`>out`) are recognised.
 *
 * THIS IS EXECUTOR POLICY, NOT SHELL POLICY (maintainer, 2026-07-18: "nxexec
 * deve essere il modo principale e gestire tutto ciò che sta gestendo la
 * shell").  It lived in nxshell.c, which is why the GRAPHICAL launch path —
 * nxlauncher/nxfilem, which already spawn through nxexec correctly — could not
 * redirect or take arguments the way the terminal could.  Moving it here gives
 * every caller ONE parser: the shell, the hosted terminal, and the service.
 *
 * On return argv (and *pargc) hold ONLY the command and its real arguments;
 * redir[] (and *nredir) describe the child fd remap; fds[] (and *nfds) are the
 * handles THIS process opened — the caller MUST close them after the spawn (the
 * child holds its own dups).  Returns 0, or -1 after reporting an open/syntax
 * error (having closed anything already opened).  A missing input file (`<`) is
 * fatal; an output target is created (O_CREAT|O_TRUNC) / appended (`>>`).
 */
static inline int nxexec_strip_redirections(int *pargc, char *argv[],
                                            struct spawn_redir *redir,
                                            int *nredir, int fds[], int *nfds) {
  return execsvc_strip_redirections(pargc, argv, redir, nredir, fds, nfds);
}

/*
 * nxexec_spawn_search_redir - nxexec_spawn_search + fd redirection (Phase 4
 * shell `<`/`>`/`>>`/`2>`).  redir[]/nredir describe the child fd remap; the
 * CALLER opened each redir[].parent_fd and CLOSES them after this returns (the
 * kernel dup'd them into the child).  nredir == 0 behaves exactly like
 * nxexec_spawn_search.  Returns the PID or <= 0 on failure.
 */
/* Shared resolver/dispatcher. user_only is an execution-service safety
 * boundary: the ROOT daemon can only launch USER-class executables from
 * /bin; /sys system programs and MACHINE extensions require the caller's
 * own authority, so clients fall back to direct capability-clamped spawn.
 * The hosted command uses the exact same resolver with user_only=0. */
static inline int nxexec_spawn_search_redir_scoped(int argc, char *argv[],
                                                   char *out_path, int detached,
                                                   const struct spawn_redir *redir,
                                                   int nredir, int user_only) {
  return execsvc_launch_search(argc, argv, out_path, NXEXEC_PATH_MAX,
      detached ? SPAWN_FLAG_DETACHED : 0u, redir, nredir, user_only);
}

/* Command/launcher personality: same resolver with its own caller's
 * privilege clamp; the privileged service alone chooses user_only=1. */
static inline int nxexec_spawn_search_redir(int argc, char *argv[],
                                            char *out_path, int detached,
                                            const struct spawn_redir *redir,
                                            int nredir) {
  return nxexec_spawn_search_redir_scoped(
      argc, argv, out_path, detached, redir, nredir, 0);
}

/*
 * nxexec_spawn_pipe_consumer - create a pipe and spawn `argv` reading from it,
 * handing the WRITE end back to the caller.
 *
 * This is the pipeline's EXECUTOR half: making the channel, wiring the
 * consumer's fd 0 to it, and honouring the consumer's own `>`/`2>`.  What
 * PRODUCES the data stays with the caller, because that genuinely differs — a
 * shell may feed a builtin's output straight in, while a graphical caller
 * spawns a producer process.  Splitting it here is what stops the pipe wiring
 * from living only inside nxshell, the way redirection used to.
 *
 * Returns the consumer pid (<=0 on failure) and stores the write end in
 * *out_write_fd (-1 if none).  The caller closes the write end to signal EOF,
 * and closes it in ANY case — a forgotten write end means the consumer waits
 * for data that can never arrive.
 */
static inline int nxexec_spawn_pipe_consumer(int argc, char *argv[],
                                             char *out_path, int *out_write_fd) {
  int pfd[2] = {-1, -1};
  *out_write_fd = -1;
  if (pipe(pfd) != 0)
    return -1;

  struct spawn_redir redir[SPAWN_MAX_REDIR];
  int fds[SPAWN_MAX_REDIR], nredir = 0, nfds = 0;
  if (nxexec_strip_redirections(&argc, argv, redir, &nredir, fds, &nfds) != 0 ||
      argc == 0) {
    for (int i = 0; i < nfds; i++)
      close(fds[i]);
    close(pfd[0]);
    close(pfd[1]);
    return -1;
  }
  /* stdin from the pipe, appended AFTER the command's own redirections so an
   * explicit `< file` on the same stage stays visible to the kernel. */
  if (nredir < SPAWN_MAX_REDIR) {
    redir[nredir].child_fd = 0;
    redir[nredir].parent_fd = pfd[0];
    redir[nredir].source_pid = 0;
    nredir++;
  }
  int pid =
      nxexec_spawn_search_redir(argc, argv, out_path, /*detached=*/0, redir,
                                nredir);
  for (int i = 0; i < nfds; i++)
    close(fds[i]);
  close(pfd[0]); /* the consumer holds its own dup */
  if (pid <= 0) {
    close(pfd[1]);
    return pid;
  }
  *out_write_fd = pfd[1];
  return pid;
}

/* nxexec_spawn_detached - launcher-style spawn of an explicit path: the child
 * never inherits the spawner as ctty (kernel-enforced, SPAWN_FLAG_DETACHED). */
static inline int nxexec_spawn_detached(const char *path, int argc,
                                        char *const argv[]) {
  return execsvc_launch_explicit(path, argc, argv, SPAWN_FLAG_DETACHED, NULL, 0);
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
  char *argv[1] = {(char *)path};
  return execsvc_launch_hosted(1, argv);
}

/* nxexec_spawn_hosted_argv - same as nxexec_spawn_hosted(), but with an
 * explicit argv forwarded to /sys/bin/nxexec (which itself forwards the
 * whole argv to the child, see nxexec.c main()).  Used by callers that need
 * to pass extra arguments to the launched program (nxfilem: "kilo <path>",
 * "<assoc-prog> <path>").  argc must be in [1, NXEXEC_ARGV_MAX-1] (the -1
 * accounts for the prepended "/sys/bin/nxexec" entry; SPAWN_MAX_ARGS=16
 * is the kernel-side cap the dispatcher clamps to).
 */
static inline int nxexec_spawn_hosted_argv(int argc, char *argv[]) {
  return execsvc_launch_hosted(argc, argv);
}

#endif /* _USER_NXEXEC_H */