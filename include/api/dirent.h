#ifndef _DIRENT_H
#define _DIRENT_H

#include <posix_types.h>

/*
 * POSIX-like <dirent.h> for the OS1 userspace libc.
 *
 * Backed by list_dir() (os1.h), which snapshots a directory listing into the
 * DIR buffer and readdir() tokenises it. GNU Coreutils/Gnulib expects the full
 * d_type contract even though OS1 filesystem metadata is still minimal.
 */

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK    10
#define DT_SOCK   12
#define DT_WHT    14

#define IFTODT(mode) (((mode) & 0170000) >> 12)

struct dirent {
  ino_t d_ino;
  unsigned char d_type;
  char d_name[256];
};

typedef struct {
  char buf[1024]; /* snapshot of list_dir() output */
  int pos;        /* tokeniser cursor into buf */
  struct dirent ent;
} DIR;

DIR *opendir(const char *name);
DIR *fdopendir(int fd);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
int dirfd(DIR *dirp);

#endif /* _DIRENT_H */
