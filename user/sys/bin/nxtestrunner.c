/*
 * NXTEST: real, fail-closed userland regression orchestrator.
 *
 * PID1 starts this ONLY through a separate nxshell -c in mdebug mode.
 * Every case is an actual ELF with actual kernel process/exit semantics.
 * Timeouts, signal exits, missing status and failed launch are all failures.
 * Interactive/destructive applications remain separate catalog lanes.
 */
#include <os1.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/wait.h>

#define NXTEST_POLL_MS 20U
#define NXTEST_POLL_LIMIT 6000U
#define NXTEST_REAP_LIMIT 20U

struct nx_test_case {
  const char *id;
  const char *path;
  const char *arg;
  uint32_t max_polls;
};

/* Canonical case IDs are stable across ARM64/AMD64; no renaming of the real
 * binaries, scripts, and child helpers that existing tests depend on. */
static const struct nx_test_case cases[] = {
    {"00nxtest_captest", "/bin/captest", NULL, 3000U},
    {"01nxtest_libctest", "/bin/libctest", NULL, 2000U},
    {"02nxtest_buffertest", "/bin/buffertest", NULL, 2000U},
    {"03nxtest_gnulibtest", "/bin/gnulibtest", NULL, 2000U},
    {"04nxtest_coreutils", "/bin/coreutilstest", NULL, 6000U},
    {"05nxtest_luatest", "/bin/luatest", NULL, 2000U},
    {"06nxtest_fdtest", "/bin/fdtest", NULL, 2000U},
    {"07nxtest_writetest", "/bin/writetest", NULL, 2000U},
    {"08nxtest_capreg", "/bin/capreg", NULL, 2000U},
    {"09nxtest_capipc", "/bin/capipc", NULL, 2000U},
    {"10nxtest_capkill", "/bin/capkill", NULL, 2000U},
    {"11nxtest_nxpax", "/sys/bin/nxpax", "--probe", 1000U},
    {"12nxtest_timerqueue", "/sys/bin/nxtimerdiag", "--queue-stress", 4000U},
    {"13nxtest_notify", "/bin/notifytest", NULL, 2000U},
    {"14nxtest_port", "/bin/porttest", NULL, 2000U},
    {"15nxtest_gfxbuffer", "/bin/gfxbuffertest", NULL, 2000U},
    {"16nxtest_simd", "/bin/simdtest", NULL, 2000U},
    {"17nxtest_gfxalpha", "/bin/gfxalphatest", NULL, 2000U},
};

static int nx_test_report(const struct nx_test_case *test, const char *state,
                          int status) {
  /* stdout already supplies the headless console fallback. Sending the same
   * record to window zero as well duplicated it in the diagnostic capture. */
  int n = printf("[NXTEST][%s] %s status=%d\n", state, test->id, status);
  return n < 0 ? -1 : 0;
}

static void nx_test_abort_child(int pid) {
  int killed = kill_process(pid);
  if (killed < 0)
    print("[NXTEST][ERROR] failed to terminate timed-out child\n");
  for (uint32_t i = 0U; i < NXTEST_REAP_LIMIT; ++i) {
    int status = 0;
    int ret = waitpid(pid, &status, WNOHANG);
    if (ret != 0)
      return;
    OS1_sleep((int)NXTEST_POLL_MS);
  }
  print("[NXTEST][ERROR] timed-out child not reaped\n");
}

static int nx_test_wait(const struct nx_test_case *test, int pid) {
  for (uint32_t step = 0U; step < NXTEST_POLL_LIMIT; ++step) {
    int status = 0;
    int result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      int passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
      if (nx_test_report(test, passed ? "PASS" : "FAIL", status) != 0)
        return -1;
      return passed ? 0 : -1;
    }
    if (result < 0) {
      if (nx_test_report(test, "FAIL-WAIT", result) != 0)
      print("[NXTEST][ERROR] diagnostic report failed\n");
      return -1;
    }
    if (step >= test->max_polls) {
      nx_test_abort_child(pid);
      if (nx_test_report(test, "FAIL-TIMEOUT", (int)step) != 0)
      print("[NXTEST][ERROR] diagnostic report failed\n");
      return -1;
    }
    OS1_sleep((int)NXTEST_POLL_MS);
  }
  nx_test_abort_child(pid);
  if (nx_test_report(test, "FAIL-TIMEOUT", (int)NXTEST_POLL_LIMIT) != 0)
      print("[NXTEST][ERROR] diagnostic report failed\n");
  return -1;
}

static int nx_test_run(const struct nx_test_case *test) {
  int pid;
  if (test->arg != NULL) {
    char *argv[2] = {(char *)test->path, (char *)test->arg};
    pid = spawn_args(test->path, 2, argv);
  } else {
    pid = spawn(test->path);
  }
  if (pid <= 0) {
    if (nx_test_report(test, "FAIL-SPAWN", pid) != 0)
      print("[NXTEST][ERROR] diagnostic report failed\n");
    return -1;
  }
  return nx_test_wait(test, pid);
}

int main(void) {
  const uint32_t count = (uint32_t)(sizeof(cases) / sizeof(cases[0]));
  uint32_t passed = 0U;
  uint32_t failed = 0U;
  if (printf("[NXTEST] real userland suite started in isolated mdebug shell\n") < 0)
    return 1;
  for (uint32_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    int result = nx_test_run(&cases[i]);
    if (result == 0)
      ++passed;
    else
      ++failed;
  }
  if (printf("[NXTEST] done: %u/%u passed, %u failure(s)\n",
             passed, count, failed) < 0)
    return 1;
  return failed == 0U ? 0 : 1;
}
