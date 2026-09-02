/*
 * user/bin/coreutils_test.c
 * Minimal smoke-test harness for the NexsOS1 GNU Coreutils port.
 *
 * This is intentionally a runtime validation program, not a host emulator or
 * Linux-compat layer test. It exercises the real userland execution path:
 *  - user app -> libc system() -> /sys/bin/nxshell -c <cmd> -> spawn()
 *  - kernel process creation with the OS1 ABI
 */

#include <os1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct coreutils_case {
    const char *name;
    const char *cmd;
    int expect_nonzero;
};

static int run_case(const struct coreutils_case *tc) {
    int rc = system(tc->cmd);
    int ok = (tc->expect_nonzero) ? (rc != 0) : (rc == 0);

    printf("[%s] %s -> rc=%d %s\n",
           ok ? "PASS" : "FAIL",
           tc->name,
           rc,
           tc->cmd);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const struct coreutils_case tests[] = {
        { "echo", "echo coreutils-ok", 0 },
        { "uname", "uname -s", 0 },
        { "pwd", "pwd", 0 },
        { "basename", "basename /bin/echo", 0 },
        { "dirname", "dirname /bin/echo", 0 },
        { "printenv", "printenv PATH", 0 },
        { "true", "true", 0 },
        { "false", "false", 1 },
        { "sleep", "sleep 0", 0 },
        { "mkdir", "mkdir /tmp/nxctestdir", 0 },
        { "touch", "touch /tmp/nxctestdir/file.txt", 0 },
        { "cat", "cat /tmp/nxctestdir/file.txt", 0 },
        { "wc", "wc -c /etc/init.cfg", 0 },
        { "head", "head -n 2 /etc/init.cfg", 0 },
        { "tail", "tail -n 1 /etc/init.cfg", 0 },
        { "unlink", "unlink /tmp/nxctestdir/file.txt", 0 },
        { "rmdir", "rmdir /tmp/nxctestdir", 0 },
        { "sync", "sync", 0 },
        { "whoami", "whoami", 0 },
        { "seq", "seq 1 3", 0 },
        { "basename2", "basename /etc/init.cfg", 0 },
        { "dirname2", "dirname /etc/init.cfg", 0 },
        { "readlink", "readlink /bin/echo", 0 },
        { "realpath", "realpath /bin/echo", 0 },
        { "tty", "tty", 0 }
    };

    int failed = 0;
    const size_t ntests = sizeof(tests) / sizeof(tests[0]);

    printf("=== NexsOS1 Coreutils Runtime Smoke Test ===\n");
    for (size_t i = 0; i < ntests; i++) {
        failed += run_case(&tests[i]);
    }

    printf("summary: %zu test(s), %d failure(s)\n", ntests, failed);
    return failed == 0 ? 0 : 1;
}
