#ifndef _UNISTD_H
#define _UNISTD_H

#include <os1.h>

/* POSIX-like sleeps, backed by the real kernel timer (SYS_NANOSLEEP): the
 * caller is descheduled and woken by its core's tick — no busy-wait. The
 * kernel primitive is nanosleep-granular (rounded up to the tick, ~10 ms at
 * HZ=100).
 *
 * usleep(usec): microseconds, POSIX-compatible.
 * For nanosecond / timespec sleeps and the monotonic clock use <time.h>
 *   (nanosleep(), clock_gettime()).
 * NOTE: os1.h's OS1_sleep(N) is the proprietary BASE API and takes MILLISECONDS,
 *   which deliberately differs from POSIX sleep(unsigned seconds). A real POSIX
 *   sleep() belongs to the libc layer above os1.h and is intentionally not
 *   declared here to avoid silently changing the unit of existing callers. */
int usleep(unsigned int usec);

/* Standard file descriptors. */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Thin POSIX shims over the OS1 base API (implemented in user/sys/lib/lib.c,
 * epic #120 onion-userland libc — no new OS1 syscalls).
 *   isatty: true for the std stream fds (0/1/2).
 *   getpid: alias of the OS1 get_pid().
 *   pipe:   OS1 has no anonymous pipes — always fails (callers fall back).
 *   unlink: no VFS delete syscall yet — accepted no-op for temp-file cleanup. */
int isatty(int fd);
int getpid(void);
int pipe(int pipefd[2]);
int unlink(const char *pathname);

#endif
