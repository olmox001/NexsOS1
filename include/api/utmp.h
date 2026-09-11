/*
 * include/api/utmp.h
 * utmp/wtmp ABI for NexsOS1 userland.
 *
 * This header is a CONTRACT between two things that must agree:
 *
 *   WRITER  user/sys/bin/nxenvinit.c — creates _PATH_UTMP and (empty)
 *           _PATH_WTMP at boot, filling one USER_PROCESS record whose
 *           fields are all read from THIS header.  Nothing in that file
 *           redeclares struct utmp, invents a field, or hardcodes a
 *           path; if a field name or a constant here changes, the bytes
 *           on disk follow.
 *
 *   READER  gnulib's readutmp.c (via <readutmp.h>'s HAVE_UTMP_H branch,
 *           which does `#include <utmp.h>` and then
 *           `typedef struct utmp STRUCT_UTMP;`) — the implementation
 *           behind `who`, `users`, `uptime`, `last`.  It parses the file
 *           with the same struct layout and the same record size
 *           (sizeof(struct utmp)) this header defines.
 *
 * Because both sides pull the layout from one file, they cannot drift —
 * that is the whole reason this header exists instead of a local
 * redeclaration in either the writer or the reader.
 *
 * Layout: field order, types and sizes match glibc's LP64
 * <sysdeps/gnu/bits/utmp.h> (the `__WORDSIZE_TIME64_COMPAT32` variant
 * that modern x86_64/aarch64 Linux userland uses for utmp-compatible
 * files).  readutmp.c has no #ifdefs that would let it "adapt" to a
 * different layout — it casts the file buffer straight to `struct utmp *`
 * and indexes fields by offset — so the layout is not ours to redesign;
 * it is what gnulib already assumes.
 *
 * Field semantics worth stating, because the writer in nxenvinit.c
 * depends on them:
 *
 *   ut_type       one of EMPTY/RUN_LVL/BOOT_TIME/…/USER_PROCESS/
 *                 DEAD_PROCESS below.  USER_PROCESS is the record `who`
 *                 prints; anything else is filtered out.
 *   ut_pid        pid of the login process (the shell's own pid here).
 *   ut_line       device name WITHOUT the "/dev/" prefix; UT_LINESIZE
 *                 bytes incl. NUL.  Conventionally "ttyN" or "pts/N".
 *   ut_id         inittab id, 4 RAW bytes, NOT NUL-terminated on glibc.
 *                 Copy with memcpy, never strncpy.
 *   ut_user       login name; UT_NAMESIZE bytes incl. NUL.  On NexsOS
 *                 this is the caller's LEVEL ("machine"/"root"/"user"/
 *                 "guest") — see nxenvinit.c's identity note.
 *   ut_host       remote host; UT_HOSTSIZE bytes incl. NUL.  Empty for
 *                 a local session.
 *   ut_exit       meaningful only on DEAD_PROCESS records; zero
 *                 otherwise.
 *   ut_session    session id.  Declared `long` here (glibc's non-
 *                 COMPAT32 branch) — 8 bytes on LP64, giving a struct
 *                 size of 392.  The writer and reader both build the
 *                 struct through this header, so the choice is
 *                 self-consistent either way; 392 matches what a
 *                 native-LP64 glibc (aarch64) would also produce.
 *   ut_tv         wall-clock of the entry.  TWO int32_t fields, not
 *                 time_t/tv_usec — glibc's utmp on-disk format pins
 *                 this to 8 bytes so 32- and 64-bit users of the same
 *                 file agree.  The 2038 ceiling is a property of the
 *                 format, not a bug in either side.
 *   ut_addr_v6    remote IPv6 address, or 0 for a local session.
 *   __glibc_      reserved tail kept at 20 bytes so the struct size
 *   reserved      matches glibc's exactly.  Named `__glibc_reserved`
 *                 (not `__unused`) because `<sys/cdefs.h>` on
 *                 BSD/macOS expands `__unused` to an attribute macro
 *                 and breaks the array declaration at compile time.
 *
 * Paths: _PATH_UTMP / _PATH_WTMP are the two files nxenvinit creates and
 * gnulib reads.  They live under /home because /home is the tree the
 * rootfs pre-creates writable; if these move, both the writer and any
 * caller that references the macro move with them, because both use the
 * macro, not a literal.
 *
 * The setutent()/getutent()/endutent()/utmpname() declarations are for
 * link-time compatibility with programs that reach for the sequential
 * read API.  gnulib's readutmp.c does NOT use them; it opens the file
 * itself.  None of the four has a definition in libgnulib.a; if a caller
 * ever links against one, the missing symbol is the diagnostic, not a
 * silent misbehaviour.
 */

#ifndef _NEXSOS_API_UTMP_H
#define _NEXSOS_API_UTMP_H

#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Login record types.  Numeric values from glibc; a reader's filter (e.g.
 * who.c's "is this a USER_PROCESS?") depends on them being exactly these. */
#define EMPTY 0
#define RUN_LVL 1
#define BOOT_TIME 2
#define NEW_TIME 3
#define OLD_TIME 4
#define INIT_PROCESS 5
#define LOGIN_PROCESS 6
#define USER_PROCESS 7
#define DEAD_PROCESS 8

/* Canonical file paths.  The writer (nxenvinit.c) and any reader (gnulib's
 * readutmp.c, via its own _PATH_UTMP default in <readutmp.h>) reference
 * these macros, never a literal, so a relocation is one edit. */
#define _PATH_UTMP "/home/var/run/utmp"
#define _PATH_WTMP "/home/var/log/wtmp"
#define _PATH_UTMPX "/home/var/run/utmpx"
#define _PATH_WTMPX "/home/var/log/wtmpx"

/* Field sizes.  UT_LINESIZE and UT_NAMESIZE are 32 on glibc LP64;
 * UT_HOSTSIZE is 256.  Both sides must agree byte for byte. */
#define UT_LINESIZE 32
#define UT_NAMESIZE 32
#define UT_HOSTSIZE 256

/* Exit status of a process marked DEAD_PROCESS.  glibc exposes the raw
 * two shorts; kept identical here so a reader that prints them (last,
 * who -d) formats them the same way. */
struct exit_status {
  short int e_termination;
  short int e_exit;
};

/* On-disk timestamp: 8 bytes total, two int32_t.  Named locally so the
 * struct utmp declaration below does not have to pull <sys/time.h> in and
 * accidentally pick up a `struct timeval` that would be 16 bytes on LP64. */
struct __utmp_timeval {
  int32_t tv_sec;
  int32_t tv_usec;
};

struct utmp {
  short ut_type;               /* one of the EMPTY/.../DEAD_PROCESS above */
  pid_t ut_pid;                /* pid of the login process                */
  char ut_line[UT_LINESIZE];   /* tty name, no "/dev/" prefix, NUL-padded */
  char ut_id[4];               /* inittab id: 4 RAW bytes, NOT NUL-term.  */
  char ut_user[UT_NAMESIZE];   /* login name, NUL-padded                  */
  char ut_host[UT_HOSTSIZE];   /* remote host, NUL-padded; empty locally  */
  struct exit_status ut_exit;  /* meaningful on DEAD_PROCESS only         */
  long ut_session;             /* session id; 8 bytes on LP64             */
  struct __utmp_timeval ut_tv; /* entry wall-clock; 2 x int32_t, 8 bytes  */
  int32_t ut_addr_v6[4];       /* remote IPv6 address, or 0               */
  /* Reserved tail.  Name chosen so no host header can macro-substitute it:
   * `<sys/cdefs.h>` on BSD/macOS defines `__unused` as
   * `__attribute__((__unused__))`, which would turn a declaration of
   * `char __unused[20];` into invalid syntax.  Size (20) is part of the
   * ABI and matches glibc. */
  char __glibc_reserved[20];
};

/* Sequential read API — declared for link-time compatibility only.  No
 * definition ships in libgnulib.a or in lib.o; gnulib's readutmp.c
 * deliberately opens the file itself and parses it with the layout above,
 * so nothing on the boot or `who`/`users` path calls these.  Kept declared
 * so a ported program that does call them fails at link with a clear
 * "undefined symbol: getutent" instead of a compile error on the
 * prototype. */
extern void setutent(void);
extern void endutent(void);
extern struct utmp *getutent(void);
extern int utmpname(const char *file);

#ifdef __cplusplus
}
#endif

#endif /* _NEXSOS_API_UTMP_H */