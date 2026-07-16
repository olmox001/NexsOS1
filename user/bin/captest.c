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
#include <os1.h>
#include <string.h>

static int failures = 0;

static void check(int win_id, const char *name, int ok) {
  printf_win(win_id, "%s: %s\n", name, ok ? "PASS" : "FAIL");
  printf("[captest] %s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok)
    failures++;
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

  if (hd >= 0) OS1low_handle_close(hd);
  if (h2 >= 0) OS1low_handle_close(h2);
  if (ph >= 0) OS1low_handle_close(ph);
  if (hng >= 0) OS1low_handle_close(hng);

  printf_win(win_id, "done: %d failure(s)\n", failures);
  printf("[captest] done: %d failure(s)\n", failures);

  for (int i = 0; i < 150; i++)
    yield();
  return failures ? 1 : 0;
}
