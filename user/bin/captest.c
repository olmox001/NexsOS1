/*
 * user/bin/captest.c
 * Capability / object-manager test (ASTRA §6.1/6.2/6.5).
 *
 * Proves the REAL capability layer end to end, using the OS1 NATIVE base API
 * (OS1low_* / OS1_object_*) — not POSIX:
 *   1. handle_create(FILE, READ) yields a handle; OS1_object_read works.
 *   2. rights are SEPARABLE+ENFORCED: write through a READ-only handle -> -EPERM.
 *   3. cap_query reports (type, rights).
 *   4. UNFORGEABILITY: a handle integer with no slot -> -EBADF (query + read).
 *   5. ATTENUATION: duplicate(READ|WRITE) of a READ|DUP handle yields READ only.
 *   6. duplicate needs OS1_RIGHT_DUPLICATE on the source -> else -EPERM.
 *   7. lifecycle: close -> 0; use after close -> -EBADF; double close -> -EBADF.
 *   8. PROCESS object: a handle to self; rights enforced (read needs READ).
 *   9. DELEGATION is gated: grant without OS1_RIGHT_TRANSFER -> -EPERM.
 *  10. O_CREAT: CREATE|WRITE creates a missing file; CREATE bit is stripped.
 *  11. Creation is explicit: no CREATE (or CREATE w/o WRITE) -> -ENOENT.
 *  12. Acquisition ACL: CREATE under /sys/bin -> -EACCES for USER.
 *  13. SYS_MKDIR shares the seam: /home ok (+empty rmdir), /sys/bin -EACCES.
 * Results go to a window AND the serial console (grep "[captest]").
 */
#include <execsvc.h> /* execution-service protocol (Phase 9 section below) */
#include <fcntl.h>
#include <os1.h>
#include <stdio.h>
#include <stdlib.h> /* getenv/setenv/unsetenv (Phase 17 section) */
#include <string.h>
#include <sys/wait.h> /* waitpid + WIFEXITED/WEXITSTATUS (Phase 9b section) */
#include <unistd.h>

static int failures = 0;
static int checks = 0;

static void check(int win_id, const char *name, int ok) {
  checks++;
  printf_win(win_id, "%s: %s\n", name, ok ? "PASS" : "FAIL");
  printf("[captest] %s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok)
    failures++;
}

/* section - group header.  This suite is the SYSTEM regression test and grows
 * with each phase (maintainer directive 2026-07-18), so results are organised
 * by layer rather than appended flat: a failure should say WHICH layer broke.
 * Layer order deliberately mirrors the debugging order used on this project:
 * kernel object/capability -> kernel IPC & streams -> POSIX personality. */
static void section(int win_id, const char *name) {
  printf_win(win_id, "-- %s --\n", name);
  printf("[captest] ---- %s ----\n", name);
}

int main(void) {
  int win_id = create_window(140, 140, 440, 320, "Cap Test");
  if (win_id < 0)
    return 1;

  const char *path = "/etc/init.cfg"; /* readable+writable rootfs file */
  char a[16];
  int ok;

  printf_win(win_id, "Object/capability tests...\n");
  printf("[captest] start (OS1 native object/capability ABI)\n");

  /* 1. create a READ capability to a file and read through it */
  int h = (int)OS1low_handle_create(OS1_NS_FS, path, OS1_RIGHT_READ, OBJ_TYPE_FILE);
  memset(a, 0, sizeof(a));
  ok = h >= 0 && OS1_object_read(h, a, 8) == 8;
  check(win_id, "create+object_read", ok);

  /* 2. rights enforced: no WRITE right -> object_write denied */
  ok = OS1_object_write(h, "ZZZZ", 4) == -EPERM;
  check(win_id, "deny-write-no-right", ok);

  /* 3. cap_query reports type + the exact held rights */
  long q = OS1low_cap_query(h);
  ok = q >= 0 && OS1_CAPQ_TYPE(q) == OBJ_TYPE_FILE &&
       OS1_CAPQ_RIGHTS(q) == OS1_RIGHT_READ;
  check(win_id, "cap_query", ok);

  /* 4. unforgeability: a handle with no installed slot is -EBADF */
  ok = OS1low_cap_query(9999) == -EBADF &&
       OS1_object_read(9999, a, 1) == -EBADF &&
       OS1low_cap_query(-1) == -EBADF;
  check(win_id, "unforgeable", ok);

  /* 5. attenuation: dup(READ|WRITE) of a READ|DUP handle yields READ only */
  int hd = (int)OS1low_handle_create(OS1_NS_FS, path,
                                     OS1_RIGHT_READ | OS1_RIGHT_DUPLICATE,
                                     OBJ_TYPE_FILE);
  int h2 = (int)OS1low_handle_duplicate(hd, OS1_RIGHT_READ | OS1_RIGHT_WRITE);
  long q2 = (h2 >= 0) ? OS1low_cap_query(h2) : -1;
  ok = hd >= 0 && h2 >= 0 && q2 >= 0 &&
       OS1_CAPQ_RIGHTS(q2) == OS1_RIGHT_READ; /* WRITE attenuated away */
  check(win_id, "dup-attenuate", ok);

  /* 6. duplicate needs OS1_RIGHT_DUPLICATE on the source (h lacks it) */
  ok = OS1low_handle_duplicate(h, OS1_RIGHT_READ) == -EPERM;
  check(win_id, "dup-needs-right", ok);

  /* 7. lifecycle: close, then use-after-close and double-close are -EBADF */
  ok = OS1low_handle_close(h) == 0 && OS1_object_read(h, a, 1) == -EBADF &&
       OS1low_handle_close(h) == -EBADF;
  check(win_id, "close-lifecycle", ok);

  /* 8. PROCESS object: a capability to self; rights enforced */
  char pidbuf[16];
  sprintf(pidbuf, "%d", get_pid());
  int ph = (int)OS1low_handle_create(OS1_NS_PROC, pidbuf, OS1_RIGHT_WAIT,
                                     OBJ_TYPE_PROCESS);
  long pq = (ph >= 0) ? OS1low_cap_query(ph) : -1;
  ok = ph >= 0 && pq >= 0 && OS1_CAPQ_TYPE(pq) == OBJ_TYPE_PROCESS &&
       OS1_CAPQ_RIGHTS(pq) == OS1_RIGHT_WAIT &&
       OS1_object_read(ph, a, 1) == -EPERM; /* no READ right on a process */
  check(win_id, "process-object", ok);
  printf("[captest] object_wait(self)=%ld (alive=-1)\n", OS1_object_wait(ph, 0));

  /* 8b. PROCESS object READ (state via the object mechanism): a READ handle to
   * self reads the live status block — it begins "pid=" and is non-empty.  This
   * is the process NOTIFYING its state through OS1_object_read, not a side
   * channel. */
  {
    int phr = (int)OS1low_handle_create(OS1_NS_PROC, pidbuf, OS1_RIGHT_READ,
                                        OBJ_TYPE_PROCESS);
    char st[128];
    memset(st, 0, sizeof(st));
    long rn = (phr >= 0) ? OS1_object_read(phr, st, sizeof(st) - 1) : -1;
    ok = phr >= 0 && rn > 0 && strncmp(st, "pid=", 4) == 0;
    check(win_id, "process-read-state", ok);
    if (phr >= 0)
      OS1low_handle_close(phr);
  }

  /* 8c. namespace → TYPED object (deep fusion): acquire /proc/<self> via the
   * filesystem namespace (a PATH, not a pid) — it resolves to the SAME PROCESS
   * capability object (cap_query confirms the type) and reads its state.  Proves
   * "everything resolvable in the namespace is a typed capability object". */
  {
    char procpath[32];
    sprintf(procpath, "/proc/%d", get_pid());
    int pf = (int)OS1low_handle_create(OS1_NS_FS, procpath, OS1_RIGHT_READ,
                                       OBJ_TYPE_FILE); /* type hint overridden */
    long pq2 = (pf >= 0) ? OS1low_cap_query(pf) : -1;
    char st2[128];
    memset(st2, 0, sizeof(st2));
    long rn2 = (pf >= 0) ? OS1_object_read(pf, st2, sizeof(st2) - 1) : -1;
    ok = pf >= 0 && pq2 >= 0 && OS1_CAPQ_TYPE(pq2) == OBJ_TYPE_PROCESS &&
         rn2 > 0 && strncmp(st2, "pid=", 4) == 0;
    check(win_id, "proc-namespace-object", ok);
    if (pf >= 0)
      OS1low_handle_close(pf);
  }

  /* 9. delegation gated: grant without OS1_RIGHT_TRANSFER -> -EPERM */
  int hng = (int)OS1low_handle_create(OS1_NS_FS, path, OS1_RIGHT_READ,
                                      OBJ_TYPE_FILE);
  ok = hng >= 0 && OS1low_cap_grant(get_pid(), hng, OS1_RIGHT_READ) == -EPERM;
  check(win_id, "grant-needs-transfer", ok);

  /* 10. O_CREAT via capability (C1): CREATE|WRITE on a MISSING path creates
   * the file behind the vfs_write_allowed seam; CREATE is acquisition-only
   * (stripped from the installed rights, never visible to cap_query); data
   * written through the handle reads back via a fresh READ handle. */
  {
    const char *npath = "/home/captest.tmp";
    OS1_fs_unlink(npath); /* ensure missing; ignore result */
    int hc = (int)OS1low_handle_create(OS1_NS_FS, npath,
                                       OS1_RIGHT_WRITE | OS1_RIGHT_CREATE,
                                       OBJ_TYPE_FILE);
    long qc = (hc >= 0) ? OS1low_cap_query(hc) : -1;
    long wn = (hc >= 0) ? OS1_object_write(hc, "cap!", 4) : -1;
    if (hc >= 0)
      OS1low_handle_close(hc);
    int hr = (int)OS1low_handle_create(OS1_NS_FS, npath, OS1_RIGHT_READ,
                                       OBJ_TYPE_FILE);
    char rb[8];
    memset(rb, 0, sizeof(rb));
    long rn = (hr >= 0) ? OS1_object_read(hr, rb, 4) : -1;
    if (hr >= 0)
      OS1low_handle_close(hr);
    ok = hc >= 0 && qc >= 0 && OS1_CAPQ_RIGHTS(qc) == OS1_RIGHT_WRITE &&
         wn == 4 && rn == 4 && memcmp(rb, "cap!", 4) == 0;
    check(win_id, "create-via-handle", ok);
    OS1_fs_unlink(npath);
  }

  /* 11. creation is explicit: a missing path without CREATE stays -ENOENT,
   * and CREATE without WRITE is meaningless (also -ENOENT). */
  ok = OS1low_handle_create(OS1_NS_FS, "/home/captest.missing",
                            OS1_RIGHT_WRITE, OBJ_TYPE_FILE) == -ENOENT &&
       OS1low_handle_create(OS1_NS_FS, "/home/captest.missing",
                            OS1_RIGHT_READ | OS1_RIGHT_CREATE,
                            OBJ_TYPE_FILE) == -ENOENT;
  check(win_id, "create-needs-flag+write", ok);

  /* 12. acquisition ACL: CREATE under the machine-only /sys/bin tree is
   * denied at handle_create (the same vfs_write_allowed seam) — USER never
   * acquires a write/create capability into the boot chain. */
  ok = OS1low_handle_create(OS1_NS_FS, "/sys/bin/captest.t",
                            OS1_RIGHT_WRITE | OS1_RIGHT_CREATE,
                            OBJ_TYPE_FILE) == -EACCES;
  check(win_id, "create-acl-sysbin", ok);

  /* 13. SYS_MKDIR shares the seam: /home create+remove works (also covers
   * empty-directory unlink), /sys/bin is -EACCES. */
  ok = _sys_mkdir("/home/captest.dir") == 0 &&
       OS1_fs_unlink("/home/captest.dir") == 0 &&
       _sys_mkdir("/sys/bin/captest.dir") == -EACCES;
  check(win_id, "mkdir-seam", ok);

  /* --- 14..18: service PORTS (ASTRA §6.5 — a port IS a capability) --------
   * These verify the property that motivates ports: a client reaches a SERVICE
   * BY NAME with an unforgeable capability, never by pid. */
  const char *pname = "OS1nx_captest";
  int psrv = OS1_port_create(pname); /* service side: takes the RECEIVE right */
  check(win_id, "port-create", psrv >= 0);

  /* 15. the name is now SERVED: a second receiver is refused, so a rogue
   * process cannot steal a published service identity by racing it. */
  int pdup = OS1_port_create(pname);
  check(win_id, "port-name-unique", pdup < 0);
  if (pdup >= 0)
    OS1low_handle_close(pdup);

  /* 16. a client acquires a SEND-only capability by NAME (no pid involved). */
  int pcli = OS1_port_open(pname);
  check(win_id, "port-open-send", pcli >= 0);

  /* 17. round-trip: send through the client capability, receive on the service
   * capability, and the kernel-stamped `from` must be our own pid (unforgeable
   * — we deliberately fill it with a lie below). */
  struct ipc_message tx, rx;
  memset(&tx, 0, sizeof(tx));
  memset(&rx, 0, sizeof(rx));
  tx.type = 4242;
  tx.data1 = 0xC0FFEE;
  tx.from = 999999; /* forged: the kernel must overwrite this */
  ok = pcli >= 0 && psrv >= 0 &&
       OS1_port_send(pcli, &tx) == (long)sizeof(tx) &&
       OS1_port_recv(psrv, &rx) == (long)sizeof(rx) && rx.type == 4242 &&
       rx.data1 == 0xC0FFEE && rx.from == get_pid();
  check(win_id, "port-roundtrip+from-unforgeable", ok);

  /* 18. an unpublished name yields no capability at all. */
  int pmiss = OS1_port_open("OS1nx_nosuchservice");
  check(win_id, "port-open-missing", pmiss < 0);
  if (pmiss >= 0)
    OS1low_handle_close(pmiss);

  if (pcli >= 0) OS1low_handle_close(pcli);
  if (psrv >= 0) OS1low_handle_close(psrv);

  /* ================= process ownership / authority (Q3) ================= */
  section(win_id, "process ownership");
  {
    /* A DESTROY capability to SELF is always acquirable (self-kill is allowed),
     * which gives us a legitimately-held handle to probe SETOWNER with. */
    int selfh = (int)OS1low_handle_create(OS1_NS_PROC, "self",
                                          OS1_RIGHT_DESTROY, OBJ_TYPE_PROCESS);
    if (selfh < 0) { /* namespace wants a decimal pid string */
      char pidstr[16];
      snprintf(pidstr, sizeof(pidstr), "%d", get_pid());
      selfh = (int)OS1low_handle_create(OS1_NS_PROC, pidstr, OS1_RIGHT_DESTROY,
                                        OBJ_TYPE_PROCESS);
    }
    check(win_id, "own-self-destroy-cap", selfh >= 0);

    /* THE security property: setting a logical owner DELEGATES authority over a
     * process, so an unprivileged caller must be refused even though it holds a
     * legitimate DESTROY capability.  Otherwise a process could re-home itself
     * under a victim and borrow that victim's authority chain. */
    long r = (selfh >= 0) ? OS1_object_ctl(selfh, OBJ_CTL_SETOWNER, 1) : 0;
    check(win_id, "own-setowner-denied-unprivileged", selfh >= 0 && r < 0);

    /* A nonsense owner is rejected on argument grounds, not silently taken. */
    long r2 = (selfh >= 0) ? OS1_object_ctl(selfh, OBJ_CTL_SETOWNER, 0) : 0;
    check(win_id, "own-setowner-rejects-bad-arg", selfh >= 0 && r2 < 0);

    /* Regression guard for the ancestry walk itself: adding owner_pid changed
     * process_kill_allowed(), so verify the ordinary self/descendant path still
     * resolves (a WAIT capability to self must still be acquirable). */
    int waith = (int)OS1low_handle_create(OS1_NS_PROC, "self", OS1_RIGHT_WAIT,
                                          OBJ_TYPE_PROCESS);
    if (waith < 0) {
      char pidstr[16];
      snprintf(pidstr, sizeof(pidstr), "%d", get_pid());
      waith = (int)OS1low_handle_create(OS1_NS_PROC, pidstr, OS1_RIGHT_WAIT,
                                        OBJ_TYPE_PROCESS);
    }
    check(win_id, "own-ancestry-walk-intact", waith >= 0);
    if (waith >= 0) OS1low_handle_close(waith);
    if (selfh >= 0) OS1low_handle_close(selfh);
  }

  /* ============================ pipes (§6.2) ============================ */
  section(win_id, "pipes");
  {
    int pf[2] = {-1, -1};
    ok = pipe(pf) == 0 && pf[0] >= 0 && pf[1] >= 0;
    check(win_id, "pipe-create", ok);

    if (ok) {
      /* The ends are DIFFERENT capabilities to one object: read end must not be
       * writable, write end must not be readable. */
      long qr = OS1low_cap_query(pf[0]);
      long qw = OS1low_cap_query(pf[1]);
      check(win_id, "pipe-type-is-PIPE",
            qr >= 0 && OS1_CAPQ_TYPE(qr) == OBJ_TYPE_PIPE);
      check(win_id, "pipe-ends-rights-split",
            qr >= 0 && qw >= 0 && (OS1_CAPQ_RIGHTS(qr) & OS1_RIGHT_READ) &&
                !(OS1_CAPQ_RIGHTS(qr) & OS1_RIGHT_WRITE) &&
                (OS1_CAPQ_RIGHTS(qw) & OS1_RIGHT_WRITE) &&
                !(OS1_CAPQ_RIGHTS(qw) & OS1_RIGHT_READ));

      char rb[16];
      memset(rb, 0, sizeof(rb));
      ok = write(pf[1], "hello", 5) == 5 && read(pf[0], rb, 5) == 5 &&
           memcmp(rb, "hello", 5) == 0;
      check(win_id, "pipe-roundtrip", ok);

      /* EOF is the property the lua fix depends on: a reader must get 0 (not a
       * block) once every writer is gone. */
      close(pf[1]);
      check(win_id, "pipe-eof-on-last-writer-close", read(pf[0], rb, 4) == 0);
      close(pf[0]);
    }
  }

  /* ================= POSIX personality (standardisation) ================ */
  section(win_id, "posix surface");
  {
    /* getpid must be the POSIX spelling of the OS1 verb, not a second impl. */
    check(win_id, "posix-getpid-matches-OS1", getpid() == get_pid());

    /* isatty is a real capability-TYPE test, not "fd < 3".  This is exactly the
     * property that made `echo ... | lua` work: with a redirected stdin lua must
     * see isatty()==0 and EXECUTE stdin instead of opening a REPL. */
    check(win_id, "posix-isatty-console", isatty(1) == 1);
    int pf2[2] = {-1, -1};
    if (pipe(pf2) == 0) {
      check(win_id, "posix-isatty-pipe-is-false", isatty(pf2[0]) == 0);
      close(pf2[0]);
      close(pf2[1]);
    } else {
      check(win_id, "posix-isatty-pipe-is-false", 0);
    }

    const char *tp = "/home/captest.trunc";
    int tfd = open(tp, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (tfd >= 0) {
      check(win_id, "posix-isatty-file-is-false", isatty(tfd) == 0);
      ok = write(tfd, "0123456789", 10) == 10;
      /* shrink: composed from lseek+read+rewrite (offset-0 write REPLACES) */
      ok = ok && ftruncate(tfd, 4) == 0 && lseek(tfd, 0, SEEK_END) == 4;
      check(win_id, "posix-ftruncate-shrink", ok);
      /* extend: zero-fill from the old EOF */
      ok = ftruncate(tfd, 9) == 0 && lseek(tfd, 0, SEEK_END) == 9;
      check(win_id, "posix-ftruncate-extend", ok);
      /* to empty: the case POSIX write(fd,buf,0) CANNOT express (it is a
       * no-op), hence the OBJ_CTL_TRUNCATE verb */
      ok = ftruncate(tfd, 0) == 0 && lseek(tfd, 0, SEEK_END) == 0;
      check(win_id, "posix-ftruncate-to-empty", ok);
      close(tfd);
      OS1_fs_unlink(tp);
    } else {
      check(win_id, "posix-ftruncate-shrink", 0);
    }

    /* fdopen: wrap an existing descriptor in a FILE* — how ported POSIX code
     * consumes a pipe/file through stdio.  Was DECLARED but unimplemented. */
    const char *fp = "/home/captest.fdopen";
    int wfd = open(fp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ok = 0;
    if (wfd >= 0) {
      write(wfd, "abc", 3);
      close(wfd);
      int rfd = open(fp, O_RDONLY, 0);
      if (rfd >= 0) {
        FILE *f = fdopen(rfd, "r");
        char fb[8];
        memset(fb, 0, sizeof(fb));
        ok = f && fread(fb, 1, 3, f) == 3 && memcmp(fb, "abc", 3) == 0;
        if (f)
          fclose(f); /* also closes rfd (POSIX ownership) */
        else
          close(rfd);
      }
      OS1_fs_unlink(fp);
    }
    check(win_id, "posix-fdopen-stream", ok);
  }

  /* ========== per-process view is VIRTUAL, never stale (Phase 5b) ======== */
  section(win_id, "virtual proc view");
  {
    char v[64];
    char k[64];
    /* Our OWN entry must be live and correct, with nobody having written it. */
    snprintf(k, sizeof(k), "sys.proc.%d.name", get_pid());
    memset(v, 0, sizeof(v));
    ok = OS1_registry_get(k, v, sizeof(v)) == 0 && v[0] != '\0';
    check(win_id, "vproc-name-live", ok);

    snprintf(k, sizeof(k), "sys.proc.%d.state", get_pid());
    memset(v, 0, sizeof(v));
    check(win_id, "vproc-state-live",
          OS1_registry_get(k, v, sizeof(v)) == 0 && v[0] != '\0');

    /* THE property the unification buys: a DEAD pid has no value.  With the old
     * userland-written copy this is exactly what went stale and needed a
     * garbage collector; virtualised, there is nothing stored to leak. */
    snprintf(k, sizeof(k), "sys.proc.%d.name", 999999);
    memset(v, 0, sizeof(v));
    OS1_registry_get(k, v, sizeof(v));
    check(win_id, "vproc-dead-pid-empty", v[0] == '\0');
  }

  /* ============ environment (Phase 17a) ================================= */
  section(win_id, "environment");
  {
    char v[64], k[96];

    /* The seam is a NAMESPACE PATH, not a shared struct: userland never sees
     * the kernel's env block, only strings through sys_registry.  That is the
     * whole reason the syscall dispatcher exists. */
    check(win_id, "env-set", setenv("CAPTEST_A", "one", 1) == 0);
    const char *got = getenv("CAPTEST_A");
    check(win_id, "env-get-roundtrip", got && strcmp(got, "one") == 0);

    check(win_id, "env-overwrite", setenv("CAPTEST_A", "two", 1) == 0 &&
                                       (got = getenv("CAPTEST_A")) &&
                                       strcmp(got, "two") == 0);

    /* overwrite=0 must NOT replace an existing value. */
    setenv("CAPTEST_A", "three", 0);
    got = getenv("CAPTEST_A");
    check(win_id, "env-no-overwrite", got && strcmp(got, "two") == 0);

    unsetenv("CAPTEST_A");
    check(win_id, "env-unset", getenv("CAPTEST_A") == (char *)0);

    /* The MACHINE default layer: HOME is seeded by init as a stored key, and
     * getenv falls through to it with nothing copied into this process. */
    got = getenv("HOME");
    check(win_id, "env-machine-default", got && got[0] == '/');

    /* A process value SHADOWS the default for this process only... */
    setenv("HOME", "/captest", 1);
    got = getenv("HOME");
    check(win_id, "env-process-shadows-default",
          got && strcmp(got, "/captest") == 0);
    /* ...and removing it exposes the default again, unharmed: the two layers
     * are layers, not two copies of one value. */
    unsetenv("HOME");
    got = getenv("HOME");
    check(win_id, "env-default-survives-shadow",
          got && strcmp(got, "/captest") != 0 && got[0] == '/');

    /* S5: a value that does not fit is REFUSED, never silently shortened.  A
     * truncated value is a DIFFERENT value, so the write would "succeed" and
     * the matching read would miss — the failure mode this replaces. */
    {
      char big[512];
      memset(big, 'x', sizeof(big) - 1);
      big[sizeof(big) - 1] = '\0';
      int r = setenv("CAPTEST_BIG", big, 1);
      check(win_id, "env-oversize-refused-not-truncated", r != 0);
      check(win_id, "env-oversize-left-unset",
            getenv("CAPTEST_BIG") == (char *)0);
    }

    /* Same property one layer down, at the registry seam itself — this bit
     * EVERY long registry write, not just the environment. */
    {
      char big[512];
      memset(big, 'y', sizeof(big) - 1);
      big[sizeof(big) - 1] = '\0';
      check(win_id, "registry-oversize-value-refused",
            OS1_registry_set("captest.big", big) != 0);
    }

    /* Writing a COMPUTED field is meaningless, not merely unprivileged: it
     * must be refused rather than stored as a node the computed value would
     * then shadow forever. */
    snprintf(k, sizeof(k), "sys.proc.%d.name", get_pid());
    check(win_id, "vproc-computed-field-readonly",
          OS1_registry_set(k, "hijacked") != 0);
    memset(v, 0, sizeof(v));
    check(win_id, "vproc-name-still-true",
          OS1_registry_get(k, v, sizeof(v)) == 0 && strcmp(v, "hijacked") != 0);

    /* Another process's environment is not ours to write. */
    check(win_id, "env-other-process-denied",
          OS1_registry_set("sys.proc.1.env.CAPTEST", "x") != 0);
  }

  /* ============ exit status survives reaping (Phase 9b) ================= */
  section(win_id, "exit status retention");
  {
    /* A child that fails FAST is reaped before its parent polls, and the status
     * used to die with the corpse — so a FAILING command reported SUCCESS.
     * Timing-dependent, hence it looked like flakiness.  Spawn a child that
     * exits non-zero immediately and only THEN wait, which is the losing order. */
    char *av[3];
    av[0] = (char *)"/bin/lua";
    av[1] = (char *)"-e";
    av[2] = (char *)"os.exit(3)";
    int pid = (int)spawn_args("/bin/lua", 3, av);
    check(win_id, "status-child-spawned", pid > 0);
    if (pid > 0) {
      /* Give it time to die AND be collected, so we are testing the retained
       * status rather than racing the corpse. */
      for (int i = 0; i < 60; i++)
        OS1_sleep(15);
      int st = 0;
      int w = waitpid(pid, &st, 0);
      check(win_id, "status-reported-after-reap", w == pid);
      printf("[captest] status raw=0x%x exited=%d code=%d signalled=%d\n", st,
             WIFEXITED(st) ? 1 : 0, WEXITSTATUS(st), WIFSIGNALED(st) ? 1 : 0);
      check(win_id, "status-nonzero-survives", w == pid && WIFEXITED(st) &&
                                                   WEXITSTATUS(st) == 3);
      /* Collected once, like a real wait(): a second call must NOT re-report. */
      int st2 = 0;
      check(win_id, "status-consumed-once", waitpid(pid, &st2, 0) != pid);
    }
  }

  /* ===================== execution service (Phase 9) ==================== */
  section(win_id, "exec service");
  {
    /* Connect — never spawn.  A client that spawned the service would get an
     * UNPRIVILEGED one (monotonic creator clamp), and the daemon's defining
     * powers (SETOWNER, taking client fds) are privileged-only: it would answer
     * requests but silently fail to delegate.  The service is started by init. */
    int svc = OS1_port_open(OS1NX_PORT_EXEC);
    check(win_id, "exec-service-published", svc >= 0);

    if (svc >= 0) {
      int reqp[2] = {-1, -1}, repp[2] = {-1, -1};
      ok = pipe(reqp) == 0 && pipe(repp) == 0;
      check(win_id, "exec-channels", ok);

      if (ok) {
        /* Build a variable-size request: header + [redir][cwd\0][argv\0...]. */
        struct execsvc_spawn_hdr hdr;
        char body[256];
        unsigned int off = 0;
        const char *a0 = "/bin/lua", *a1 = "-e", *a2 = "os.exit(0)";
        memset(&hdr, 0, sizeof(hdr));
        hdr.version = EXECSVC_VERSION;
        hdr.argc = 3;
        hdr.nredir = 0;
        body[off++] = '\0'; /* cwd: empty */
        memcpy(body + off, a0, strlen(a0) + 1); off += strlen(a0) + 1;
        memcpy(body + off, a1, strlen(a1) + 1); off += strlen(a1) + 1;
        memcpy(body + off, a2, strlen(a2) + 1); off += strlen(a2) + 1;
        hdr.body_len = off;

        struct ipc_message m;
        memset(&m, 0, sizeof(m));
        m.type = EXECSVC_REQ_SPAWN;

        /* Transfer the two channels THROUGH the port.  A handle index only means
         * something in one process's table, so the rights must travel with the
         * message; the kernel installs them in the service and rewrites the
         * leading payload slots with the indices IT will see.  Doing this by
         * cap_grant instead would need the service's PID — dragging pid
         * addressing back into the one path that exists to remove it. */
        int give[2] = {reqp[0], repp[1]};
        long sent = OS1_port_send_caps(svc, &m, give, 2);
        check(win_id, "exec-caps-transferred", sent == (long)sizeof(m));

        if (sent == (long)sizeof(m)) {
          /* The service reads the request body from the pipe we handed it. */
          write(reqp[1], (const char *)&hdr, sizeof(hdr));
          write(reqp[1], body, off);
          close(reqp[1]); /* EOF, so a short body cannot hang the service */

          struct execsvc_spawn_rep rep;
          memset(&rep, 0, sizeof(rep));
          long got = read(repp[0], (char *)&rep, sizeof(rep));
          check(win_id, "exec-reply-received", got == (long)sizeof(rep));
          check(win_id, "exec-spawned-pid", got == (long)sizeof(rep) &&
                                                 rep.pid > 0);
          /* THE Q3 PROPERTY: the job must belong to US, not to the service that
           * mechanically spawned it — otherwise a shell migrated onto the daemon
           * would silently lose kill/stop/cont over its own jobs. */
          check(win_id, "exec-owner-is-requester",
                got == (long)sizeof(rep) && rep.owner_pid == get_pid());
          if (got == (long)sizeof(rep) && rep.pid > 0)
            kill_process(rep.pid);
        } else {
          close(reqp[1]);
        }

        close(reqp[0]);
        close(repp[0]);
        close(repp[1]);
      }
      OS1low_handle_close(svc);
    }
  }

  if (hd >= 0) OS1low_handle_close(hd);
  if (h2 >= 0) OS1low_handle_close(h2);
  if (ph >= 0) OS1low_handle_close(ph);
  if (hng >= 0) OS1low_handle_close(hng);

  printf_win(win_id, "done: %d/%d passed, %d failure(s)\n", checks - failures,
             checks, failures);
  printf("[captest] done: %d/%d passed, %d failure(s)\n", checks - failures,
         checks, failures);

  for (int i = 0; i < 150; i++)
    yield();
  return failures ? 1 : 0;
}
