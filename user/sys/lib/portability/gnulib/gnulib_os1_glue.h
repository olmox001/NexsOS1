/*
 * user/sys/lib/portability/gnulib/gnulib_os1_glue.h
 * Glue header interfacing GNU Gnulib primitives to NexsOS1 kernel & libc APIs.
 */

#ifndef _GNULIB_OS1_GLUE_H
#define _GNULIB_OS1_GLUE_H

#include <os1.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#ifdef __cplusplus
extern "C" {
#endif

/* System constants for NexsOS1 userland */
#define NEXSOS_PAGE_SIZE 4096
#define NEXSOS_MAX_FD    64

/* Bridge utilities */
int  gnulib_os1_getpagesize(void);
int  gnulib_os1_getdtablesize(void);
const char *gnulib_os1_getprogname(void);
void gnulib_os1_setprogname(const char *name);

const char *getprogname(void);
void setprogname(const char *name);
int memeq(const void *a, const void *b, size_t n);

/* Safe file descriptor hooks */
ssize_t gnulib_os1_safe_read(int fd, void *buf, size_t count);
ssize_t gnulib_os1_safe_write(int fd, const void *buf, size_t count);

/* String & memory utilities needed by gnulib */
void *gnulib_os1_rawmemchr(const void *s, int c);
void *gnulib_os1_memrchr(const void *s, int c, size_t n);


#ifdef __cplusplus
}
#endif

#endif /* _GNULIB_OS1_GLUE_H */
