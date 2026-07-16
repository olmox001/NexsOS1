#ifndef _FCNTL_H
#define _FCNTL_H

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x0200
#define O_APPEND 0x0400
#define O_TRUNC  0x0800
#define O_EXCL   0x1000

/* The kernel SYS_OPEN honours only O_ACCMODE (it opens an existing file);
 * O_CREAT/O_TRUNC/O_APPEND are emulated in libc's open() (lib.c) on top of the
 * VFS create-on-write primitive, so no kernel change is needed and every
 * fd-based writer gets POSIX create/truncate/append semantics. */
int open(const char *pathname, int flags, ...);

#endif
