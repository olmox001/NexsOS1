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
#include <fcntl.h> /* O_* + open() — redirection parsing below */
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

/*
 * Phase 3 — process identity (PLAN-2026-07-17): nxexec is the launch authority,
 * so it assigns the ONE canonical display name for a program (the exe's
 * basename, quotes already stripped: "/bin/lua" -> "lua") and publishes it two
 * ways, per the maintainer's "window-title convention + registry namespace"
 * decision:
 *   - it titles the window it hosts with that name (the bar reads titles);
 *   - it registers sys.proc.<pid>.name / .icon in the registry so the bar/dock
 *     can serialise ANY pid's identity by number, independent of an app's own
 *     (possibly colliding or changing) window title.
 * The icon key is the same canonical name; nxicon classifies it.
 */
static inline const char *nxexec_basename(const char *path) {
  if (!path)
    return "";
  const char *b = path;
  for (const char *p = path; *p; p++)
    if (*p == '/')
      b = p + 1;
  return b;
}

static inline void nxexec_register_identity(int pid, const char *name) {
  /* `sys.proc.<pid>.name` is now VIRTUAL — computed by the kernel from the
   * process table on every read (Phase 5b).  Writing it here would recreate the
   * second, best-effort copy whose staleness required a garbage collector.
   * Only the ICON is still published, and it is program-keyed rather than
   * pid-keyed: an icon is a property of the PROGRAM, so keying it by process
   * instance was the modelling error that made it go stale. */
  char key[64];
  if (!name || !*name)
    return;
  snprintf(key, sizeof(key), "sys.appicon.%s", name);
  OS1_registry_set(key, name);
  (void)pid;
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

  /* argv[0] = the RESOLVED path (Phase 9) — see nxexec_spawn_search_redir. */
  int r = nxexec_resolve_path(name, out_path, NXEXEC_PATH_MAX);
  if (r == 0) {
    argv[0] = out_path;
    return (int)do_spawn(out_path, argc, argv);
  }
  if (r < 0)
    return -1; /* path malformato (es. "~utente") */

  /* Nome nudo: ricerca /bin poi /sys/bin (ora sul nome ripulito). */
  snprintf(out_path, NXEXEC_PATH_MAX, "/bin/%s", name);
  argv[0] = out_path;
  int pid = (int)do_spawn(out_path, argc, argv);
  if (pid > 0)
    return pid;
  snprintf(out_path, NXEXEC_PATH_MAX, "/sys/bin/%s", name);
  argv[0] = out_path;
  return (int)do_spawn(out_path, argc, argv);
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
  int argc = *pargc, out = 0;
  *nredir = 0;
  *nfds = 0;
  for (int i = 0; i < argc; i++) {
    char *t = argv[i];
    int child_fd, oflags;
    const char *fname = 0;
    if (strcmp(t, "<") == 0) {
      child_fd = 0, oflags = O_RDONLY;
    } else if (strcmp(t, ">") == 0) {
      child_fd = 1, oflags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (strcmp(t, ">>") == 0) {
      child_fd = 1, oflags = O_WRONLY | O_CREAT | O_APPEND;
    } else if (strcmp(t, "2>") == 0) {
      child_fd = 2, oflags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (t[0] == '<' && t[1]) {
      child_fd = 0, oflags = O_RDONLY, fname = t + 1;
    } else if (t[0] == '2' && t[1] == '>' && t[2]) {
      child_fd = 2, oflags = O_WRONLY | O_CREAT | O_TRUNC, fname = t + 2;
    } else if (t[0] == '>' && t[1] == '>' && t[2]) {
      child_fd = 1, oflags = O_WRONLY | O_CREAT | O_APPEND, fname = t + 2;
    } else if (t[0] == '>' && t[1]) {
      child_fd = 1, oflags = O_WRONLY | O_CREAT | O_TRUNC, fname = t + 1;
    } else {
      argv[out++] = t; /* an ordinary command argument */
      continue;
    }
    if (!fname) { /* bare operator token: the filename is the next token */
      if (i + 1 >= argc) {
        printf("nxexec: syntax error near '%s'\n", t);
        goto fail;
      }
      fname = argv[++i];
    }
    if (*nredir >= SPAWN_MAX_REDIR) {
      print("nxexec: too many redirections\n");
      goto fail;
    }
    int fd = open(fname, oflags, 0644);
    if (fd < 0) {
      printf("nxexec: cannot open %s\n", fname);
      goto fail;
    }
    fds[(*nfds)++] = fd;
    redir[*nredir].child_fd = child_fd;
    redir[*nredir].parent_fd = fd;
    redir[*nredir].source_pid = 0; /* our own table (non-zero selects the
                                    * privileged take-from-another-process
                                    * path — never leave it uninitialised) */
    (*nredir)++;
  }
  argv[out] = 0;
  *pargc = out;
  return 0;
fail:
  for (int j = 0; j < *nfds; j++)
    close(fds[j]);
  *nfds = 0;
  *nredir = 0;
  return -1;
}

/*
 * nxexec_spawn_search_redir - nxexec_spawn_search + fd redirection (Phase 4
 * shell `<`/`>`/`>>`/`2>`).  redir[]/nredir describe the child fd remap; the
 * CALLER opened each redir[].parent_fd and CLOSES them after this returns (the
 * kernel dup'd them into the child).  nredir == 0 behaves exactly like
 * nxexec_spawn_search.  Returns the PID or <= 0 on failure.
 */
static inline int nxexec_spawn_search_redir(int argc, char *argv[],
                                            char *out_path, int detached,
                                            const struct spawn_redir *redir,
                                            int nredir) {
  unsigned int flags = detached ? SPAWN_FLAG_DETACHED : 0u;
  static char clean_name[NXEXEC_PATH_MAX];
  argv[0] =
      (char *)nxexec_strip_path_quotes(argv[0], clean_name, sizeof(clean_name));
  const char *name = argv[0];

  /* The child's argv[0] is the RESOLVED path, not the bare name as typed
   * (Phase 9, "esecuzione da path non diretto"): a program that re-executes
   * itself from its own argv[0] — lua's test suite rebuilds every command as
   * `"<progname>" ...` — must get a DIRECT path, or the re-execution depends on
   * the search succeeding again.  It also makes the terminal path agree with
   * the launcher path, which already passes an absolute path (progname was
   * `/bin/lua` from the launcher but bare `lua` from the shell). */
  int r = nxexec_resolve_path(name, out_path, NXEXEC_PATH_MAX);
  if (r == 0) {
    argv[0] = out_path;
    return (int)OS1low_process_spawn_redir(out_path, argc, argv, flags, redir,
                                           nredir);
  }
  if (r < 0)
    return -1; /* malformed path (e.g. "~user") */

  /* Bare name: search /bin then /sys/bin.  A failed candidate aborts its own
   * half-built child (kernel process_abort_spawn) and drops the dup'd handle
   * refs, so retrying the next path with the same redir list is safe. */
  snprintf(out_path, NXEXEC_PATH_MAX, "/bin/%s", name);
  argv[0] = out_path;
  int pid = (int)OS1low_process_spawn_redir(out_path, argc, argv, flags, redir,
                                            nredir);
  if (pid > 0)
    return pid;
  snprintf(out_path, NXEXEC_PATH_MAX, "/sys/bin/%s", name);
  argv[0] = out_path;
  return (int)OS1low_process_spawn_redir(out_path, argc, argv, flags, redir,
                                         nredir);
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