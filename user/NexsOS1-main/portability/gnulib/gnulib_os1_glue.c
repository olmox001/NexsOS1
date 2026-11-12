/*
 * user/sys/lib/portability/gnulib/gnulib_os1_glue.c
 * Real implementation connecting Gnulib abstractions to NexsOS1 syscalls and VFS.
 */

#ifndef _GNULIB_OS1_GLUE_IMPL
#define _GNULIB_OS1_GLUE_IMPL
#endif
#include "gnulib_os1_glue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* The GNU progname provider owns program_name. NexsOS getprogname and
 * setprogname share that one canonical value instead of maintaining a
 * shadow that is silently ignored after GNU has initialized it. */
extern const char *program_name;

static char g_progname_buf[64] = "nexsos_app";

int gnulib_os1_getpagesize(void) {
    return NEXSOS_PAGE_SIZE;
}

int gnulib_os1_getdtablesize(void) {
    return NEXSOS_MAX_FD;
}

const char *getprogname(void) {
    if (program_name && *program_name) {
        return program_name;
    }
    return g_progname_buf;
}

void setprogname(const char *name) {
    if (name == NULL || name[0] == '\0')
        return;
    /* BSD getprogname returns the leaf, unlike GNU program_name which
     * generally contains argv[0]. Keep GNU's public storage canonical
     * while preserving the short BSD accessor's exact contract. */
    const char *leaf = strrchr(name, '/');
    leaf = leaf == NULL ? name : leaf + 1;
    size_t len = strlen(leaf);
    if (len == 0U || len >= sizeof(g_progname_buf))
        return;
    /* The bound is constant for analysis and len was validated above. */
    for (size_t i = 0U; i < sizeof(g_progname_buf); ++i) {
        g_progname_buf[i] = leaf[i];
        if (i == len)
            break;
    }
    program_name = g_progname_buf;
}

const char *gnulib_os1_getprogname(void) {
    return getprogname();
}

void gnulib_os1_setprogname(const char *name) {
    setprogname(name);
}

ssize_t gnulib_os1_safe_read(int fd, void *buf, size_t count) {
    return read(fd, (char *)buf, count);
}

ssize_t gnulib_os1_safe_write(int fd, const void *buf, size_t count) {
    return write(fd, (const char *)buf, count);
}

void *gnulib_os1_rawmemchr(const void *s, int c) {
    const unsigned char *p = (const unsigned char *)s;
    unsigned char uc = (unsigned char)c;
    while (*p != uc) {
        p++;
    }
    return (void *)p;
}

void *gnulib_os1_memrchr(const void *s, int c, size_t n) {
    if (n == 0) return NULL;
    const unsigned char *p = (const unsigned char *)s + n - 1;
    unsigned char uc = (unsigned char)c;
    while (n > 0) {
        if (*p == uc) {
            return (void *)p;
        }
        p--;
        n--;
    }
    return NULL;
}

#include <error.h>
#include <stdarg.h>

unsigned int error_message_count = 0;
int error_one_per_line = 0;
void (*error_print_progname)(void) = NULL;

void verror(int status, int errnum, const char *format, va_list args) {
    fflush(stdout);
    const char *pname = getprogname();
    if (error_print_progname) {
        error_print_progname();
    } else {
        if (pname && *pname) {
            fprintf(stderr, "%s: ", pname);
        }
    }
    vfprintf(stderr, format, args);
    if (errnum) {
        fprintf(stderr, ": %s", strerror(errnum));
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    error_message_count++;

    if (errnum) {
        OS1_report_error(pname ? pname : "gnulib", errnum);
    } else if (status) {
        OS1_report_error(pname ? pname : "gnulib", EFAULT);
    }

    if (status) {
        exit(status);
    }
}

void error(int status, int errnum, const char *format, ...) {
    va_list args;
    va_start(args, format);
    verror(status, errnum, format, args);
    va_end(args);
}

void verror_at_line(int status, int errnum, const char *filename,
                    unsigned int linenumber, const char *format, va_list args) {
    fflush(stdout);
    const char *pname = getprogname();
    if (error_print_progname) {
        error_print_progname();
    } else {
        if (pname && *pname) {
            fprintf(stderr, "%s:%s:%u: ", pname, filename ? filename : "", linenumber);
        } else {
            fprintf(stderr, "%s:%u: ", filename ? filename : "", linenumber);
        }
    }
    vfprintf(stderr, format, args);
    if (errnum) {
        fprintf(stderr, ": %s", strerror(errnum));
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    error_message_count++;

    if (errnum) {
        OS1_report_error(pname ? pname : "gnulib", errnum);
    } else if (status) {
        OS1_report_error(pname ? pname : "gnulib", EFAULT);
    }

    if (status) {
        exit(status);
    }
}

void error_at_line(int status, int errnum, const char *filename,
                   unsigned int linenumber, const char *format, ...) {
    va_list args;
    va_start(args, format);
    verror_at_line(status, errnum, filename, linenumber, format, args);
    va_end(args);
}

size_t __fpending(FILE *fp) {
    return fp ? (size_t)fp->wcount : 0;
}

/* memeq — external linkable version for when the static inline can't be used.
   This is needed especially on aarch64 where the compiler doesn't always inline it.
   The static inline version in gnulib_config_nexsos.h will be used when possible,
   and this external version is used as a fallback. */
int memeq(const void *a, const void *b, size_t n) {
    return memcmp(a, b, n) == 0;
}

/* fts stubs for chmod and other utilities that traverse directory trees */
#include "fts_.h"
#include <errno.h>

FTS *fts_open(char * const *argv, int options, 
              int (*compar)(const FTSENT **, const FTSENT **)) {
    (void)argv;
    (void)options;
    (void)compar;
    errno = ENOSYS;
    return NULL;
}

FTSENT *fts_read(FTS *sp) {
    (void)sp;
    errno = ENOSYS;
    return NULL;
}

FTSENT *fts_children(FTS *sp, int instr) {
    (void)sp;
    (void)instr;
    errno = ENOSYS;
    return NULL;
}

int fts_set(FTS *sp, FTSENT *p, int instr) {
    (void)sp;
    (void)p;
    (void)instr;
    errno = ENOSYS;
    return -1;
}

int fts_close(FTS *sp) {
    (void)sp;
    errno = ENOSYS;
    return -1;
}