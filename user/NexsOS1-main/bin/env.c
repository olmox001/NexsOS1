/*
 * user/bin/env.c
 * Standard 'env' command utility for NexsOS1.
 */
#include <os1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int print_environment(void) {
    char buf[4096];
    int n = OS1_env_enum(buf, sizeof(buf));
    if (n < 0)
        return -1;

    char *save = NULL;
    for (char *k = strtok_r(buf, "\n", &save); k; k = strtok_r(NULL, "\n", &save)) {
        const char *value = getenv(k);
        printf("%s=%s\n", k, value ? value : "");
    }
    return 0;
}

static int spawn_with_stdio_passthrough(const char *path, int argc,
                                       char *const argv[]) {
    struct spawn_redir redir[3];
    int nredir = 0;

    for (int fd = 0; fd < 3; fd++) {
        if (OS1low_cap_query(fd) < 0)
            continue;
        redir[nredir].child_fd = fd;
        redir[nredir].parent_fd = fd;
        redir[nredir].source_pid = 0;
        nredir++;
    }

    if (nredir == 0)
        return _sys_spawn(path, argc, argv, 0);

    return _sys_spawn_redir(path, argc, argv, 0, redir, nredir);
}

int main(int argc, char *argv[]) {
    int i = 1;
    int clear_env = 0;

    while (i < argc) {
        if (strcmp(argv[i], "-i") == 0) {
            clear_env = 1;
            i++;
            continue;
        }

        if (strcmp(argv[i], "-u") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "env: option requires an argument -- 'u'\n");
                return 125;
            }
            unsetenv(argv[++i]);
            i++;
            continue;
        }

        if (strchr(argv[i], '=')) {
            char *eq = strchr(argv[i], '=');
            *eq = '\0';
            if (setenv(argv[i], eq + 1, 1) != 0) {
                fprintf(stderr, "env: unable to set %s\n", argv[i]);
                return 125;
            }
            i++;
            continue;
        }

        break;
    }

    if (clear_env) {
        clearenv();
        /* Re-apply assignments after the reset so NAME=value pairs still work. */
        int j = 1;
        while (j < i) {
            if (strcmp(argv[j], "-i") == 0 || strcmp(argv[j], "-u") == 0) {
                j++;
                if (strcmp(argv[j - 1], "-u") == 0 && j < argc)
                    j++;
                continue;
            }
            if (strchr(argv[j], '=')) {
                char *eq = strchr(argv[j], '=');
                *eq = '\0';
                setenv(argv[j], eq + 1, 1);
            }
            j++;
        }
    }

    if (i >= argc) {
        return print_environment() == 0 ? 0 : 125;
    }

    /* Command specified: spawn it */
    const char *cmd = argv[i];
    char path[256];

    int pid = -1;
    int sub_argc = argc - i;
    char *const *sub_argv = &argv[i];

    /* Try exact path first */
    pid = spawn_with_stdio_passthrough(cmd, sub_argc, sub_argv);

    /* Try /bin/cmd if relative */
    if (pid < 0 && cmd[0] != '/') {
        snprintf(path, sizeof(path), "/bin/%s", cmd);
        pid = spawn_with_stdio_passthrough(path, sub_argc, sub_argv);
    }

    /* Try /sys/bin/cmd if relative */
    if (pid < 0 && cmd[0] != '/') {
        snprintf(path, sizeof(path), "/sys/bin/%s", cmd);
        pid = spawn_with_stdio_passthrough(path, sub_argc, sub_argv);
    }

    if (pid < 0) {
        fprintf(stderr, "env: %s: No such file or directory\n", cmd);
        return 127;
    }

    int code = 0;
    OS1low_process_wait_status(pid, &code);
    return code;
}
