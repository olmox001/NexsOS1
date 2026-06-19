#ifndef _POLL_H
#define _POLL_H

/*
 * Minimal POSIX <poll.h> for the OS1 userspace libc.
 *
 * OS1 has no pollable fd set; IPC is checked with try_recv() (os1.h). poll()
 * (lib.c) returns 0 (timeout, no fd ready), matching base-nexs's own
 * baremetal libc behaviour, so non-blocking poll(...,0) callers fall through
 * to their try_recv path. Implemented in the libc layer, NOT as an OS1 syscall.
 */

typedef unsigned long nfds_t;

struct pollfd {
  int fd;
  short events;
  short revents;
};

#define POLLIN  0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#endif /* _POLL_H */
