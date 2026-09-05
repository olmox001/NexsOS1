/*
 * user/bin/coreutils_test.c
 * Verification test harness for GNU Coreutils on NexsOS1.
 *
 * This exercises the real userland execution path:
 *  - user app -> libc system() -> /sys/bin/nxshell -c <cmd> -> spawn()
 *  - kernel process creation with the OS1 ABI
 *
 * Output conforms to the tools/nxrun.sh test harness protocol:
 *   [coreutilstest] PASS <case>
 *   [coreutilstest] done: N/N passed, 0 failure(s)
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

static int run_case(const char *prog, const struct coreutils_case *tc) {
    int rc = system(tc->cmd);
    int ok = (tc->expect_nonzero) ? (rc != 0) : (rc == 0);

    printf("[%s] %s %s -> rc=%d\n",
           prog,
           ok ? "PASS" : "FAIL",
           tc->name,
           rc);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *prog = "coreutilstest";
    if (argc > 0 && argv[0] && strstr(argv[0], "coreutils_test")) {
        prog = "coreutils_test";
    }

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
        { "sync", "sync", 0 },
        { "whoami", "whoami", 0 },
        { "seq", "seq 1 3", 0 },
        { "basename2", "basename /etc/init.cfg", 0 },
        { "dirname2", "dirname /etc/init.cfg", 0 },
        { "readlink", "readlink /bin/echo", 0 },
        { "realpath", "realpath /bin/echo", 0 },
        { "tty", "tty", 0 },
        { "expr", "expr 10 + 20", 0 },
        { "factor", "factor 42", 0 },
        { "cut", "cut -b 1-4 /etc/init.cfg", 0 },
        { "fold", "fold -w 20 /etc/init.cfg", 0 },
        { "fmt", "fmt -w 40 /etc/init.cfg", 0 },
        { "nl", "nl /etc/init.cfg", 0 },
        { "nproc", "nproc", 0 },
        { "paste", "paste /etc/init.cfg /etc/init.cfg", 0 },
        { "shuf", "shuf -i 1-5 -n 2", 0 },
        { "uniq", "uniq /etc/init.cfg", 0 },
        { "uptime", "uptime", 0 },
        { "users", "users", 0 },
        { "hostid", "hostid", 0 },
        { "who", "who", 0 },
        { "logname", "logname", 0 },
        { "pathchk", "pathchk /bin/echo", 0 },
        { "nice", "nice echo nice-ok", 0 },
        { "nohup", "nohup echo nohup-ok", 0 },
        { "truncate", "truncate -s 16 /tmp/nxctestdir/trunc.txt", 0 },
        { "chmod", "chmod 644 /tmp/nxctestdir/file.txt", 0 },
        { "chown", "chown 0 /tmp/nxctestdir/file.txt", 0 },
        { "chgrp", "chgrp 0 /tmp/nxctestdir/file.txt", 0 },
        { "link", "link /tmp/nxctestdir/file.txt /tmp/nxctestdir/file_link.txt", 0 },
        { "unlink_link", "unlink /tmp/nxctestdir/file_link.txt", 0 },
        { "unlink_trunc", "unlink /tmp/nxctestdir/trunc.txt", 0 },
        { "unlink_file", "unlink /tmp/nxctestdir/file.txt", 0 },
        { "rmdir", "rmdir /tmp/nxctestdir", 0 }
    };

    int failed = 0;
    const size_t ntests = sizeof(tests) / sizeof(tests[0]);

    printf("=== NexsOS1 Coreutils Runtime Test Suite ===\n");
    for (size_t i = 0; i < ntests; i++) {
        failed += run_case(prog, &tests[i]);
    }

    size_t passed = (failed <= (int)ntests) ? (ntests - (size_t)failed) : 0;
    printf("[%s] done: %zu/%zu passed, %d failure(s)\n", prog, passed, ntests, failed);
    if (strcmp(prog, "coreutilstest") != 0) {
        printf("[coreutilstest] done: %zu/%zu passed, %d failure(s)\n", passed, ntests, failed);
    }

    return failed == 0 ? 0 : 1;
}
