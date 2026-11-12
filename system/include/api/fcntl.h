#ifndef _FCNTL_H
#define _FCNTL_H

#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT     0x0200
#define O_APPEND    0x0400
#define O_TRUNC     0x0800
#define O_EXCL      0x1000
#define O_NONBLOCK  0x4000
#define O_NOCTTY    0x0400
#define O_DIRECTORY 0x10000
#define O_NOFOLLOW  0x20000
#define O_CLOEXEC   0x80000
#define O_SEARCH    0
#define O_BINARY    0
#define O_TEXT      0

/* access() mode flags (for POSIX access checks) */
#define F_OK        0  /* existence */
#define X_OK        1  /* execute */
#define W_OK        2  /* write */
#define R_OK        4  /* read */

#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define F_DUPFD_CLOEXEC 1030

#define FD_CLOEXEC 1

#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_SYMLINK_FOLLOW 0x400
#define AT_REMOVEDIR 0x200

int fcntl(int fd, int cmd, ...);
int open(const char *pathname, int flags, ...);
int openat(int dirfd, const char *pathname, int flags, ...);
int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags);
int fchmodat(int dirfd, const char *pathname, mode_t mode, int flags);
int fchmod(int fd, mode_t mode);
int utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);
int fdutimensat(int fd, int dirfd, const char *pathname, const struct timespec times[2], int flags);




#endif
