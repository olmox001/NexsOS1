#ifndef _TIME_H
#define _TIME_H

/*
 * include/api/time.h
 * POSIX-like time layer built ON TOP of the os1.h base primitives
 * (os1_mono_ns / os1_cpu_ns). os1.h is NEXS's proprietary base API; this header
 * is the start of the real POSIX/libc surface that sits above it. See
 * docs/TIMER-MODEL.md §4.
 */

#include <os1.h>

typedef long time_t;

struct timespec {
  time_t tv_sec;  /* seconds */
  long   tv_nsec; /* nanoseconds [0, 999999999] */
};

/* Clock ids. Mapped onto the two kernel clocks (monotonic / process CPU).
 * REALTIME currently aliases MONOTONIC — there is no wall-clock RTC yet. */
typedef int clockid_t;
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          0
#define CLOCK_PROCESS_CPUTIME_ID 1

/* clock_gettime: fill *ts with the current value of clk. Returns 0 on success,
 * -1 if ts is NULL. Backed by os1_mono_ns() / os1_cpu_ns(). */
int clock_gettime(clockid_t clk, struct timespec *ts);

/* nanosleep: POSIX blocking sleep for req->tv_sec + req->tv_nsec, on top of the
 * SYS_NANOSLEEP primitive (the caller is descheduled, no busy-wait). Sleeps are
 * not interruptible here, so they always run to completion: *rem (if non-NULL)
 * is zeroed and 0 is returned. -1 on a NULL/invalid req. The kernel rounds the
 * deadline up to the tick (~10 ms at HZ=100) but tracks it in real wall time. */
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif /* _TIME_H */
