/*
 * include/api/selinux/selinux.h
 * Standard SELinux stub definitions for non-SELinux systems (NexsOS1).
 */

#ifndef _SELINUX_SELINUX_H
#define _SELINUX_SELINUX_H

#include <sys/types.h>
#include <errno.h>

typedef char *security_context_t;

static inline int is_selinux_enabled(void) { return 0; }
static inline int getcon(security_context_t *con) { (void)con; errno = ENOTSUP; return -1; }
static inline void freecon(security_context_t con) { (void)con; }
static inline int setfscreatecon(const char *con) { (void)con; errno = ENOTSUP; return -1; }
static inline int matchpathcon_init(const char *path) { (void)path; errno = ENOTSUP; return -1; }
static inline void matchpathcon_fini(void) {}

#endif /* _SELINUX_SELINUX_H */
