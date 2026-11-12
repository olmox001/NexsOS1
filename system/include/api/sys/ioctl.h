#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

/*
 * Minimal POSIX <sys/ioctl.h> for the OS1 userspace libc.
 *
 * Only TIOCGWINSZ is recognised, answered from the window's character grid
 * (window_grid, os1.h) when the fd is a window, else failing with -1 so callers
 * fall back to a default size (the kilo port relies on this fallback).  All
 * other requests return -1.  Implemented in the libc layer, NOT an OS1 syscall.
 */

struct winsize {
  unsigned short ws_row;
  unsigned short ws_col;
  unsigned short ws_xpixel;
  unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413

int ioctl(int fd, unsigned long request, ...);

#endif /* _SYS_IOCTL_H */
