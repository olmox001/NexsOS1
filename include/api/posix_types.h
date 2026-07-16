/*
 * include/api/posix_types.h
 * POSIX-facing shared types for the userland API surface
 */
#ifndef _POSIX_TYPES_H
#define _POSIX_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Basic types */
typedef int64_t ssize_t;
typedef int32_t pid_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;
typedef uint32_t mode_t;
typedef int64_t off_t;
typedef uint64_t ino_t;
typedef uint32_t dev_t;
typedef uint32_t nlink_t;
typedef int64_t time_t;
typedef int64_t blkcnt_t;
typedef int32_t blksize_t;

/* Physical and virtual addresses */
typedef uint64_t phys_addr_t;
typedef uint64_t virt_addr_t;

/* Atomic types */
typedef struct {
  volatile int32_t counter;
} atomic_t;
typedef struct {
  volatile int64_t counter;
} atomic64_t;

/* Page size */
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))

/* Alignment macros */
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)

/* Pointer arithmetic */
#define PTR_ALIGN(p, a) ((__typeof__(p))ALIGN((unsigned long)(p), (a)))

/* Bit operations */
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (64 - 1 - (h))))

/* Container of */
#define container_of(ptr, type, member)                                        \
  __extension__({                                                              \
    const __typeof__(((type *)0)->member) *__mptr = (ptr);                     \
    (type *)((char *)__mptr - offsetof(type, member));                         \
  })

/* Array size */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Min/Max */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(val, lo, hi) MIN(MAX(val, lo), hi)

/* Error codes (POSIX compatible) */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define ENOTBLK 15
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENODEV 19
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOTTY 25
#define ETXTBSY 26
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EMLINK 31
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define ENOSYS 38
#define ENOTEMPTY 39

/* Success */
#define EOK 0

/* os1_strerror - errno -> human-readable string.  Single source of truth for
 * BOTH kernel and userland (this header is already the shared home of the
 * EPERM..ENOTEMPTY macros above, included by the kernel build via
 * -Iinclude/api same as userland) — previously userland's lib.c carried its
 * own 11-case switch (strerror()) that silently fell back to "Unknown
 * error" for anything past ENOSYS.  static inline: safe to include in any
 * number of translation units, no libc calls. */
static inline const char *os1_strerror(int err) {
  switch (err) {
  case EOK: return "Success";
  case EPERM: return "Operation not permitted";
  case ENOENT: return "No such file or directory";
  case ESRCH: return "No such process";
  case EINTR: return "Interrupted system call";
  case EIO: return "I/O error";
  case ENXIO: return "No such device or address";
  case E2BIG: return "Argument list too long";
  case ENOEXEC: return "Exec format error";
  case EBADF: return "Bad file descriptor";
  case ECHILD: return "No child processes";
  case EAGAIN: return "Resource temporarily unavailable";
  case ENOMEM: return "Cannot allocate memory";
  case EACCES: return "Permission denied";
  case EFAULT: return "Bad address";
  case ENOTBLK: return "Block device required";
  case EBUSY: return "Device or resource busy";
  case EEXIST: return "File exists";
  case EXDEV: return "Cross-device link";
  case ENODEV: return "No such device";
  case ENOTDIR: return "Not a directory";
  case EISDIR: return "Is a directory";
  case EINVAL: return "Invalid argument";
  case ENFILE: return "Too many open files in system";
  case EMFILE: return "Too many open files";
  case ENOTTY: return "Inappropriate ioctl for device";
  case ETXTBSY: return "Text file busy";
  case EFBIG: return "File too large";
  case ENOSPC: return "No space left on device";
  case ESPIPE: return "Illegal seek";
  case EROFS: return "Read-only file system";
  case EMLINK: return "Too many links";
  case EPIPE: return "Broken pipe";
  case EDOM: return "Numerical argument out of domain";
  case ERANGE: return "Numerical result out of range";
  case ENOSYS: return "Function not implemented";
  case ENOTEMPTY: return "Directory not empty";
  default: return "Unknown error";
  }
}

/* open(2) flags (ABI-03 fd table).  Only the access mode is supported for
 * now: the VFS cannot create or truncate files yet, so O_CREAT/O_TRUNC/
 * O_APPEND are rejected with -EINVAL rather than silently ignored.
 * Guarded: userland fcntl.h/stdio.h carry the same values. */
#ifndef O_RDONLY
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#endif
#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif
/* Creation/truncation/append personality flags.  SYS_OPEN honours these
 * (create/truncate via the vfs_write_allowed + vfs_create seam); userland
 * fcntl.h carries the same values.  Kept here too so the kernel dispatcher
 * sees them. */
#ifndef O_CREAT
#define O_CREAT 0x0200
#define O_APPEND 0x0400
#define O_TRUNC 0x0800
#define O_EXCL 0x1000
#endif

/* lseek(2) whence */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* Likely/Unlikely branch hints */
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* Compiler attributes */
#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __section(s) __attribute__((section(s)))
#define __unused __attribute__((unused))
#define __noreturn __attribute__((noreturn))
#define __weak __attribute__((weak))
#define __always_inline inline __attribute__((always_inline))

/* Memory barriers */
#define barrier() __asm__ __volatile__("" : : : "memory")
#define mb() __asm__ __volatile__("dsb sy" : : : "memory")
#define rmb() __asm__ __volatile__("dsb ld" : : : "memory")
#define wmb() __asm__ __volatile__("dsb st" : : : "memory")
#define isb() __asm__ __volatile__("isb" : : : "memory")

/* SMP memory barriers */
#define smp_mb() mb()
#define smp_rmb() rmb()
#define smp_wmb() wmb()

/* NULL */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* IPC message types */
#define IPC_TYPE_RAW 0
#define IPC_TYPE_INPUT 1
#define IPC_TYPE_NOTIFY 0x100
#define IPC_TYPE_MOUSE 4
#define IPC_TYPE_RESIZE 0x200 /* window/desktop resize: data1=w, data2=h (GFX-DYN-01) */

/* IPC_LOOK_PING_MAGIC - marks an IPC_TYPE_NOTIFY message (in .data2) as a
 * SILENT compositor look-changed ping (nxres_broadcast_look, user/sys/bin/
 * nxres.h) rather than a user-facing notification — a real notify() call
 * never sets data2, so the two can never collide.  Lives here (not in
 * nxres.h) because input_poll_event (user/sys/lib/lib.c) — the SINGLE
 * consumer of every windowed app's IPC mailbox — needs it too: every app
 * already funnels keyboard/mouse/resize through that one try_recv() call,
 * so a second, competing try_recv() loop elsewhere in the same app (the
 * first version of this feature) silently STOLE mouse/keyboard messages
 * before input_poll_event ever saw them.  Recognizing the ping inside
 * input_poll_event itself (as INPUT_TYPE_LOOK_CHANGED, input.h) keeps the
 * mailbox single-consumer, exactly like IPC_TYPE_RESIZE already is. */
#define IPC_LOOK_PING_MAGIC 0x4C4F4F4Bu /* "LOOK" */

/* IPC message structure */
struct ipc_message {
  int from;
  int type;
  uint64_t data1;
  uint64_t data2;
  char payload[64];
};

#endif /* _POSIX_TYPES_H */
