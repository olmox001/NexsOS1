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
#include <stdlib.h> /* getenv() — see the dynamic HOME resolution in
                      * nxexec_resolve_path()'s tier 2 below. */
#include <string.h>

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
#define NXEXEC_STABLE_POLLS 6

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
  int w = window_of_pid(pid);
  if (w > 0 && w == *last_win)
    (*stable)++;
  else
    *stable = (w > 0) ? 1 : 0;
  *last_win = w;
  return *stable >= NXEXEC_STABLE_POLLS;
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
                                                   size_t sz) {
  if (!name || !out || sz == 0)
    return name;

  size_t len = strlen(name);

  if (len >= 2 && ((name[0] == '"' && name[len - 1] == '"') ||
                   (name[0] == '\'' && name[len - 1] == '\''))) {

    len -= 2;

    if (len >= sz)
      len = sz - 1;

    memcpy(out, name + 1, len);
    out[len] = '\0';

    return out;
  }

  return name;
}

static inline int nxexec_resolve_path(const char *name, char *out_path,
                                      size_t sz) {
  if (!name || !*name || !out_path || sz == 0)
    return -1;

  char clean_name[NXEXEC_PATH_MAX];

  name = nxexec_strip_path_quotes(name, clean_name, sizeof(clean_name));

  if (name[0] == '/') { /* 1: root-assoluto */
    snprintf(out_path, sz, "%s", name);
    return 0;
  }

  if (name[0] == '~') { /* 2: home-relativo */
    /* Dynamic: ask getenv("HOME") first (lib.c currently stubs it to
     * NXEXEC_HOME_DIR itself, but that's an implementation detail of the
     * stub, not a contract) and fall back to NXEXEC_HOME_DIR only if HOME
     * is unset/empty — a real per-user HOME later becomes a getenv() change
     * alone, no nxexec.h edit, no rebuild of every consumer (nxshell.c,
     * nxlauncher.c, and whatever "standard for non-system programs" launch
     * path follows this pattern next). */
    const char *home = getenv("HOME");
    if (!home || !*home)
      home = NXEXEC_HOME_DIR;
    if (name[1] == '\0')
      snprintf(out_path, sz, "%s", home);
    else if (name[1] == '/')
      snprintf(out_path, sz, "%s%s", home, name + 1);
    else
      return -1; /* forma "~utente": non supportata, non c'è una pwdb
                    multi-utente */
    return 0;
  }

  if (strchr(name, '/')) { /* 3: process-vfs-relativo */
    char cwd[NXEXEC_PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != 0)
      snprintf(cwd, sizeof(cwd), "/"); /* fallback best-effort */
    size_t cl = strlen(cwd);
    if (cl > 0 && cwd[cl - 1] == '/')
      snprintf(out_path, sz, "%s%s", cwd, name);
    else
      snprintf(out_path, sz, "%s/%s", cwd, name);
    return 0;
  }

  return 1; /* 4: nome nudo -> il chiamante fa la ricerca /bin, /sys/bin */
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
static inline int nxexec_run_foreground(int pid) {
  if (pid <= 0)
    return NXEXEC_JOB_EXITED;
  int last_win = -1;
  int stable = 0;
  while (1) {
    if (nxexec_window_stable(pid, &last_win, &stable))
      return NXEXEC_JOB_DETACHED; /* a persistent own window -> GUI app */

    if (wait(pid) != -1)
      return NXEXEC_JOB_EXITED; /* child finished (dead/zombie/gone) */

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
        return NXEXEC_JOB_EXITED;
      }
      if (c == 0x1a) { /* Ctrl-Z: suspend the foreground job (Phase 2) */
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

/*
 * nxexec_spawn_search - resolve a bare program name against /bin then
 * /sys/bin (absolute paths bypass the search) and spawn it with argv.
 * out_path (NXEXEC_PATH_MAX bytes) receives the resolved path for the
 * caller's diagnostics.  Returns the PID or <= 0 on failure.
 * detached != 0 selects the DETACHED launch mode (see header comment).
 */
static inline int nxexec_spawn_search(int argc, char *argv[], char *out_path,
                                      int detached) {
  long (*do_spawn)(const char *, int, char *const[]) =
      detached ? OS1low_process_spawn_detached : OS1low_process_spawn;

  /* Dequote ONCE, up front, and write the cleaned name back into argv[0] so
   * it is used consistently for BOTH resolution and the bare-name /bin,
   * /sys/bin search — and so the child sees a clean argv[0] (its progname).
   * The previous code stripped quotes only inside nxexec_resolve_path()'s
   * local buffer, then fell through to the bare-name branch still holding the
   * quoted original, producing the "/bin/\"lua\"" not-found seen from
   * os.execute('"lua" ...'). */
  static char clean_name[NXEXEC_PATH_MAX];
  argv[0] =
      (char *)nxexec_strip_path_quotes(argv[0], clean_name, sizeof(clean_name));
  const char *name = argv[0];

  int r = nxexec_resolve_path(name, out_path, NXEXEC_PATH_MAX);
  if (r == 0)
    return (int)do_spawn(out_path, argc, argv);
  if (r < 0)
    return -1; /* path malformato (es. "~utente") */

  /* Nome nudo: ricerca /bin poi /sys/bin (ora sul nome ripulito). */
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

/* nxexec_spawn_hosted_argv - same as nxexec_spawn_hosted(), but with an
 * explicit argv forwarded to /sys/bin/nxexec (which itself forwards the
 * whole argv to the child, see nxexec.c main()).  Used by callers that need
 * to pass extra arguments to the launched program (nxfilem: "kilo <path>",
 * "<assoc-prog> <path>").  argc must be in [1, NXEXEC_ARGV_MAX-1] (the -1
 * accounts for the prepended "/sys/bin/nxexec" entry; SPAWN_MAX_ARGS=16
 * is the kernel-side cap the dispatcher clamps to).
 */
static inline int nxexec_spawn_hosted_argv(int argc, char *argv[]) {
  if (argc < 1 || !argv)
    return -1;
  if (argc + 1 > NXEXEC_ARGV_MAX)
    return -1;
  char *full[NXEXEC_ARGV_MAX];
  full[0] = (char *)"/sys/bin/nxexec";
  for (int i = 0; i < argc; i++)
    full[i + 1] = argv[i];
  return (int)OS1low_process_spawn_detached("/sys/bin/nxexec", argc + 1, full);
}

#endif /* _USER_NXEXEC_H */