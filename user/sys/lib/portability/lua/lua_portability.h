#ifndef LUA_PORTABILITY_H
#define LUA_PORTABILITY_H

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

/* NexsOS1's <math.h> is minimal (only fabs). Lua needs more. Declare
 * the math symbols used by the Lua core and lmathlib. Implementations
 * are provided in lua_portability.c. */
#ifndef HUGE_VAL
#define HUGE_VAL (__builtin_huge_val())
#endif
#ifndef HUGE_VALF
#define HUGE_VALF (__builtin_huge_valf())
#endif

#ifndef LUA_USE_NEXSOS
#define LUA_USE_NEXSOS
#endif

/*
 * Lua module search path — the SINGLE authoritative definition.
 *
 * luaconf.h ships /usr/local paths; because this header is force-included
 * (-include lua_portability.h) ahead of lua.h, our LUA_ROOT/LDIR/CDIR and
 * LUA_PATH_DEFAULT win over luaconf.h's #ifndef guards.  The Makefile no
 * longer passes any -DLUA_* path macro (a \"...;...\" -D can't survive the
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
#define LUA_PATH_DEFAULT                                                        \
  LUA_LDIR "?.lua;" LUA_LDIR "?/init.lua;"                                      \
           "./?.lua;./?/init.lua"
#define LUA_CPATH_DEFAULT                                                       \
  LUA_CDIR "?.so;" LUA_CDIR "loadall.so;./?.so"

double ldexp(double x, int exp);
double frexp(double x, int *exp);
double modf(double x, double *iptr);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double pow(double x, double y);
double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double log(double x);
double log10(double x);
double log2(double x);
double exp(double x);
double cosh(double x);
double sinh(double x);
double tanh(double x);

/* Locale definitions and categories */
#define LC_ALL 0
#define LC_COLLATE 1
#define LC_CTYPE 2
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5

struct lconv {
  char *decimal_point;
  char *thousands_sep;
  char *grouping;
  char *int_curr_symbol;
  char *currency_symbol;
  char *mon_decimal_point;
  char *mon_thousands_sep;
  char *mon_grouping;
  char *positive_sign;
  char *negative_sign;
  char int_frac_digits;
  char frac_digits;
  char p_cs_precedes;
  char p_sep_by_space;
  char n_cs_precedes;
  char n_sep_by_space;
  char p_sign_posn;
  char n_sign_posn;
};

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

/* Time and Clock definitions.  time_t/struct timespec/clock_gettime come
 * from the system <time.h> (included above); only clock_t is new here. */
typedef long clock_t;

#ifndef CLOCKS_PER_SEC
#define CLOCKS_PER_SEC 1000000000L
#endif

#ifndef L_tmpnam
#define L_tmpnam 128
#endif

char *tmpnam(char *s);
clock_t clock(void);
double difftime(time_t time1, time_t time0);

struct tm {
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
  int tm_wday;
  int tm_yday;
  int tm_isdst;
};

time_t time(time_t *t);
struct tm *localtime(const time_t *timep);
struct tm *gmtime(const time_t *timep);
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);
time_t mktime(struct tm *tm);

/* Double conversions */
double strtod(const char *nptr, char **endptr);
int strcoll(const char *s1, const char *s2);

/* Declaration of os1 library open function */
struct lua_State;
int luaopen_os1(struct lua_State *L);

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
