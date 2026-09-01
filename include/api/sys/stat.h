#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <time.h>
#include <posix_types.h>

#define UTIME_NOW  0x3fffffff
#define UTIME_OMIT 0x3ffffffe

struct stat {
    dev_t     st_dev;
    ino_t     st_ino;
    mode_t    st_mode;
    nlink_t   st_nlink;
    uid_t     st_uid;
    gid_t     st_gid;
    dev_t     st_rdev;
    off_t     st_size;
    time_t    st_atime;
    time_t    st_mtime;
    time_t    st_ctime;
};

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFLNK  0120000
#define S_IFCHR  0020000
#define S_IFBLK  0060000
#define S_IFIFO  0010000
#define S_IFSOCK 0140000

#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)


#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000

#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU 0700

/* mknod/mkfifo: NexsOS1 does not support device files or named pipes (POSIX
 * personality stub; these return -ENOTSUP at runtime). */
int mknod(const char *pathname, mode_t mode, dev_t dev);
int mkfifo(const char *pathname, mode_t mode);

#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXG 0070

#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IRWXO 0007

#define S_IRUGO (S_IRUSR | S_IRGRP | S_IROTH)
#define S_IWUGO (S_IWUSR | S_IWGRP | S_IWOTH)
#define S_IXUGO (S_IXUSR | S_IXGRP | S_IXOTH)
#define S_IRWXUGO (S_IRWXU | S_IRWXG | S_IRWXO)

int stat(const char *path, struct stat *buf);
int lstat(const char *path, struct stat *buf);
int fstat(int fd, struct stat *buf);
int mkdir(const char *path, mode_t mode);
mode_t umask(mode_t mask);
int chmod(const char *path, mode_t mode);
int fchmod(int fd, mode_t mode);
int lchmod(const char *path, mode_t mode);
int utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);
int fdutimensat(int fd, int dirfd, const char *pathname, const struct timespec times[2], int flags);

/* POSIX special-object type test macros — NexsOS1 has none of these */
#define S_TYPEISSHM(p) 0
#define S_TYPEISTMO(p) 0
#define S_TYPEISMQ(p)  0




#endif
