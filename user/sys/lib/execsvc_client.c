/*
 * user/sys/lib/execsvc_client.c
 * Client stub for the execution service (Phase 9c).
 *
 * Deliberately its OWN translation unit rather than more code inside lib.c:
 * Phase 10a/12 will split service clients out of the libc entirely, and lib.c is
 * already the file whose entanglement those phases exist to undo.  Starting it
 * separate means that split is a Makefile move, not another extraction.
 *
 * WHAT THIS IS FOR: nxexec is meant to be THE standard executor and the POSIX
 * compatibility point for execution.  system() historically bypassed it
 * completely (it spawned the shell itself), so the POSIX surface had its own
 * private execution path — exactly the divergence the daemon exists to remove.
 *
 * WHAT IT IS NOT FOR: interactive foreground jobs.  The service becomes the
 * spawning parent, and while OBJ_CTL_SETOWNER restores AUTHORITY it does NOT
 * move the controlling terminal, so Ctrl-Z/Ctrl-C would break.  That is Phase 9d
 * (ctty handback), and until then interactive jobs keep the in-process path.
 */
#include <execsvc.h>
#include <os1.h>
#include <string.h>
#include <unistd.h>

/* write_full - a pipe write may be short; a partial request would be read as a
 * malformed one by the service, so finish it or fail. */
static int write_full(int fd, const char *buf, int len) {
  int done = 0;
  while (done < len) {
    long w = write(fd, buf + done, (unsigned long)(len - done));
    if (w <= 0)
      return done;
    done += (int)w;
  }
  return done;
}

/* read_full - same reasoning on the reply. */
static int read_full_c(int fd, char *buf, int want) {
  int got = 0;
  while (got < want) {
    long r = read(fd, buf + got, (unsigned long)(want - got));
    if (r <= 0)
      return got;
    got += (int)r;
  }
  return got;
}

/*
 * execsvc_spawn - ask the execution service to run argv, returning its pid.
 *
 * Returns > 0 (pid) on success, or < 0 if the service is unreachable or refuses
 * — the caller is expected to FALL BACK to spawning directly.  That fallback is
 * not defensive padding: the service is supervised by init and can be briefly
 * absent across a respawn, and a POSIX system() that failed in that window
 * would be a worse regression than the divergence being removed.
 */
int execsvc_spawn(int argc, char *const argv[], unsigned int flags) {
  if (argc <= 0 || argc > EXECSVC_ARG_MAX || !argv)
    return -1;

  int svc = OS1_port_open(OS1NX_PORT_EXEC);
  if (svc < 0)
    return -1; /* no service published: caller falls back */

  int reqp[2] = {-1, -1}, repp[2] = {-1, -1};
  int pid = -1;
  if (pipe(reqp) != 0) {
    OS1low_handle_close(svc);
    return -1;
  }
  if (pipe(repp) != 0) {
    close(reqp[0]);
    close(reqp[1]);
    OS1low_handle_close(svc);
    return -1;
  }

  /* Header + packed body: [redir][cwd\0][argv[i]\0...].  No redirections here:
   * system() inherits the caller's stdio, which the child gets by default. */
  struct execsvc_spawn_hdr hdr;
  char body[EXECSVC_BODY_MAX];
  unsigned int off = 0;
  int ok = 1;

  memset(&hdr, 0, sizeof(hdr));
  hdr.version = EXECSVC_VERSION;
  hdr.flags = flags;
  hdr.argc = argc;
  hdr.nredir = 0;
  body[off++] = '\0'; /* cwd: empty = service default */
  for (int i = 0; i < argc && ok; i++) {
    unsigned int n = (unsigned int)strlen(argv[i]) + 1;
    if (off + n > sizeof(body)) {
      ok = 0; /* refuse rather than silently truncate a command */
      break;
    }
    memcpy(body + off, argv[i], n);
    off += n;
  }
  hdr.body_len = off;

  if (ok) {
    /* Transfer the channels THROUGH the port: a handle index is meaningful only
     * inside one process's table, and cap_grant would need the service's PID —
     * which the port model deliberately does not expose. */
    struct ipc_message m;
    memset(&m, 0, sizeof(m));
    m.type = EXECSVC_REQ_SPAWN;
    int give[2];
    give[0] = reqp[0];
    give[1] = repp[1];
    if (OS1_port_send_caps(svc, &m, give, 2) == (long)sizeof(m)) {
      write_full(reqp[1], (const char *)&hdr, (int)sizeof(hdr));
      write_full(reqp[1], body, (int)off);
      close(reqp[1]); /* EOF: a short body must not hang the service */
      reqp[1] = -1;

      struct execsvc_spawn_rep rep;
      memset(&rep, 0, sizeof(rep));
      if (read_full_c(repp[0], (char *)&rep, (int)sizeof(rep)) ==
              (int)sizeof(rep) &&
          rep.version == EXECSVC_VERSION)
        pid = rep.pid;
    }
  }

  if (reqp[0] >= 0)
    close(reqp[0]);
  if (reqp[1] >= 0)
    close(reqp[1]);
  close(repp[0]);
  close(repp[1]);
  OS1low_handle_close(svc);
  return pid;
}
