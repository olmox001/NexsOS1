/*
 * user/sys/lib/portability/coreutils/coreutils_os1_glue.h
 * NexsOS1 glue interface for GNU Coreutils.
 */

#ifndef _COREUTILS_OS1_GLUE_H
#define _COREUTILS_OS1_GLUE_H

#include <os1.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct os1_utsname {
    char sysname[32];
    char nodename[32];
    char release[32];
    char version[32];
    char machine[32];
};

int coreutils_os1_uname(struct os1_utsname *buf);
int posix2_version(void);

/* Multibyte string functions for coreutils expr.c / pathchk.c */
char *mbschr(const char *s, int c);
size_t mbslen(const char *s);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);

/* iopoll functions (implementations in coreutils_os1_glue.c) */
int isapipe(int fd);
int iopoll(int fdin, int fdout, bool block);
bool iopoll_input_ok(int fdin);
bool iopoll_output_ok(int fdout);
bool write_wait(int fd, void const *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* _COREUTILS_OS1_GLUE_H */
