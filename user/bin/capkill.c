/*
 * user/bin/capkill.c
 * Capability-based process lifecycle test (ASTRA §6.5 — seL4: no kill-by-PID
 * without a capability).  Parent half; target = /bin/capkill_child.
 *
 *   1. spawn a guest child that loops forever;
 *   2. a WAIT-only PROCESS handle CANNOT kill it (object_ctl KILL -> -EPERM);
 *   3. the child survives the denied kill;
 *   4. a handle that also holds OS1_RIGHT_DESTROY kills it via the capability
 *      (OS1_object_ctl, not by PID);
 *   5. the child is then gone.
 * Tagged "[capkill]" on the serial console.
 */
#include <os1.h>
#include <string.h>

static int failures = 0;

static void check(int win, const char *name, int ok) {
  printf_win(win, "%s: %s\n", name, ok ? "PASS" : "FAIL");
  printf("[capkill] %s: %s\n", name, ok ? "PASS" : "FAIL");
  if (!ok)
    failures++;
}

int main(void) {
  int win = create_window(150, 150, 460, 280, "Cap Kill");
  printf("[capkill] parent PID %d\n", get_pid());

  long child = spawn_level("/bin/capkill_child", PLVL_GUEST);
  check(win, "spawn-child", child > 0);
  for (int i = 0; i < 50; i++) /* let the child start looping */
    yield();

  char pidbuf[16];
  sprintf(pidbuf, "%d", (int)child);

  /* 2. a WAIT-only capability cannot kill (no DESTROY right) */
  int hno = (int)OS1low_handle_create(OS1_NS_PROC, pidbuf, OS1_RIGHT_WAIT,
                                      OBJ_TYPE_PROCESS);
  check(win, "deny-kill-no-destroy",
        hno >= 0 && OS1_object_ctl(hno, OBJ_CTL_KILL, 0) == -EPERM);

  /* 3. the child survives a denied kill */
  check(win, "child-survives-denied-kill", wait((int)child) == -1);

  /* 4. a DESTROY capability kills it — via the handle, never by PID */
  int hk = (int)OS1low_handle_create(
      OS1_NS_PROC, pidbuf, OS1_RIGHT_WAIT | OS1_RIGHT_DESTROY, OBJ_TYPE_PROCESS);
  check(win, "kill-via-capability",
        hk >= 0 && OS1_object_ctl(hk, OBJ_CTL_KILL, 0) == 0);

  /* 5. the child is now gone (reaped: wait returns its pid, or -2 = gone) */
  int gone = 0;
  for (int i = 0; i < 2000 && !gone; i++) {
    int w = wait((int)child);
    if (w == (int)child || w == -2)
      gone = 1;
    else
      yield();
  }
  check(win, "child-terminated", gone);

  if (hno >= 0)
    OS1low_handle_close(hno);
  if (hk >= 0)
    OS1low_handle_close(hk);

  printf("[capkill] done: %d failure(s)\n", failures);
  printf_win(win, "done: %d failure(s)\n", failures);
  for (int i = 0; i < 150; i++)
    yield();
  return failures ? 1 : 0;
}
