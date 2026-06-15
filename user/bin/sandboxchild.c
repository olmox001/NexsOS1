/*
 * user/bin/sandboxchild.c
 * Guest worker for the sandbox test (USR-SEC-03 #79).
 *
 * Spawned by /bin/sandboxtest at PLVL_GUEST (capabilities = CAP_WINDOW only).
 * It attempts each gated syscall and prints the outcome to the serial console
 * (printf always reaches the UART) tagged "[sandbox]", so the headless test
 * harness can grep the results.  Expected:
 *   - spawn()                  -> denied (no CAP_SPAWN)
 *   - open(.., O_WRONLY)       -> denied (no CAP_FS_WRITE)
 *   - send() to a non-relative -> denied (no CAP_IPC_ANY)
 *   - create_window()          -> allowed (guest HAS CAP_WINDOW)
 */
#include <fcntl.h>
#include <os1.h>

static int failures = 0;

/* expect_neg: the op must be refused (negative errno). */
static void expect_neg(const char *name, long rc) {
  int ok = (rc < 0);
  printf("[sandbox] %s rc=%ld -> %s\n", name, rc, ok ? "PASS" : "FAIL");
  if (!ok)
    failures++;
}

/* expect_ok: the op must succeed (>= 0). */
static void expect_ok(const char *name, long rc) {
  int ok = (rc >= 0);
  printf("[sandbox] %s rc=%ld -> %s\n", name, rc, ok ? "PASS" : "FAIL");
  if (!ok)
    failures++;
}

int main(void) {
  printf("[sandbox] child PID %d running as guest\n", get_pid());

  /* CAP_SPAWN: denied. */
  expect_neg("spawn", spawn("/bin/counter"));

  /* CAP_FS_WRITE: opening a writable config file for write is denied. */
  expect_neg("open-wronly", open("/etc/init.cfg", O_WRONLY));

  /* CAP_IPC_ANY: messaging a non-relative (init, PID 1 — an ancestor above
   * our parent, so neither parent nor descendant) is denied. */
  struct ipc_message m;
  m.type = 1;
  m.data1 = 0;
  m.data2 = 0;
  m.payload[0] = '\0';
  expect_neg("send-nonrelative", send(1, &m));

  /* CAP_WINDOW: guests may draw — this must succeed. */
  expect_ok("create-window", create_window(60, 60, 200, 80, "Guest"));

  printf("[sandbox] child done: %d failure(s)\n", failures);

  while (1)
    yield();
  return 0;
}
