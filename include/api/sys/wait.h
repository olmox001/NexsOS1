#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include "../posix_types.h"

/*
 * Minimal POSIX <sys/wait.h> for the OS1 userspace libc.
 *
 * waitpid() wraps the OS1 base wait() (os1.h).  OS1 has no signal model and the
 * exit-status plumbing is not wired yet (SCHED-06), so the status word always
 * decodes as a clean exit (code 0).  Implemented in the libc layer, NOT an OS1
 * syscall.  Note: the POSIX wait(int*) is intentionally NOT declared here — it
 * would collide with the OS1 base wait(int pid); ported code uses waitpid().
 */

#define WNOHANG   1
#define WUNTRACED 2

#define WIFEXITED(status)   (1)
#define WEXITSTATUS(status) ((status) & 0xff)
#define WIFSIGNALED(status) (0)
#define WTERMSIG(status)    (0)

pid_t waitpid(pid_t pid, int *status, int options);

#endif /* _SYS_WAIT_H */
