#ifndef _DIRENT_H
#define _DIRENT_H

#include "posix_types.h"

/*
 * Minimal POSIX <dirent.h> for the OS1 userspace libc.
 *
 * Backed by list_dir() (os1.h), whose output is a space-separated name list
 * (ext4_list).  opendir() snapshots the listing into the DIR buffer and
 * readdir() tokenises it.  d_type is always DT_UNKNOWN (the listing carries no
 * type).  Implemented in the libc layer, NOT as an OS1 syscall.
 */

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

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
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);

#endif /* _DIRENT_H */
