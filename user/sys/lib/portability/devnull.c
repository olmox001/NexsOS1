/*
 * user/sys/lib/portability/devnull.c
 *
 * Synthetic /dev/null (USR-DEVNULL-01).
 *
 * NexsOS1 has no /dev tree: file objects only exist for real ext4 paths,
 * and the VFS has no concept of a device node. GNU Coreutils opens
 * "/dev/null" unconditionally in a few well-known spots — nohup.c's
 * "redirect stdin when it's a tty" path is the one this tree's
 * coreutils_test exercises — so plain open("/dev/null", ...) failing
 * with ENOENT broke those call sites even though nothing else about
 * them is unsupported.
 *
 * Per project convention (see user/sys/lib/portability/), fixes to
 * behaviour that lives in code NOT owned by this portability layer are
 * not made by hand-editing that code. lib.c is NexsOS1's native,
 * single-object C runtime (linked whole into every ELF, per its own
 * file header) — same situation as the vendored Gnulib/Coreutils trees
 * conceptually, in that the fix belongs beside the feature gap, not
 * inside the existing implementation. This file uses the linker's
 * --wrap mechanism instead of a source edit: every user ELF is linked
 * with -Wl,--wrap=<symbol> for each function below (see USER_LINK_FLAGS
 * in the top-level Makefile), which redirects callers of e.g. open() to
 * __wrap_open() here, and gives __wrap_open() a way to reach the
 * original implementation via __real_open(). lib.c itself is untouched
 * and un-recompiled by this change.
 *
 * Scope is intentionally narrow: only the literal path "/dev/null" is
 * recognized (nothing else under /dev, which still doesn't exist).
 * Every other path/fd falls straight through to the real lib.c
 * implementation with zero overhead beyond the one string compare (for
 * open()) or one integer compare (for the fd-based calls).
 *
 * NOTE (USR-DUP2-01, still open, NOT fixed by this file): lib.c's
 * dup2() is a same-process fd no-op — it returns newfd without
 * aliasing the underlying object — so nohup's open("/dev/null") +
 * dup2(fd, 0) sequence now gets past the open() call (no more spurious
 * "failed to render standard input unusable" error) but does NOT
 * actually retarget fd 0 to the null device: reads on fd 0 still reach
 * whatever fd 0 already was. Fixing that needs real fd-aliasing in the
 * kernel's handle table and is out of scope for a portability-layer
 * patch; flagged here so it isn't mistaken for fixed.
 */

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

/* NX_DEVNULL_FD: a fixed sentinel fd that can never collide with a real
 * kernel fd. gnulib_os1_glue.h caps the real per-process fd table at
 * NEXSOS_MAX_FD (64); this sits far above that on purpose. */
#define NX_DEVNULL_FD 1000

/* Real (un-wrapped) lib.c entry points, reached via -Wl,--wrap=<name>. */
extern int __real_open(const char *pathname, int flags, ...);
extern long __real_read(int fd, char *buf, unsigned long count);
extern long __real_write(int fd, const char *buf, size_t count);
extern int __real_close(int fd);
extern long __real_lseek(int fd, long offset, int whence);
extern int __real_isatty(int fd);
extern int __real_fstat(int fd, struct stat *buf);

/* Prototypes required by -Wmissing-prototypes / -Werror. */
int __wrap_open(const char *pathname, int flags, ...);
long __wrap_read(int fd, char *buf, unsigned long count);
long __wrap_write(int fd, const char *buf, size_t count);
int __wrap_close(int fd);
long __wrap_lseek(int fd, long offset, int whence);
int __wrap_isatty(int fd);
int __wrap_fstat(int fd, struct stat *buf);

int __wrap_open(const char *pathname, int flags, ...) {
  if (pathname && strcmp(pathname, "/dev/null") == 0)
    return NX_DEVNULL_FD;
  /* lib.c's open() ignores the variadic mode argument entirely (it
   * never declares a va_list for it), so it is safe to not forward it
   * here either. */
  return __real_open(pathname, flags);
}

long __wrap_read(int fd, char *buf, unsigned long count) {
  if (fd == NX_DEVNULL_FD) {
    (void)buf;
    (void)count;
    return 0; /* EOF, like the real device */
  }
  return __real_read(fd, buf, count);
}

long __wrap_write(int fd, const char *buf, size_t count) {
  if (fd == NX_DEVNULL_FD) {
    (void)buf;
    return (long)count; /* discard, report success like the real device */
  }
  return __real_write(fd, buf, count);
}

int __wrap_close(int fd) {
  if (fd == NX_DEVNULL_FD)
    return 0;
  return __real_close(fd);
}

long __wrap_lseek(int fd, long offset, int whence) {
  if (fd == NX_DEVNULL_FD) {
    (void)offset;
    (void)whence;
    return 0;
  }
  return __real_lseek(fd, offset, whence);
}

int __wrap_isatty(int fd) {
  if (fd == NX_DEVNULL_FD)
    return 0;
  return __real_isatty(fd);
}

int __wrap_fstat(int fd, struct stat *buf) {
  if (fd == NX_DEVNULL_FD) {
    if (!buf) {
      errno = EFAULT;
      return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->st_mode = S_IFCHR | 0666;
    buf->st_nlink = 1;
    return 0;
  }
  return __real_fstat(fd, buf);
}