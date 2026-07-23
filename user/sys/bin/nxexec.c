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

/*
 * read_full - a pipe read may return SHORT (it is a stream, and the writer may
 * still be filling it).  The request is a fixed-size record, so anything less
 * than the whole record is a malformed request, not a partial success — loop
 * until complete, or fail.
 */
static int read_full(int fd, void *buf, int want) {
  char *p = (char *)buf;
  int got = 0;
  while (got < want) {
    long r = read(fd, p + got, (unsigned long)(want - got));
    if (r <= 0)
      return got; /* 0 = writer closed early; <0 = error */
    got += (int)r;
  }
  return got;
}

/*
 * execsvc_handle_spawn - serve ONE spawn request (ASTRA §6.4 SRL service).
 *
 * The rendezvous message carries only references; the body arrives out-of-line
 * through the pipe the client delegated to us (its 1272-byte request cannot fit
 * in a 64-byte ipc_message payload).  m->from is kernel-stamped and is our only
 * trustworthy statement of WHO is asking — it is both the authorisation basis
 * and the logical owner we assign to the job.
 */
static void execsvc_handle_spawn(const struct ipc_message *m) {
  /* The channel handles arrive TRANSLATED into our own table by
   * SYS_PORT_SEND_CAPS, which publishes them in the leading payload slots — a
   * handle index is meaningless across process boundaries, so the client's own
   * numbers (whatever it held) are deliberately NOT used here. */
  int reqh = -1, reph = -1;
  memcpy(&reqh, m->payload + 0 * sizeof(int), sizeof(int));
  memcpy(&reph, m->payload + 1 * sizeof(int), sizeof(int));
  if (reqh < 0 || reph < 0)
    return; /* no usable channels: nothing to answer on */

  struct execsvc_spawn_hdr hdr;
  static char body[EXECSVC_BODY_MAX]; /* static: the service is single-threaded
                                       * and 4 KiB does not belong on the stack */
  struct execsvc_spawn_rep rep;
  struct execsvc_redir rq[SPAWN_MAX_REDIR];
  int nrq = 0;
  memset(&rep, 0, sizeof(rep));
  rep.version = EXECSVC_VERSION;
  rep.pid = -EINVAL;

  /* Validate the header BEFORE trusting any length it declares: body_len is a
   * client-chosen number, so it is bounded against EXECSVC_BODY_MAX first and
   * only then used to size a read. */
  if (read_full(reqh, (char *)&hdr, (int)sizeof(hdr)) == (int)sizeof(hdr) &&
      hdr.version == EXECSVC_VERSION && hdr.argc > 0 &&
      hdr.argc <= EXECSVC_ARG_MAX && hdr.nredir >= 0 &&
      hdr.nredir <= SPAWN_MAX_REDIR && hdr.body_len <= EXECSVC_BODY_MAX &&
      read_full(reqh, body, (int)hdr.body_len) == (int)hdr.body_len) {

    /* Parse the packed body.  EVERY step is bounded by body_len, so a truncated
     * or hostile body cannot walk past the buffer. */
    unsigned int off = 0;
    int ok_parse = 1;
    char *av[EXECSVC_ARG_MAX];

    unsigned int rbytes = (unsigned int)hdr.nredir * sizeof(struct execsvc_redir);
    if (rbytes > hdr.body_len) {
      ok_parse = 0;
    } else {
      memcpy(rq, body, rbytes);
      nrq = hdr.nredir;
      off = rbytes;
    }

    /* cwd, then argv — each a NUL-terminated run inside the body. */
    const char *cwd = "";
    if (ok_parse) {
      cwd = body + off;
      while (off < hdr.body_len && body[off])
        off++;
      if (off >= hdr.body_len)
        ok_parse = 0; /* unterminated: refuse rather than read on */
      else
        off++; /* step past the NUL */
    }
    for (int i = 0; ok_parse && i < hdr.argc; i++) {
      if (off >= hdr.body_len) {
        ok_parse = 0;
        break;
      }
      av[i] = body + off;
      while (off < hdr.body_len && body[off])
        off++;
      if (off >= hdr.body_len) {
        ok_parse = 0; /* unterminated final argument */
        break;
      }
      off++;
    }
    /* cwd: adopt the REQUESTER's for the duration of this spawn.
     *
     * The child inherits cwd from its CREATOR, which is this service — so
     * without this every relative path in a served command resolved against
     * the service's "/" instead of the caller's directory.  It was parsed and
     * then discarded ((void)cwd), which made the hole invisible in review: the
     * protocol looked like it carried cwd and nothing consumed it.
     *
     * Adopt-spawn-restore is safe HERE and only here: the service loop is
     * single-threaded and serves one request to completion, so no second
     * request can observe the borrowed directory.  It is a stopgap with a
     * named successor — Phase 9d carries cwd, ctty and environment in the
     * spawn itself, as ONE mechanism, which is what removes the borrowing.
     *
     * Restoring to "/" rather than to the previous value is deliberate: "/"
     * is the service's own cwd at start-up and the only value it is ever
     * supposed to hold at rest, so a failed restore cannot strand the service
     * inside a client's directory. */
    int cwd_adopted = 0;
    if (ok_parse && cwd[0])
      cwd_adopted = (chdir(cwd) == 0);

    /* Translate the abstract fd sources into kernel redirections (Q2 hybrid):
     *   GRANTED   -> the client delegated the handle to US, so it indexes our
     *                own table: source_pid stays 0.
     *   CLIENT_FD -> the handle stays in the CLIENT's table and the kernel
     *                takes it directly.  We pin the source to m->from — the
     *                KERNEL-STAMPED requester — so the service can never be
     *                talked into harvesting a third party's descriptors, even
     *                though it is privileged enough to do so. */
    struct spawn_redir redir[SPAWN_MAX_REDIR];
    int nredir = 0, bad = !ok_parse;
    for (int i = 0; !bad && i < nrq; i++) {
      redir[nredir].child_fd = rq[i].child_fd;
      redir[nredir].parent_fd = rq[i].ref;
      if (rq[i].source == EXECSVC_FD_GRANTED)
        redir[nredir].source_pid = 0;
      else if (rq[i].source == EXECSVC_FD_CLIENT_FD)
        redir[nredir].source_pid = m->from;
      else {
        bad = 1;
        break;
      }
      nredir++;
    }

    if (!bad) {
      char path[NXEXEC_PATH_MAX];
      int pid = nxexec_spawn_search_redir(hdr.argc, av, path,
                                          (hdr.flags & SPAWN_FLAG_DETACHED) != 0,
                                          redir, nredir);
      if (pid > 0) {
        /* Hand the job to the REQUESTER (Q3): we are the mechanical parent, but
         * the client owns it, so its kill/stop/cont authority still reaches the
         * job.  Without this the requesting shell would silently lose job
         * control over everything it launched. */
        char pidstr[16];
        snprintf(pidstr, sizeof(pidstr), "%d", pid);
        int ph = (int)OS1low_handle_create(OS1_NS_PROC, pidstr,
                                           OS1_RIGHT_DESTROY, OBJ_TYPE_PROCESS);
        if (ph >= 0) {
          OS1_object_ctl(ph, OBJ_CTL_SETOWNER, m->from);
          OS1low_handle_close(ph);
        }
        rep.pid = pid;
        rep.owner_pid = m->from;
        snprintf(rep.resolved, EXECSVC_PATH_MAX, "%s", path);
      } else {
        rep.pid = -ENOENT;
      }
    }

    /* Give the borrowed directory back before answering, so the service is at
     * rest in a known place no matter which branch above ran. */
    if (cwd_adopted)
      chdir("/");
  }

  write(reph, (const char *)&rep, sizeof(rep));
  /* Drop the delegated channels AND any handle the client GRANTED us for the
   * child: the kernel already dup'd what it needed into the child, so holding
   * them here would keep pipes/files open for the service's lifetime — a slow
   * descriptor leak in a process that never exits.  CLIENT_FD refs are NOT ours
   * to close: they live in the client's table. */
  close(reqh);
  close(reph);
  for (int i = 0; i < nrq && i < SPAWN_MAX_REDIR; i++) {
    if (rq[i].source == EXECSVC_FD_GRANTED)
      close(rq[i].ref);
  }
}

/*
 * nxexec_service - the R6 privileged execution daemon.
 *
 * Publishes the OS1nx_exec port and serves spawn requests.  Taking the RECEIVE
 * right IS the claim on the service identity: if another nxexec already serves
 * it, OS1_port_create fails and this instance exits instead of competing.
 */
static int nxexec_service(void) {
  /*
   * Claiming the name is RETRIED, briefly, before giving up.
   *
   * The port is unpublished when its last RECEIVER handle closes, which
   * happens while the dead instance's handle table is being torn down — after
   * init has already seen the corpse and queued a respawn.  So the ordinary,
   * expected respawn lands in a window where the old name is still served and
   * OS1_port_create() returns -EEXIST.  Exiting there turned a clean restart
   * into a crash loop: init respawns, the new instance loses the same race,
   * exits, init respawns... which is what the supervisor's backoff then had to
   * absorb, and what presented as init endlessly restarting things.
   *
   * A bounded retry — not an unbounded one: if some OTHER process genuinely
   * holds the name, this instance must still exit and say so rather than spin
   * forever competing for a service identity it does not own.
   */
#define SERVICE_CLAIM_TRIES 20
#define SERVICE_CLAIM_WAIT_MS 100
  int port = -1;
  for (int attempt = 0; attempt < SERVICE_CLAIM_TRIES; attempt++) {
    port = OS1_port_create(OS1NX_PORT_EXEC);
    if (port >= 0)
      break;
    OS1_sleep(SERVICE_CLAIM_WAIT_MS);
  }
  if (port < 0) {
    printf("nxexec: cannot publish %s after %d attempts (already served?)\n",
           OS1NX_PORT_EXEC, SERVICE_CLAIM_TRIES);
    return 1;
  }
  printf("nxexec: execution service listening on %s\n", OS1NX_PORT_EXEC);
  for (;;) {
    struct ipc_message m;
    long r = OS1_port_recv(port, &m);
    if (r == 0)
      break; /* every sender closed: nothing can ever arrive again */
    if (r < 0)
      continue;
    if (m.type == EXECSVC_REQ_SPAWN)
      execsvc_handle_spawn(&m);
  }
  OS1low_handle_close(port);
  return 0;
}

int main(int argc, char *argv[]) {
  /* Service mode (R6 daemon).  Kept as a MODE of the same binary rather than a
   * separate one: the execution policy below (path resolution, argv[0]
   * normalisation, hosting, job control) must not fork into two divergent
   * copies — that divergence is the very bug this daemon exists to remove. */
  if (argc >= 2 && strcmp(argv[1], "--service") == 0)
    return nxexec_service();

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