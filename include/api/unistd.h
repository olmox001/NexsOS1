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

#endif
