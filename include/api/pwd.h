/*
 * include/api/pwd.h
 * POSIX password database interface for NexsOS1.
 */

#ifndef _PWD_H
#define _PWD_H

#include <posix_types.h>
#include <stddef.h>

struct passwd {
    char   *pw_name;       /* username */
    char   *pw_passwd;     /* user password */
    uid_t   pw_uid;        /* user ID */
    gid_t   pw_gid;        /* group ID */
    char   *pw_gecos;      /* real name / comment */
    char   *pw_dir;        /* home directory */
    char   *pw_shell;      /* shell program */
};

struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);
void           setpwent(void);
struct passwd *getpwent(void);
void           endpwent(void);

#endif /* _PWD_H */
