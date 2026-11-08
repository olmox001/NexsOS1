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
 * unlink()          OS1_fs_unlink (parent DIR capability)   here
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

/* unlink: parent-directory capability mutation via OS1_fs_unlink(), same as
 * remove(). */
int unlink(const char *pathname);
int link(const char *oldpath, const char *newpath);

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
int fchdir(int fd);
unsigned int sleep(unsigned int seconds);
int getpagesize(void);
pid_t fork(void);
int chown(const char *path, uid_t owner, gid_t group);
int lchown(const char *path, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
int chownat(int dirfd, const char *pathname, uid_t owner, gid_t group);
int lchownat(int dirfd, const char *pathname, uid_t owner, gid_t group);
int fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group, int flags);
int rmdir(const char *pathname);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int fsync(int fd);
int fdatasync(int fd);
void sync(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int access(const char *pathname, int mode);
ssize_t copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out, size_t len, unsigned int flags);

/* Process / signal primitives */
pid_t getppid(void);
int   kill(pid_t pid, int sig);
int   setuid(uid_t uid);
int   seteuid(uid_t uid);
int   setgid(gid_t gid);
int   setegid(gid_t gid);
int   setreuid(uid_t ruid, uid_t euid);
int   setregid(gid_t rgid, gid_t egid);
int   getgroups(int size, gid_t list[]);

/* Terminal / tty */
char *ttyname(int fd);
int   ttyname_r(int fd, char *buf, size_t buflen);
char *getlogin(void);
int   getlogin_r(char *buf, size_t bufsize);

/* Misc POSIX */
long  sysconf(int name);
unsigned int alarm(unsigned int seconds);
int   pause(void);
int   nice(int inc);
int   lchmod(const char *path, mode_t mode);
int   getentropy(void *buffer, size_t length);

/* exec family: replace the current process image with a new program.
 * NexsOS1 implements these as stubs that return -ENOTSUP (not supported). */
int   execv(const char *pathname, char *const argv[]);
int   execvp(const char *file, char *const argv[]);
int   execl(const char *pathname, const char *arg, ...);
int   execlp(const char *file, const char *arg, ...);
int   execle(const char *pathname, const char *arg, ...);

/* sysconf names */
#define _SC_NPROCESSORS_ONLN  84
#define _SC_NPROCESSORS_CONF  83
#define _SC_PHYS_PAGES        85
#define _SC_AVPHYS_PAGES      86
#define _SC_PAGESIZE           30
#define _SC_CLK_TCK            2
#define _SC_OPEN_MAX           5
#define _SC_LOGIN_NAME_MAX   71
#define _SC_HOST_NAME_MAX   72









extern char **environ;

/* Standard POSIX getopt variables */

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;
int getopt(int argc, char *const argv[], const char *optstring);

#endif




