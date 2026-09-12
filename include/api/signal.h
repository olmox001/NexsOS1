#ifndef _SIGNAL_H
#define _SIGNAL_H

/*
 * POSIX-like <signal.h> for the OS1 userspace libc.
 *
 * OS1 does not implement real asynchronous delivery, but the GNU/Gnulib source
 * tree expects the standard signal API surface at compile time. The libc layer
 * implements the no-op semantics used by command-line utilities: handlers are
 * accepted but do not change process execution. This is intentionally a
 * compatibility contract, not a Linux emulation layer.
 */

#include <stddef.h>

#ifndef _SIG_ATOMIC_T
#define _SIG_ATOMIC_T
typedef int sig_atomic_t;
#endif

typedef unsigned long sigset_t;
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGILL  4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS  7
#define SIGFPE  8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGPOLL 29

#define NSIG 32

#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO   4
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

#define FPE_INTDIV 1
#define FPE_INTOVF 2
#define FPE_FLTDIV 3
#define FPE_FLTOVF 4
#define FPE_FLTUND 5
#define FPE_FLTRES 6
#define FPE_FLTINV 7
#define FPE_FLTSUB 8

#ifndef _SIGINFO_T
#define _SIGINFO_T
typedef struct {
  int si_signo;
  int si_errno;
  int si_code;
  int si_pid;
  int si_uid;
  void *si_addr;
  int si_status;
  long si_band;
  union {
    int _pad[28];
  } _sifields;
} siginfo_t;
#endif

#ifndef _STACK_T_DEFINED
#define _STACK_T_DEFINED
typedef struct {
  void *ss_sp;
  int ss_flags;
  size_t ss_size;
} stack_t;
#endif

struct sigaction {
  union {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, siginfo_t *, void *);
  } __sa_handler;
  sigset_t sa_mask;
  int sa_flags;
  void (*sa_restorer)(void);
};
#define sa_handler   __sa_handler.sa_handler
#define sa_sigaction __sa_handler.sa_sigaction

int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);
int sigprocmask(int how, const sigset_t *restrict set, sigset_t *restrict oldset);
int sigaction(int signum, const struct sigaction *restrict act,
              struct sigaction *restrict oldact);
int sigaltstack(const stack_t *restrict ss, stack_t *restrict oss);

sighandler_t signal(int signum, sighandler_t handler);
int raise(int sig);

#endif /* _SIGNAL_H */

