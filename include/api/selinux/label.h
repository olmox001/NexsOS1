/*
 * include/api/selinux/label.h
 * Standard SELinux label stub for NexsOS1.
 */

#ifndef _SELINUX_LABEL_H
#define _SELINUX_LABEL_H

#include <selinux/selinux.h>

struct selabel_handle;
#define SELABEL_CTX_FILE 0

static inline struct selabel_handle *selabel_open(int backend, const void *opts, unsigned nopts) {
    (void)backend; (void)opts; (void)nopts;
    errno = ENOTSUP;
    return (void *)0;
}

static inline void selabel_close(struct selabel_handle *hnd) {
    (void)hnd;
}

static inline int selabel_lookup(struct selabel_handle *hnd, security_context_t *context,
                                 const char *key, int type) {
    (void)hnd; (void)context; (void)key; (void)type;
    errno = ENOTSUP;
    return -1;
}

#endif /* _SELINUX_LABEL_H */
