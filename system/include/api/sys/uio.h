/*
 * include/api/sys/uio.h
 * POSIX vectorized I/O interface for NexsOS1.
 */

#ifndef _SYS_UIO_H
#define _SYS_UIO_H

#include <posix_types.h>

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#endif /* _SYS_UIO_H */
