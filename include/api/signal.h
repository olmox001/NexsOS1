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

struct sigaction {
  union {
    void (*sa_handler)(int);
    void (*sa_sigaction)(int, void *, void *);
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

sighandler_t signal(int signum, sighandler_t handler);
int raise(int sig);

#endif /* _SIGNAL_H */

