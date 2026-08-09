#ifndef LUA_PORTABILITY_H
#define LUA_PORTABILITY_H

/* l_noret: attribute for functions that never return.
 * Lua's ldo.c luaD_throw is marked l_noret but GCC may not see all
 * paths as noreturn (longjmp path). Force the attribute. */
#if defined(__GNUC__)
#define l_noret __attribute__((noreturn)) void
#else
#define l_noret void
#endif

#include <setjmp.h>
#include <stddef.h>
/* time_t/struct timespec/clock_gettime already exist as the system header
 * (include/api/time.h) - reuse it instead of a second, parallel typedef of
 * time_t here (the exact "we have a system header the lua compat layer
 * isn't using" mismatch: two independent `typedef long time_t;` in scope
 * are harmless today only because they happen to agree byte-for-byte; drift
 * between the two would silently desync the ABI Lua's time functions and
 * every other syscall wrapper agree on). Only struct tm/localtime/gmtime/
 * strftime/mktime below are genuinely new - time.h does not declare them. */
#include <time.h>
#include <math.h>

/* Provide sig_atomic_t before Lua's <signal.h> include path resolves.
 * NexsOS1's <signal.h> does not declare it, but Lua's lstate.h uses it
 * via the l_signalT macro. Force-include this header before Lua's
 * sources so the type is always in scope. */
#ifndef _SIG_ATOMIC_T_DEFINED
#define _SIG_ATOMIC_T_DEFINED
typedef int sig_atomic_t;
#endif

/* Pre-define Lua's l_signalT so lstate.h's #if !defined(l_signalT)
 * check resolves to our definition (matches the l_signalT = sig_atomic_t
 * line in lstate.h, but takes priority). */
#if !defined(l_signalT) && !defined(LUA_PORT_SIGNALT_DEFINED)
#define LUA_PORT_SIGNALT_DEFINED
#define l_signalT sig_atomic_t
#endif

/* NexsOS1's <math.h> now provides a full math library.
 * The math symbols used by the Lua core and lmathlib are now in <math.h>.
 * Only strtod and non-math symbols remain here. */

/* HUGE_VAL/HUGE_VALF are now provided by <math.h> */

/*
 * Lua module search path — the SINGLE authoritative definition.
 *
 * luaconf.h ships /usr/local paths; because this header is force-included
 * (-include) ahead of lua.h, our LUA_ROOT/LDIR/CDIR and
 * LUA_PATH_DEFAULT win over luaconf.h's #ifndef guards.  The Makefile no
 * longer passes any -DLUA_* path macro (a "...;..." -D can't survive the
 * shell — the ';' break the compile line), so keep the paths here only.
 *
 * Root is /lib, NOT /home: the VFS write-ACL (kernel/fs/vfs.c
 * vfs_write_allowed) makes /home the only user-writable tree, while /lib is
 * read-for-all / root-write-only.  System Lua modules therefore live in /lib
 * (any process can require them; users cannot tamper with them); /home would
 * expose them to overwrite.  sys/lib is the build-time source tree, not a
 * runtime VFS path.  Trailing '/' matters: luaconf.h concatenates directly.
 */
#define LUA_ROOT "/lib/"
#define LUA_LDIR LUA_ROOT "lua/5.4/lib/"
#define LUA_CDIR LUA_ROOT "lua/5.4/lib/"

/* Search the system module tree, then the current directory.  Defined
 * explicitly (rather than left to luaconf.h) to avoid the LDIR==CDIR
 * duplicate pair its generator would emit. */
#define LUA_PATH_DEFAULT                                                       \
  LUA_LDIR "?.lua;" LUA_LDIR "?/init.lua;"                                     \
           "./?.lua;./?/init.lua"
#define LUA_CPATH_DEFAULT LUA_CDIR "?.so;" LUA_CDIR "loadall.so;./?.so"

/* Locale: override luaconf.h's localeconv-based macro before luaconf.h
 * includes it.  NexsOS1 has no locale support; the decimal point is always '.'.
 */
#define lua_getlocaledecpoint() ('.')

/* time_t/struct tm/clock_gettime/nanosleep/time/mktime/difftime/localtime/
 * gmtime/strftime are declared in <time.h> (system header).
 * clock_t/tmpnam/clock are implemented below. */
typedef long clock_t;
#ifndef CLOCKS_PER_SEC
#define CLOCKS_PER_SEC 1000000000L
#endif
#ifndef L_tmpnam
#define L_tmpnam 128
#endif

char *tmpnam(char *s);
clock_t clock(void);

/* Double conversions */
int strcoll(const char *s1, const char *s2);

#include <locale.h>

/* Declaration of os1 library open function */
struct lua_State;
int luaopen_os1(struct lua_State *L);

/*
 * NOTE(LUA-TTY-02): lua.c chooses "interactive REPL" vs "EXECUTE stdin" with
 * lua_stdin_is_tty(), whose ISO-C fallback (lua.c) hardcodes it to 1 —
 * literally commented there as "assume stdin is a tty" —
 * unless LUA_USE_POSIX is defined.  We deliberately do NOT define
 * LUA_USE_POSIX (it also drags in popen/dlopen assumptions we do not provide),
 * so lua ASSUMED a tty unconditionally: `echo "print(10)" | lua` and
 * `lua < script` entered the REPL instead of executing stdin — and the REPL
 * reads the KEYBOARD mailbox (LUA-TTY-01 below), not fd 0, so the piped program
 * was never consumed (it printed the banner + '>' and waited for a keypress).
 *
 * Wire it to our REAL isatty() (lib.c): a genuine capability-type test of the
 * descriptor's object — CONSOLE vs FILE vs PIPE — so a redirected or piped
 * stdin correctly EXECUTES, while an interactive console still gets the REPL
 * plus the keyboard reader below.  This is the lua-side half of the fix; the
 * libc side was implementing isatty(), which unistd.h had only DECLARED.
 */
#include <unistd.h>
#define lua_stdin_is_tty() isatty(0)

/*
 * NOTE(LUA-TTY-01): REPL line input overrides lua.c's lua_readline/
 * _initreadline/_saveline/_freeline fallback (fgets(stdin) - see lua.c's own
 * '#if !defined(lua_readline)' guard; force-including this header ahead of
 * lua.c makes our definitions win, no lua.c edits needed). fgets()/read(fd
 * 0,...) hands back keyboard.c's BASE ascii_map byte, not the character
 * after the active keyboard layout's overrides (the .key vs .utf8 split
 * input_event_t already exposes for windowed apps); nxlua has no window, so
 * it decodes its own mailbox instead (lua_portability.c). Echo stays with
 * nxexec.h's host-side relay (USR-TTY-01 #123) - os1_lua_readline only
 * accumulates the line.
 */
#define LUA_PORT_MAXINPUT 512
int os1_lua_readline(char *buf, int bufsize);

#define lua_initreadline(L) ((void)L)
#define lua_readline(L, b, p)                                                  \
  ((void)L, print(p), os1_lua_readline((b), LUA_PORT_MAXINPUT))
#define lua_saveline(L, line)                                                  \
  {                                                                            \
    (void)L;                                                                   \
    (void)line;                                                                \
  }
#define lua_freeline(L, b)                                                     \
  {                                                                            \
    (void)L;                                                                   \
    (void)b;                                                                   \
  }

#endif /* LUA_PORTABILITY_H */