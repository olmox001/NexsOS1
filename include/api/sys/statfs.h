#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H

#include <sys/types.h>

#ifndef __fsid_t_defined
#define __fsid_t_defined
typedef struct { int __val[2]; } fsid_t;
#endif

struct statfs {
  unsigned long f_type;
  unsigned long f_bsize;
  fsblkcnt_t f_blocks;
  fsblkcnt_t f_bfree;
  fsblkcnt_t f_bavail;
  fsfilcnt_t f_files;
  fsfilcnt_t f_ffree;
  fsid_t f_fsid;
  unsigned long f_namelen;
  unsigned long f_frsize;
  unsigned long f_flags;
  unsigned long f_spare[4];
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#endif
