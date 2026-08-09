#ifndef _TIME_H
#define _TIME_H

/*
 * include/api/time.h
 * POSIX-like time layer built ON TOP of the os1.h base primitives
 * (os1_mono_ns / os1_cpu_ns). os1.h is NEXS's proprietary base API; this header
 * is the start of the real POSIX/libc surface that sits above it. See
 * docs/TIMER-MODEL.md §4.
 */

#include <stddef.h>

typedef long time_t;

typedef int clockid_t;

struct timespec {
  time_t tv_sec; /* seconds */
  long tv_nsec;  /* nanoseconds [0, 999999999] */
};

struct tm {
  int tm_sec;   /* seconds after the minute [0, 60] */
  int tm_min;   /* minutes after the hour [0, 59] */
  int tm_hour;  /* hours since midnight [0, 23] */
  int tm_mday;  /* day of the month [1, 31] */
  int tm_mon;   /* months since January [0, 11] */
  int tm_year;  /* years since 1900 */
  int tm_wday;  /* days since Sunday [0, 6] */
  int tm_yday;  /* days since January 1 [0, 365] */
  int tm_isdst; /* Daylight Saving Time flag */
};

/* Clock ids. Mapped onto the two kernel clocks (monotonic / process CPU).
 * REALTIME currently aliases MONOTONIC — there is no wall-clock RTC yet. */
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 0
#define CLOCK_PROCESS_CPUTIME_ID 1

/* clock_gettime: fill *ts with the current value of clk. Returns 0 on success,
 * -1 if ts is NULL. Backed by os1_mono_ns() / os1_cpu_ns(). */
int clock_gettime(clockid_t clk, struct timespec *ts);

/* nanosleep: POSIX blocking sleep for req->tv_sec + req->tv_nsec, on top of the
 * SYS_NANOSLEEP primitive (the caller is descheduled, no busy-wait). Sleeps are
 * not interruptible here, so they always run to completion: *rem (if non-NULL)
 * is zeroed and 0 is returned. -1 on a NULL/invalid req. The kernel rounds the
 * deadline up to the tick (~10 ms at HZ=100) but tracks it in real wall time.
 */
int nanosleep(const struct timespec *req, struct timespec *rem);

/* time: return seconds since epoch, or set *t if non-NULL. */
time_t time(time_t *t);

/* mktime: convert struct tm to time_t. */
time_t mktime(struct tm *tm);

/* difftime: difference between two time_t values in seconds. */
double difftime(time_t time1, time_t time0);

/* localtime/gmtime: convert time_t to struct tm. */
struct tm *localtime(const time_t *timep);
struct tm *gmtime(const time_t *timep);

/* strftime: format time into string. */
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);

/* clock: return processor time used by the program (in CLOCKS_PER_SEC units).
 */
typedef long clock_t;
#ifndef CLOCKS_PER_SEC
#define CLOCKS_PER_SEC 1000000000L
#endif
clock_t clock(void);

#endif /* _TIME_H */