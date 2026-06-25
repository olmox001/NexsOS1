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

  /* 9. delegation gated: grant without OS1_RIGHT_TRANSFER -> -EPERM */
  int hng = (int)OS1low_handle_create(OS1_NS_FS, path, OS1_RIGHT_READ,
                                      OBJ_TYPE_FILE);
  ok = hng >= 0 && OS1low_cap_grant(get_pid(), hng, OS1_RIGHT_READ) == -EPERM;
  check(win_id, "grant-needs-transfer", ok);

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
