/*
 * user/sys/lib/portability/coreutils/coreutils_os1_glue.c
 * Implementation of Coreutils glue for NexsOS1.
 */

#include "coreutils_os1_glue.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <poll.h>
#include <unistd.h>

int coreutils_os1_uname(struct os1_utsname *buf) {
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    strncpy(buf->sysname, "NexsOS1", sizeof(buf->sysname) - 1);
    buf->sysname[sizeof(buf->sysname) - 1] = '\0';

    strncpy(buf->nodename, "nexsos", sizeof(buf->nodename) - 1);
    buf->nodename[sizeof(buf->nodename) - 1] = '\0';

    strncpy(buf->release, "0.0.5.4", sizeof(buf->release) - 1);
    buf->release[sizeof(buf->release) - 1] = '\0';

    strncpy(buf->version, "ASTRA-V0.0.5.4", sizeof(buf->version) - 1);
    buf->version[sizeof(buf->version) - 1] = '\0';

#if defined(ARCH_AMD64) || defined(__x86_64__)
    strncpy(buf->machine, "x86_64", sizeof(buf->machine) - 1);
#else
    strncpy(buf->machine, "aarch64", sizeof(buf->machine) - 1);
#endif
    buf->machine[sizeof(buf->machine) - 1] = '\0';

    return 0;
}

int posix2_version(void) {
    return 200809;
}

int isapipe(int fd) {
    struct stat st;
    if (fstat(fd, &st) < 0)
        return 0;
    return S_ISFIFO(st.st_mode) ? 1 : 0;
}

int iopoll(int fdin, int fdout, bool block) {
    (void)fdin;
    (void)block;
    if (fdout < 0)
        return 0;

    struct pollfd pfd;
    pfd.fd = fdout;
    pfd.events = POLLOUT;
    pfd.revents = 0;

    int r = poll(&pfd, 1, block ? -1 : 0);
    if (r < 0)
        return -3; /* IOPOLL_ERROR */
    if (r == 0 && !block)
        return 0;
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
        return -2; /* IOPOLL_BROKEN_OUTPUT */

    return 0;
}

bool iopoll_input_ok(int fdin) {
    struct stat st;
    bool always_ready = fstat(fdin, &st) == 0
                        && (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode));
    return !always_ready;
}

bool iopoll_output_ok(int fdout) {
    return isapipe(fdout) > 0;
}

bool write_wait(int fd, void const *buffer, size_t size) {
    char const *buf = buffer;

#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

    while (size > 0) {
        ssize_t written = write(fd, buf, size);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLOUT;
                pfd.revents = 0;

                int rc = poll(&pfd, 1, -1);
                if (rc < 0 && errno == EINTR)
                    continue;
                if (rc < 0)
                    return false;
                continue;
            }
            return false;
        }
        if (written == 0)
            return false;
        buf += written;
        size -= (size_t)written;
    }

    return true;
}

/* mbschr and mbslen: multibyte string functions for coreutils.
 * In NexsOS1, we treat strings as UTF-8 but operate on bytes.
 * These stubs provide basic ASCII-compatible implementations. */

/* mbschr: find character in multibyte string.
 * In NexsOS1, since we're ASCII-compatible, just use strchr. */
char *mbschr(const char *s, int c) {
  if (!s)
    return NULL;
  return (char *)strchr(s, c);
}

/* mbslen: length of multibyte string.
 * In NexsOS1, treat as byte length (like strlen for ASCII). */
size_t mbslen(const char *s) {
  if (!s)
    return 0;
  return strlen(s);
}

/* mbrlen: minimal byte-wise multibyte-length helper used by pathchk.c.
 * NexsOS1 treats all bytes as single-byte characters in the bootstrap layer. */
size_t mbrlen(const char *s, size_t n, mbstate_t *ps) {
    (void)n;
    if (ps)
        memset(ps, 0, sizeof(*ps));
    if (!s)
        return 0;
    if (*s == '\0')
        return 0;
    return 1;
}
