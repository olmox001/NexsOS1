#ifndef _UNISTD_H
#define _UNISTD_H

#include <os1.h>

/* POSIX-like sleeps, backed by the real kernel timer (SYS_NANOSLEEP): the
 * caller is descheduled and woken by its core's tick — no busy-wait. The
 * kernel primitive is nanosleep-granular (rounded up to the tick, ~10 ms at
 * HZ=100). sleep(ms) lives in <os1.h>. */
int usleep(unsigned int usec);

#endif
