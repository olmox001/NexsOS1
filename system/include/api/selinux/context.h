/*
 * include/api/selinux/context.h
 * Standard SELinux context stub for NexsOS1.
 */

#ifndef _SELINUX_CONTEXT_H
#define _SELINUX_CONTEXT_H

#include <selinux/selinux.h>

typedef void *context_t;

static inline context_t context_new(const char *str) { (void)str; errno = ENOTSUP; return (void *)0; }
static inline void context_free(context_t con) { (void)con; }
static inline const char *context_str(context_t con) { (void)con; return (void *)0; }
static inline int context_user_set(context_t con, const char *s) { (void)con; (void)s; errno = ENOTSUP; return -1; }
static inline int context_role_set(context_t con, const char *s) { (void)con; (void)s; errno = ENOTSUP; return -1; }
static inline int context_type_set(context_t con, const char *s) { (void)con; (void)s; errno = ENOTSUP; return -1; }
static inline int context_range_set(context_t con, const char *s) { (void)con; (void)s; errno = ENOTSUP; return -1; }

#endif /* _SELINUX_CONTEXT_H */
