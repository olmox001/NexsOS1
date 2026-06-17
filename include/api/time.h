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

#endif /* _TIME_H */
