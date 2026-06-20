#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include "../posix_types.h"

/*
 * Minimal POSIX <sys/mman.h> for the OS1 userspace libc.
 *
 * OS1 has no demand-paged mmap; anonymous mappings (MAP_ANONYMOUS) are backed
 * by the userspace heap (malloc/free) in lib.c, so page-pool code ported from
 * POSIX (base-nexs core/pager.c) works.  File-backed mmap is not supported.
 * Implemented in the libc layer, NOT as an OS1 syscall.
 */

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);

#endif /* _SYS_MMAN_H */
