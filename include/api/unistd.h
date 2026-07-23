/*
 * include/api/unistd.h
 * The POSIX <unistd.h> PERSONALITY over the OS1 base API (ASTRA layering).
 *
 * Layering contract (docs/ASTRA.md, PLAN-2026-07-17 stratification):
 *   - `OS1_*` / `OS1low_*` (os1.h) are the NEXS base API — the single
 *     implementation of every primitive.
 *   - the POSIX names here are a THIN COMPATIBILITY MAPPING onto them, so
 *     ported software can `#include <unistd.h>` and get the standard surface
 *     while NEXS-native code keeps calling the OS1 verbs directly.  Both are
 *     available; neither re-implements the other (no duplicated logic).
 *
 * This header is the authoritative POSIX entry point: including it gives the
 * COMPLETE set below.  Some names are declared in os1.h (which this includes)
 * because they predate the split — they are listed here so the POSIX surface is
 * documented in one place, but are deliberately NOT re-declared, so there is
 * exactly one declaration of each and the two headers cannot drift.
 *
 * POSIX name        OS1 primitive it maps to            declared in
 * ---------------------------------------------------------------------------
 * read()            OS1_object_read / SYS_READ          os1.h
 * write()           OS1_object_write / SYS_WRITE        os1.h
 * close()           OS1low_handle_close                 os1.h
 * lseek()           OBJ_CTL_SEEK on the handle          os1.h
 * chdir()           OS1_fs_chdir                        os1.h
 * getcwd()          OS1_fs_getcwd                       os1.h
 * sbrk()            OS1low_vm_sbrk                      os1.h
 * pipe()            OS1low_pipe (SYS_PIPE, OBJ_TYPE_PIPE)   here
 * unlink()          OS1_fs_unlink (SYS_UNLINK)              here
 * truncate()        OS1_fs_read/OS1_fs_write (whole-file)   here
 * ftruncate()       lseek/read/write + OBJ_CTL_TRUNCATE     here
 * isatty()          OS1low_cap_query (type == CONSOLE)      here
 * getpid()          get_pid / OS1low_process_self           here
 * usleep()          SYS_NANOSLEEP (real timer sleep)        here
 *
 * KNOWN DEVIATIONS from strict POSIX (intentional, documented rather than
 * silently wrong):
 *   - read()/write() take `char *` / `const char *`, not `void *`, and return
 *     `long` rather than `ssize_t`.
 *   - getcwd() returns int (0 on success), NOT `char *`.
 *   - sleep(unsigned seconds) is NOT declared: os1.h's OS1_sleep() takes
 *     MILLISECONDS, and declaring a POSIX sleep() here would silently change
 *     the unit for existing callers.  Use usleep()/nanosleep() or OS1_sleep().
 */
#ifndef _UNISTD_H
#define _UNISTD_H

#include <os1.h>

/* Standard file descriptors. */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* usleep(usec): microseconds, POSIX-compatible, backed by the real kernel timer
 * (SYS_NANOSLEEP) — the caller is descheduled and woken by its core's tick, no
 * busy-wait.  The primitive is nanosleep-granular (rounded up to the tick,
 * ~10 ms at HZ=100).  For ns/timespec sleeps and the monotonic clock use
 * <time.h> (nanosleep(), clock_gettime()). */
int usleep(unsigned int usec);

/* pipe: a real anonymous byte pipe (OBJ_TYPE_PIPE via SYS_PIPE).  pipefd[0] is
 * the READ end, pipefd[1] the WRITE end; both are ordinary descriptors, so
 * read()/write()/close() work on them and the shell can hand either end to a
 * child as its stdin/stdout.  Returns 0, or -1 with errno set. */
int pipe(int pipefd[2]);

/* unlink: real VFS delete (SYS_UNLINK via OS1_fs_unlink), same as remove(). */
int unlink(const char *pathname);

/* isatty: true iff the descriptor's underlying object is a CONSOLE.  This is a
 * real capability-type test (OS1low_cap_query), NOT "fd < 3" — with shell
 * redirection fd 1 may be a FILE (`cmd > out`) or a PIPE (`cmd | cmd`), and
 * interactive programs (the lua REPL) must correctly see isatty()==0 there. */
int isatty(int fd);

/* getpid: POSIX name for the OS1 get_pid(). */
int getpid(void);

/* truncate/ftruncate: set a file's length exactly.  Built on the FS-layer
 * whole-file-replace primitive (an offset-0 write REPLACES the file — the
 * Phase 1 truncation standard); ftruncate reaches the same effect through an
 * open descriptor, using OBJ_CTL_TRUNCATE for the truncate-to-empty case that a
 * zero-length write cannot express (POSIX write(fd,...,0) is a no-op). */
int truncate(const char *path, long length);
int ftruncate(int fd, long length);

#endif
