#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H

#include <sys/time.h>
#include <time.h>

/* POSIX resource limits (stub implementation for NexsOS1).
 * NexsOS1 does not enforce traditional process resource limits; these are
 * minimal definitions for POSIX compatibility. */

#define RLIMIT_CPU      0       /* CPU time limit in seconds */
#define RLIMIT_FSIZE    1       /* File size limit */
#define RLIMIT_DATA     2       /* Data segment limit */
#define RLIMIT_STACK    3       /* Stack size limit */
#define RLIMIT_CORE     4       /* Core dump file size limit */
#define RLIMIT_NOFILE   5       /* File descriptor limit */
#define RLIMIT_NPROC    6       /* Process count limit */
#define RLIMIT_MEMLOCK  7       /* Locked memory limit */
#define RLIMIT_AS       8       /* Address space limit */

typedef unsigned long rlim_t;

struct rlimit {
    rlim_t rlim_cur;            /* Current (soft) limit */
    rlim_t rlim_max;            /* Maximum (hard) limit */
};

struct rusage {
    struct timeval ru_utime;    /* User CPU time used */
    struct timeval ru_stime;    /* System CPU time used */
    long   ru_maxrss;           /* Maximum resident set size */
    long   ru_ixrss;            /* Shared memory size */
    long   ru_idrss;            /* Unshared memory size */
    long   ru_isrss;            /* Unshared stack size */
    long   ru_minflt;           /* Minor page faults */
    long   ru_majflt;           /* Major page faults */
    long   ru_nswap;            /* Swaps */
    long   ru_inblock;          /* Block input operations */
    long   ru_oublock;          /* Block output operations */
    long   ru_msgsnd;           /* Messages sent */
    long   ru_msgrcv;           /* Messages received */
    long   ru_nsignals;         /* Signals received */
    long   ru_nvcsw;            /* Voluntary context switches */
    long   ru_nivcsw;           /* Involuntary context switches */
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN 1

#define PRIO_PROCESS    0       /* process priority */
#define PRIO_PGRP       1       /* process group priority */
#define PRIO_USER       2       /* user priority */

/* Minimal stubs: NexsOS1 doesn't enforce limits, so these mostly succeed
 * with dummy values. */
int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);
int getrusage(int who, struct rusage *usage);
int getpriority(int which, int who);
int setpriority(int which, int who, int prio);

#endif
