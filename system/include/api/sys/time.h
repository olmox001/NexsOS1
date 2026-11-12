/*
 * include/api/sys/time.h
 * POSIX timeval and gettimeofday for NexsOS1.
 */

#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct timeval {
    time_t tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *tv, struct timezone *tz);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TIME_H */
