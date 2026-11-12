/*
 * include/api/grp.h
 * POSIX group database interface for NexsOS1.
 */

#ifndef _GRP_H
#define _GRP_H

#include <posix_types.h>
#include <stddef.h>

struct group {
    char   *gr_name;
    char   *gr_passwd;
    gid_t   gr_gid;
    char  **gr_mem;
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);
void          setgrent(void);
struct group *getgrent(void);
void          endgrent(void);

#endif /* _GRP_H */
