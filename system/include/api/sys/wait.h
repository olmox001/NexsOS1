#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <posix_types.h>

/*
 * Minimal POSIX <sys/wait.h> for the OS1 userspace libc.
 *
 * waitpid() wraps the PROCESS-capability wait and now delivers the REAL exit
 * code (PLAN-2026-07-17 Phase 2): the kernel stores exit_code on sys_exit and
 * the libc encodes it into the standard status word.  OS1 has no signal model,
 * but a process that died by KILL/fault (never called exit) is reported as
 * WIFSIGNALED with a stand-in SIGKILL(9), vs WIFEXITED for a clean exit.  Impl
 * in the libc layer, NOT an OS1 syscall.  Note: the POSIX wait(int*) is
 * intentionally NOT declared here — it would collide with the OS1 base
 * wait(int pid); ported code uses waitpid().
 */

#define WNOHANG   1
#define WUNTRACED 2

/* Standard status encoding: the exit code sits in bits 8..15, the low byte is
 * the terminating signal (always 0 here — no signals). */
#define WIFEXITED(status)   (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)
#define WIFSIGNALED(status) (((status) & 0x7f) != 0)
#define WTERMSIG(status)    ((status) & 0x7f)

pid_t waitpid(pid_t pid, int *status, int options);

#endif /* _SYS_WAIT_H */
