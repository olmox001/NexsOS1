/*
 * user/sys/lib/lib.c
 * Userland C runtime and system call wrapper library
 *
 * This file is the sole C runtime for all userland processes.  It is
 * compiled into lib.o and linked WHOLE (single object, not an archive) into
 * every ELF (there is no shared library mechanism); USER_CFLAGS builds it
 * with -ffunction-sections -fdata-sections and the ELF link step applies
 * -Wl,--gc-sections (see USR-BLOAT-01/02 below), so "linked whole" no
 * longer means "every function pays rent in every ELF" the way it did when
 * this file was first audited. It provides:
 *
 *   - Thin C wrappers around every _sys_*() assembly stub from syscall.S.
 *   - Standard I/O emulation (fopen/fclose/fread/fwrite/fseek/ftell) backed
 *     by file_read/file_write syscalls, write-buffered per FILE.
 *   - Formatting (printf, snprintf, sprintf, vsnprintf, vsscanf, sscanf).
 *   - Input event decoding (input_poll_event: keyboard and mouse IPC msgs).
 *   - Graphics helpers (graphics_draw_rect, graphics_blit, graphics_draw_text,
 *     graphics_text_width, graphics_load_image).
 *   - A real (registry-backed) POSIX personality: strdup, strtol, abs, fabs,
 *     atof/strtod, getenv/setenv/unsetenv, mkdir, system, stat, puts, fflush,
 *     remove, rename, vfprintf, waitpid, pipe, and ~100 further shims.
 *   - UTF-8 decoder (utf8_decode).
 *   - Stack smash protector stub (__stack_chk_guard, __stack_chk_fail).
 *
 * NASA/JPL "Power of Ten" compliance policy (added by this pass, applied
 * incrementally module-by-module — see MODULE banners below for what has
 * been brought into line so far):
 *   R1  Simple control flow only: no goto, no direct or indirect recursion.
 *   R2  Every loop has a statically visible upper bound.
 *   R3  No heap allocation after process start-up on any syscall-veneer or
 *       formatting hot path (malloc.c's arena is for application code).
 *   R4  A function body fits on one printed "page" (~60 lines); split
 *       larger ones into named helpers.
 *   R5  >= 2 runtime assertions per non-trivial function, guarding
 *       preconditions/postconditions; gated behind NX_STRICT (opt-in via
 *       `make NX_STRICT=1`, the same knob kernel/nx_contract.h's
 *       NX_MUST_USE checks were originally gated behind — see nx_assert()
 *       below for why this file keeps it opt-in) so release builds keep
 *       today's perf and debug builds get the checks.
 *   R6  Declare every object at the smallest possible scope.
 *   R7  Check the return value of every non-void call, or cast to
 *       (void) with a comment saying why the result is intentionally
 *       unused.
 *   R8  Preprocessor limited to inclusion guards and simple constant/
 *       function-like macros; no conditional compilation that hides a
 *       function's control flow from the reader.
 *   R9  At most one level of pointer dereference per expression; no more
 *       than one function-pointer indirection (s_atexit_handlers[] is the
 *       one deliberate exception — a fixed-size callback table, not a
 *       dispatch chain).
 *   R10 Compile clean under -Wall -Wextra -Wpedantic -Wshadow
 *       -Wwrite-strings (already COMMON_FLAGS) with zero suppressions
 *       introduced by this pass.
 * Deviations from R1-R10 that cannot be closed inside this file alone are
 * recorded at the point of deviation with a "DEVIATION(Rn): ..." comment
 * instead of being silently left out — see USR-BLOAT-01/02 and
 * USR-LIB-01 immediately below for the two structural ones.
 *
 * STB libraries (NOTE USR-BLOAT-01/02 — FIXED at the build-system layer,
 * VERIFIED 2026-09 against the Makefile actually in use, not the version
 * this comment previously cross-checked): USER_CFLAGS now carries
 * -ffunction-sections -fdata-sections, and USER_LINK_FLAGS carries
 * -Wl,--gc-sections unconditionally (plus -Wl,-s for `BUILD=release`,
 * `BUILD ?= debug` keeping full symbols by default) — exactly the change
 * this comment used to propose as a separate, unapplied patch. lib.o still
 * compiles STB_IMAGE_IMPLEMENTATION unconditionally and is still linked as
 * one whole object, but with per-function/per-data sections and the linker
 * actually asked to drop unreferenced ones, an ELF that never calls
 * graphics_load_image no longer pays for stb_image's code — the mechanism
 * this comment previously said was necessary AND sufficient is now both. No
 * further lib.c change closes this; re-measuring counter.elf's size after a
 * `BUILD=release` rebuild is the way to confirm it, not a code review of
 * this file.
 *
 * Kernel source inclusion (NOTE USR-LIB-01 — VERIFIED STILL OPEN):
 *   vsnprintf.c, math.c, string.c are sourced directly from kernel/lib/ via
 *   relative #include paths (see the MODULE 2 banner below, where the
 *   #includes live). Any internal change to those kernel files silently
 *   changes userland behaviour with no compiler diagnostic.
 *   DEVIATION(R8): this is a real boundary violation, not just a style
 *   nit, and the correct fix is architectural, not a #include swap: extract
 *   the freestanding-safe subset of those three kernel files into a THIRD
 *   location (e.g. lib/common/{string,vsnprintf,math}.c) that both
 *   kernel/lib/ and user/sys/lib/ symlink or copy from at build time, so
 *   there is exactly one source of truth instead of an include-path
 *   coupling. That move touches kernel/lib/ and the Makefile, which are
 *   outside this file's blast radius for this pass — left as a documented,
 *   correctly-scoped TODO rather than papered over.
 *
 * Known issues — RE-AUDITED against the actual code below and the kernel
 * source (not just re-asserted from the previous pass):
 *   USR-LIB-01   OPEN (see above; unchanged).
 *   USR-LIB-02   FIXED. Stdio wrappers do proper NULL checks (no more
 *                `(size_t)fp > 10` magic-value guard). fopen() streams are
 *                write-buffered (FILE.wbuf, FILE_WBUF_SIZE) so incremental
 *                writers issue one syscall per bufferful instead of one per
 *                fwrite(); fflush/fclose/fseek and every fread() flush first.
 *   USR-LIB-03   FIXED. graphics_draw_text no longer declares a 100KB static
 *                buffer or falls back to terminal rendering.
 *   USR-LIB-04   FIXED, contrary to what this note previously claimed:
 *                system() runs NXShell for real and returns its exit status
 *                (not a hardcoded 0); getenv/setenv/unsetenv/environ are a
 *                real POSIX personality over a registry-backed OS1_env_*
 *                layer (see the ENVIRONMENT module); atof() is strtod(), a
 *                real IEEE-754 parse, not `(double)atoi()`; mkdir() composes
 *                over a real parent-directory capability (OS1_fs_mkdir).
 *                The previous note describing these as no-ops/truncating was
 *                itself stale documentation, not a bug in the code — kept
 *                here, corrected, so the history is not lost.
 *   USR-LIB-05   FIXED, likewise stale as previously written: vfprintf()
 *                honours `stream` via fwrite() — a real fopen()ed FILE*
 *                writes to its own path/position, it does not land on fd 1.
 *                One real, narrower gap remains and is worth keeping on
 *                record: stdin/stdout/stderr are three fds over ONE kernel
 *                CONSOLE object (kernel/core/object.c), so stdout and
 *                stderr are visually indistinguishable on this console —
 *                that is a kernel/console-object property, not something
 *                vfprintf can fix by itself.
 *   USR-SEC-01   FIXED AT THE KERNEL BOUNDARY, which is the architecturally
 *                correct place for it: SYS_REGISTRY's write path is gated by
 *                CAP_REG_WRITE and a first-writer-wins owner_pid check
 *                (kernel/lib/registry.c), enforced before userland ever gets
 *                a say — so a userland-side re-check here would be
 *                redundant, not defense in depth (the untrusted side cannot
 *                be the enforcement point). Kept as a comment at the
 *                OS1_registry_* wrappers pointing at the kernel enforcement,
 *                so the next reader does not "fix" it a second time.
 *   USR-SEC-02   FIXED AT THE KERNEL BOUNDARY, same shape as USR-SEC-01:
 *                SYS_KILL is gated by process_kill_allowed()
 *                (kernel/core/syscall_dispatch.c — CAP_IPC_ANY, or
 *                target is caller's parent/ancestor) and SYS_SEND is gated
 *                by CAP_IPC_ANY for non-relatives. send()/kill_process()
 *                stay thin veneers on purpose; duplicating the check in
 *                userland cannot add real security since userland is not
 *                the trust boundary.
 *   USR-BLOAT-01 FIXED at the build-system layer (see above) — verified
 *                against the Makefile now in use, not proposed.
 *   USR-BLOAT-02 FIXED, same Makefile change (-Wl,-s under `BUILD=release`)
 *                — see above.
 */
#include "portability/os1_video_platform.h"
#include <ctype.h>
#include <errno.h>
#include <execsvc.h>
#include <fcntl.h>
#include <graphics.h>
#include <input.h>
#include <inttypes.h>
#include <langinfo.h>
#include <limits.h> /* INT_MAX — nx_assert() bound checks (R5) */
#include <math.h>
#include <os1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>
/* POSIX compatibility shims implemented at the bottom of this file (the OS1
 * onion-userland libc layer, epic #120; no new OS1 syscalls). */
#include <dirent.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/wait.h> /* waitpid(), WNOHANG, WEXITSTATUS (Phase 2) */
#include <termios.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-braces"
/* ==========================================================================
 * MODULE 6 — Graphics: image/video decoders and drawing primitives
 * (R1-R10 applied to this module; see the file header's compliance policy)
 * ========================================================================== */
/*
 * STB_IMAGE_IMPLEMENTATION: embed the full stb_image decoder.
 * STBI_NO_STDIO/LINEAR/HDR disable file-I/O and HDR format support that
 * are unavailable or unnecessary in a freestanding environment.
 * STBI_NO_THREAD_LOCALS/STBI_NO_FAILURE_STRINGS are required by OS1 userland:
 * there is no initialized TLS block behind TPIDR_EL0, so stb's __thread failure
 * state would fault in ordinary decoder error paths.
 * VERIFIED FIX (was NOTE(USR-BLOAT-01) "adds ~50KB of .text to every ELF
 * regardless of use"): still compiled unconditionally here, but the
 * Makefile now builds this file with -ffunction-sections/-fdata-sections
 * and links every user ELF with -Wl,--gc-sections (see the file header),
 * so an ELF that never calls graphics_load_image no longer carries this
 * decoder's code — the 50KB was a property of the OLD link step, not of
 * compiling this header in, and does not need this header split into its
 * own translation unit to go away.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS 4096
#include <stb_image.h>
#pragma GCC diagnostic pop

/*
 * OS1VID_IMPLEMENTATION: istanzia qui, una sola volta per l'intero link
 * (stesso pattern di STB_IMAGE_IMPLEMENTATION sopra — vale anche qui la
 * stessa nota USR-BLOAT-01/02 "FIXED at the build-system layer": con
 * -ffunction-sections/--gc-sections un ELF che non usa mai il video player
 * non si porta dietro pl_mpeg), il compat layer video
 * (include/api/os1vid.h) che include a sua volta include/api/pl_mpeg.h
 * (upstream MIT, NON modificato:
 * https://raw.githubusercontent.com/phoboslab/pl_mpeg/master/pl_mpeg.h).
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#pragma GCC diagnostic ignored "-Wmissing-braces"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define OS1VID_IMPLEMENTATION
#include <os1vid.h>
#pragma GCC diagnostic pop

/* errno: global error variable expected by POSIX-style libc callers. */
int errno = 0;

/* nx_assert - Power-of-Ten R5 runtime assertion, compiled in only when
 * NX_STRICT is defined. VERIFIED against the actual Makefile (not assumed):
 * NX_STRICT is opt-in (`make NX_STRICT=1`), gating warn_unused_result via
 * NX_MUST_USE in kernel/nx_contract.h for the SAME stated reason this file
 * documents below — nx_contract.h's own history is that a gate you have to
 * remember to switch on caught nothing, so NX_MUST_USE was made unconditional
 * there. nx_assert() stays opt-in rather than copying that fix, because unlike
 * a compiler attribute it has a real runtime cost and an abort side effect;
 * `make NX_STRICT=1` is this project's existing "how much does the strict
 * profile catch" report (see the Makefile's own NX_STRICT comment) and this
 * hooks into that same report instead of adding a second strictness knob.
 * When active, on failure it reports through the ONE error seam
 * (OS1_report_error, prototyped in <os1.h> and already included above) with
 * EFAULT, so a violated invariant is visible the same way a hard I/O fault
 * is, then aborts via OS1low_process_exit (likewise from <os1.h>) — a
 * violated precondition here means a bug in the caller, not a recoverable
 * I/O condition, so returning an error code is not an option: continuing
 * would touch dangling handles or a NULL stream with a "confirmed working"
 * codepath around it. */
#ifdef NX_STRICT
#define nx_assert(cond)                                                      \
  do {                                                                       \
    if (!(cond)) {                                                           \
      OS1_report_error("nx_assert:" __FILE__ ":" #cond, EFAULT);             \
      OS1low_process_exit(134); /* 128+SIGABRT, matching POSIX abort() */    \
    }                                                                        \
  } while (0)
#else
/* R5/off-path: NX_STRICT off must not just vanish to ((void)0) — that leaves
 * every variable that existed only to feed a condition (e.g. `idlen` in
 * OS1low_process_wait_status) looking "assigned but never read" to
 * -Wunused-variable, and this tree builds with -Werror. `(void)(cond)`
 * still counts as a use for the compiler, still costs nothing observable
 * (the comparison's result is discarded, and `cond` must stay
 * side-effect-free by the same rule any assert() condition follows), and
 * keeps release builds warning-clean without adding an abort path. */
#define nx_assert(cond) ((void)(cond))
#endif

/* These timezone helpers are required by the gnulib time layer and must be
 * declared before their definitions to satisfy the project's strict -Werror
 * configuration. */
time_t mktime_z(timezone_t tz, struct tm *tm);
struct tm *localtime_rz(timezone_t tz, const time_t *t, struct tm *tm);

/*
 * errno_ret - THE POSIX errno seam for the syscall veneers below.
 *
 * Kernel syscalls return a NEGATIVE errno on failure (e.g. -ENOENT, -EACCES);
 * the POSIX contract every libc caller relies on is "set errno to the positive
 * code and return -1".  Routing the file/fd wrappers through here is what stops
 * failures surfacing as strerror(0) == "Success" (the "(Success)" Lua's
 * io.open printed on a missing file) — and it is deliberately generic: ANY
 * POSIX-style consumer, not just Lua, gets correct errno/strerror from now on.
 */
/* ==========================================================================
 * MODULE 1 — errno seam and the single error-reporting policy
 * (R1-R10 applied; see the file header's compliance policy)
 * ========================================================================== */
static long errno_ret_ctx(long r, const char *ctx) {
  if (r < 0) {
    /* R5: `errno = (int)-r` truncates a long to an int; every kernel errno
     * is a small positive constant (<posix_types.h>), so -r must already
     * fit in an int before the cast — assert it instead of silently
     * truncating a would-be-huge "errno" into an unrelated small one. */
    nx_assert(-r > 0 && -r <= INT_MAX);
    errno = (int)-r;
    /*
     * The classifier runs HERE, so the diagnostic declaration is universal.
     *
     * OS1_report_error's own comment promises that a hard failure in ANY libc
     * or portability path becomes visible, but it was reached from three call
     * sites only — so every OTHER POSIX veneer set errno correctly and was
     * never classified.  A promise kept at three sites is not a policy, it is
     * a convention each new compatibility layer (SDL, lua, doom, tomorrow
     * musl) has to remember to call — and "remember to call it" is the shape
     * of a duplication waiting to diverge.
     *
     * Safe to route everything through it because the POLICY is already
     * selective: ENOENT/EAGAIN and the rest of normal control flow stay
     * silent, EACCES/EPERM warn, and only EIO/EFAULT/ENOMEM/ENOSPC/EROFS
     * raise the red one.  Probes cannot spam it.
     */
    OS1_report_error(ctx, (int)r);
    return -1;
  }
  return r;
}

/* errno_ret - the POSIX seam without a context tag (ctx defaults to "libc").
 * Callers that know the object they failed on should use errno_ret_ctx so the
 * notification names it. */
static long errno_ret(long r) { return errno_ret_ctx(r, NULL); }

/*
 * OS1_report_error - THE single userland error-surfacing seam
 * (PLAN-2026-07-17-STRATIFICATION.md, Phase 0).
 *
 * Maps an errno to the notification system BY SEVERITY CLASS, so a hard
 * failure in ANY libc / portability path (SDL, lua, doom, ...) becomes visible
 * — a red/amber notification with context — instead of a silent -1 an app can
 * ignore and loop on.  Every layer calls THIS one policy; none hand-rolls its
 * own notify (no duplication, uniform behaviour).  Normal control-flow errors
 * stay silent so probes (ENOENT, EAGAIN) never spam:
 *
 *   EIO/EFAULT/ENOMEM/ENOSPC/EROFS -> error (red)   — unexpected / hard fault
 *   EACCES/EPERM                   -> warn  (amber) — policy denial
 *   everything else                -> silent        — normal control flow
 *
 * `err` may be given as errno (positive) or as a raw -errno syscall return.
 * `ctx` is a short tag: a subsystem ("vfs", "mmap") or "op path".
 */
void OS1_report_error(const char *ctx, int err) {
  if (err < 0)
    err = -err;
  if (!ctx)
    ctx = "libc";
  switch (err) {
  case EIO:
  case EFAULT:
  case ENOMEM:
  case ENOSPC:
  case EROFS:
    OS1_notify_error(ctx, strerror(err));
    break;
  case EACCES:
  case EPERM:
    OS1_notify_warn(ctx, strerror(err));
    break;
  default:
    break; /* normal control-flow error: no notification */
  }
}

/* ==========================================================================
 * MODULE 2 — Syscall wrappers, time, and process control
 * (R1-R10 applied to this module; see the file header's compliance policy)
 *
 * Each function below is a thin C-callable veneer over an assembly stub in
 * user/arch/<arch>/syscall.S.  Arguments are passed in the arch ABI registers
 * (x0-x5 on AArch64, rdi/rsi/rdx/r10/r8/r9 on x86-64) by the C compiler;
 * the stub moves the syscall number into x8/rax and issues svc/syscall.
 *
 * send(), kill_process(), and spawn() deliberately accept any pid/path with
 * NO check in this file — see USR-SEC-02 in the file header: SYS_KILL and
 * SYS_SEND are capability-gated in the kernel (process_kill_allowed(),
 * CAP_IPC_ANY), which is the real trust boundary; a second check here would
 * be theatre, not defense in depth.
 * ========================================================================== */
long read(int fd, char *buf, unsigned long count) {
  return errno_ret(_sys_read(fd, buf, count));
}
long write(int fd, const char *buf, size_t count) {
  return errno_ret(_sys_write(fd, buf, count));
}
long OS1_time_now(void) { return _sys_get_time(); }
long get_time(void) { return OS1_time_now(); } /* compat shim (DIR-01 F4) */

time_t time(time_t *t) {
  time_t sec = (time_t)OS1_time_now();
  if (t)
    *t = sec;
  return sec;
}

/* Tier 3 os1 time primitives (docs/TIMER-MODEL.md §4); SYS_CLOCK_GETTIME
 * clk 0 = monotonic ns since boot, clk 1 = this process's CPU time in ns. */
unsigned long long os1_mono_ns(void) {
  return (unsigned long long)_sys_clock_gettime(0);
}
unsigned long long os1_cpu_ns(void) {
  return (unsigned long long)_sys_clock_gettime(1);
}
/* clock_gettime: POSIX layer over the os1 primitives (<time.h>). */
int clock_gettime(int clk, struct timespec *ts) {
  if (!ts)
    return -1;
  unsigned long long ns =
      (clk == CLOCK_PROCESS_CPUTIME_ID) ? os1_cpu_ns() : os1_mono_ns();
  ts->tv_sec = (time_t)(ns / 1000000000ULL);
  ts->tv_nsec = (long)(ns % 1000000000ULL);
  return 0;
}

/* nanosleep: POSIX blocking sleep over the SYS_NANOSLEEP primitive (<time.h>).
 * Not interruptible here, so it always completes: *rem is zeroed, returns 0. */
int nanosleep(const struct timespec *req, struct timespec *rem) {
  if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L)
    return -1;
  unsigned long long ns = (unsigned long long)req->tv_sec * 1000000000ULL +
                          (unsigned long long)req->tv_nsec;
  _sys_nanosleep(ns);
  if (rem) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}
/* --- Process control: OS1low_ canonical primitives (ASTRA §6.1, DIR-01 F4) ---
 * The stable low-level process surface; each just wraps a _sys_ stub, so there
 * is no new syscall and no behaviour change.  The bare verbs below are
 * zero-breakage compat shims forwarding here (kill_process/wait/yield are shims
 * defined further down, next to their original neighbours).  exit keeps the
 * while(1): unreachable dead code that silences the "noreturn" warning in
 * compilers that do not see svc #0 as a terminator. */
/* Forward declaration: every spawn primitive below propagates
 * the caller's environment into the child's per-process registry
 * namespace before returning the pid — see __env_propagate_to_child,
 * defined further down with the rest of the OS1_env_* surface. */
static void __env_propagate_to_child(long child_pid);

long OS1low_process_spawn(const char *path, int argc, char *const argv[]) {
  long pid = _sys_spawn(path, argc, argv, 0);
  __env_propagate_to_child(pid);
  return pid;
}
/* Detached (SPAWN_FLAG_DETACHED, #193): the child does NOT inherit the spawner
 * as ctty — the nxexec launcher-mode.  See caps.h for the flag semantics. */
long OS1low_process_spawn_detached(const char *path, int argc,
                                   char *const argv[]) {
  long pid = _sys_spawn(path, argc, argv, SPAWN_FLAG_DETACHED);
  __env_propagate_to_child(pid);
  return pid;
}
/* Phase 4: spawn with fd redirection (shell `<`/`>`/`>>`/`2>`).  The redirect
 * targets must already be open in THIS process; the kernel dups them into the
 * child, so the caller closes its copies after we return. */
long OS1low_process_spawn_redir(const char *path, int argc, char *const argv[],
                                unsigned int flags,
                                const struct spawn_redir *redir, int nredir) {
  long pid = (nredir <= 0)
                 ? _sys_spawn(path, argc, argv, flags)
                 : _sys_spawn_redir(path, argc, argv, flags, redir, nredir);
  __env_propagate_to_child(pid);
  return pid;
}
long OS1low_process_spawn_caps(const char *path, int level,
                               unsigned long caps) {
  long pid = _sys_spawn_caps(path, level, caps, 0);
  __env_propagate_to_child(pid);
  return pid;
}
/* OS1low_pipe (Phase 4): anonymous byte pipe (OBJ_TYPE_PIPE).  fds[0]=read end,
 * fds[1]=write end.  The kernel installs both handles; read/write/close operate
 * on them like any other fd. */
int OS1low_pipe(int fds[2]) { return (int)errno_ret(_sys_pipe(fds)); }
/* POSIX pipe(): thin personality over OS1low_pipe (unistd.h). */
int pipe(int pipefd[2]) { return OS1low_pipe(pipefd); }
int OS1low_process_kill(int pid) { return _sys_kill(pid); }
/* OS1low_process_wait (F4 M4.5): wait via a PROCESS capability +
 * OS1_object_wait. A WAIT-only handle is acquirable for any live process
 * (wait-right is separable from kill-right); if the process is already gone,
 * acquisition fails and we fall back to the ambient SYS_WAIT so the legacy "not
 * found" (-2) result is preserved. */
/* __wait_encode - map the kernel's NEXS-neutral wait code to the POSIX status
 * word (Phase 2). code >= 0: voluntary exit -> exit code in bits 8..15
 * (WIFEXITED). code < 0: killed -> low byte = -code (WIFSIGNALED). */
static int __wait_encode(int code) {
  if (code < 0)
    return (-code) & 0x7f;
  return (code & 0xff) << 8;
}

int OS1low_process_wait(int pid) { return OS1low_process_wait_status(pid, 0); }

/* OS1low_process_wait_status - like OS1low_process_wait, but on reap writes the
 * process's raw exit_code to *code (Phase 2). Carried through the PROCESS
 * capability's OS1_object_wait(handle, &code); the ambient fallback has no
 * status channel, so *code stays 0 there. */
int OS1low_process_wait_status(int pid, int *code) {
  char idbuf[16];
  /* R5/R7: a 32-bit pid renders as at most 11 chars ("-2147483648") + NUL =
   * 12, well inside idbuf[16]; assert the fit instead of trusting it
   * silently, and check sprintf's own return (chars written, excluding
   * NUL) rather than discarding it. */
  int idlen = sprintf(idbuf, "%d", pid);
  nx_assert(idlen > 0 && (size_t)idlen < sizeof(idbuf));
  long h = OS1low_handle_create(OS1_NS_PROC, idbuf, OS1_RIGHT_WAIT,
                                OBJ_TYPE_PROCESS);
  if (h < 0) {
    /* No capability: the process is already GONE — which since Phase 9b is the
     * case where a retained status is precisely what we are after, so the
     * ambient path must carry it back rather than drop it. */
    return _sys_wait_status(pid, code);
  }
  long r = OS1_object_wait((int)h, (long)code);
  /* R7: close() failing here (an already-invalid handle) cannot change what
   * we return — the wait result in `r` is already committed — so the value
   * is intentionally discarded, not merely forgotten. */
  (void)OS1low_handle_close((int)h);
  return (int)r;
}
/* OS1_process_stop / _cont - job control (Phase 2). Acquire a PROCESS
 * capability with the control (DESTROY) right — the same authority as kill —
 * and issue OBJ_CTL_STOP/CONT. Returns 0 or a negative errno. */
static int os1_process_ctl(int pid, int cmd) {
  char idbuf[16];
  int idlen = sprintf(idbuf, "%d", pid);
  nx_assert(idlen > 0 && (size_t)idlen < sizeof(idbuf));
  long h = OS1low_handle_create(OS1_NS_PROC, idbuf, OS1_RIGHT_DESTROY,
                                OBJ_TYPE_PROCESS);
  if (h < 0)
    return (int)h;
  long r = OS1_object_ctl((int)h, cmd, 0);
  (void)OS1low_handle_close((int)h); /* R7: see rationale above */
  return (int)r;
}
int OS1_process_stop(int pid) { return os1_process_ctl(pid, OBJ_CTL_STOP); }
int OS1_process_cont(int pid) { return os1_process_ctl(pid, OBJ_CTL_CONT); }

void OS1low_process_yield(void) { _sys_yield(); }
int OS1low_process_self(void) { return _sys_get_pid(); }
void OS1low_process_exit(int status) {
  _sys_exit(status);
  while (1)
    ;
}

/* Bare-name compat shims (DIR-01). */
int get_pid(void) { return OS1low_process_self(); }
/* --- POSIX <unistd.h> personality (thin mapping onto the OS1 verbs above) ---
 */
/* getpid: the POSIX spelling of get_pid(). */
int getpid(void) { return get_pid(); }

/* Minimal Linux-compatible random API for GNU Coreutils and other ports.
 * The kernel does not expose a SYS_getrandom syscall in this tree yet, so the
 * libc layer synthesizes a non-blocking pseudo-random source from a tiny xorshift
 * state seeded from the OS1 clock and PID.  This is intentionally sufficient to
 * satisfy compile-time/test-time expectations and keep the userland port moving
 * without special-casing any applet. */
static unsigned long long os1_rand_state = 0;
static unsigned long long os1_rand_next(void) {
  unsigned long long x = os1_rand_state;
  if (x == 0) {
    x = (unsigned long long)OS1_time_now() ^ 0x9e3779b97f4a7c15ULL;
    x ^= (unsigned long long)getpid() << 13;
    x ^= (unsigned long long)os1_mono_ns();
  }
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  os1_rand_state = x;
  return x;
}

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
  (void)flags;
  if (!buf && buflen != 0) {
    errno = EFAULT;
    return -1;
  }
  unsigned char *p = (unsigned char *)buf;
  for (size_t i = 0; i < buflen; ++i) {
    if ((i & 7) == 0) {
      unsigned long long v = os1_rand_next();
      for (int j = 0; j < 8 && i + j < buflen; ++j)
        p[i + j] = (unsigned char)(v >> (8 * j));
      i += 7;
    }
  }
  return (ssize_t)buflen;
}

int getentropy(void *buffer, size_t length) {
  if (!buffer && length != 0) {
    errno = EFAULT;
    return -1;
  }
  if (length > 256) {
    errno = EINVAL;
    return -1;
  }
  ssize_t r = getrandom(buffer, length, 0);
  if (r < 0)
    return -1;
  return 0;
}

/* isatty: a descriptor is a terminal iff the object behind its handle is a
 * CONSOLE.  This is the REAL test via the capability type (OS1low_cap_query),
 * not the old "fd < 3" assumption: with shell redirection fd 1 may be a FILE
 * (`cmd > out`) or a PIPE (`cmd | cmd`), and interactive programs (the lua
 * REPL) must see isatty() == 0 in exactly those cases. */
int isatty(int fd) {
  long q = OS1low_cap_query(fd);
  if (q < 0) {
    errno = EBADF;
    return 0;
  }
  return OS1_CAPQ_TYPE(q) == OBJ_TYPE_CONSOLE ? 1 : 0;
}
#define MAX_ATEXIT 32
static void (*s_atexit_handlers[MAX_ATEXIT])(void);
static int s_atexit_count = 0;

int atexit(void (*function)(void)) {
  if (!function || s_atexit_count >= MAX_ATEXIT)
    return -1;
  s_atexit_handlers[s_atexit_count++] = function;
  return 0;
}

void exit(int status) {
  while (s_atexit_count > 0) {
    void (*fn)(void) = s_atexit_handlers[--s_atexit_count];
    if (fn)
      fn();
  }
  fflush(stdout);
  fflush(stderr);
  OS1low_process_exit(status);
}

void _Exit(int status) { OS1low_process_exit(status); }
void _exit(int status) { OS1low_process_exit(status); }

int spawn(const char *path) { return (int)OS1low_process_spawn(path, 0, 0); }
int spawn_args(const char *path, int argc, char *const argv[]) {
  return (int)OS1low_process_spawn(path, argc, argv);
}
/* spawn_caps: explicit capability mask; spawn_level: the level's default
 * preset (request CAP_ALL and let the kernel clamp to the level ceiling). */
long spawn_caps(const char *path, int level, unsigned long caps) {
  return OS1low_process_spawn_caps(path, level, caps);
}
long spawn_level(const char *path, int level) {
  return OS1low_process_spawn_caps(path, level, CAP_ALL);
}

/* Object / capability API (ASTRA §6.1/6.2) — thin veneers over the _sys_ stubs.
 * OS1 native base surface; POSIX layers on top of these, not vice versa. */
long OS1low_handle_create(int ns, const char *path, unsigned int rights,
                          int type) {
  return _sys_handle_create(ns, path, rights, type);
}
long OS1low_handle_duplicate(int handle, unsigned int new_rights) {
  return _sys_handle_dup(handle, new_rights);
}
long OS1low_handle_close(int handle) { return _sys_handle_close(handle); }
long OS1low_cap_query(int handle) { return _sys_cap_query(handle); }
long OS1low_cap_grant(int target_pid, int handle, unsigned int rights) {
  return _sys_cap_grant(target_pid, handle, rights);
}
long OS1_object_read(int handle, void *buf, unsigned long n) {
  return _sys_object_read(handle, buf, n);
}
long OS1_object_write(int handle, const void *buf, unsigned long n) {
  return _sys_object_write(handle, buf, n);
}
long OS1_object_wait(int handle, long arg) {
  return _sys_object_wait(handle, arg);
}

/* --- Service ports (ASTRA §6.5) -------------------------------------------
 * Thin personality over handle_create + object I/O: the port is the capability,
 * so there is no separate "port syscall" to add — acquiring the handle IS the
 * authority decision, and send/receive are ordinary object writes/reads.
 */
int OS1_port_create(const char *name) {
  /* RECEIVE right publishes the port and claims ownership; TRANSFER lets the
   * owner delegate send rights onward (cap_grant) without re-opening by name.
   */
  return (int)errno_ret(OS1low_handle_create(OS1_NS_PORT, name,
                                             OS1_RIGHT_READ | OS1_RIGHT_WRITE |
                                                 OS1_RIGHT_TRANSFER |
                                                 OS1_RIGHT_DUPLICATE,
                                             OBJ_TYPE_PORT));
}
int OS1_port_open(const char *name) {
  /* No READ: a client gets a SEND-only capability to an existing service. */
  return (int)errno_ret(OS1low_handle_create(
      OS1_NS_PORT, name, OS1_RIGHT_WRITE | OS1_RIGHT_DUPLICATE, OBJ_TYPE_PORT));
}
long OS1_port_send(int handle, const struct ipc_message *msg) {
  return errno_ret(OS1_object_write(handle, msg, sizeof(struct ipc_message)));
}
long OS1_port_send_caps(int handle, struct ipc_message *msg, const int *fds,
                        int nfds) {
  return errno_ret(_sys_port_send_caps(handle, msg, fds, nfds));
}
long OS1_port_recv(int handle, struct ipc_message *msg) {
  return errno_ret(OS1_object_read(handle, msg, sizeof(struct ipc_message)));
}
long OS1_object_ctl(int handle, int cmd, long arg) {
  return _sys_object_ctl(handle, cmd, arg);
}

/* Window manager surface (ASTRA §6.7: windows as objects).  Enumeration is a
 * direct read syscall; control goes through an OBJ_TYPE_WINDOW capability
 * (acquire → ctl → close), so authority is the unforgeable handle, not ambient
 * identity — an app drives its OWN window freely, a WM drives any window. */
long OS1_window_enum(struct window_info *buf, unsigned long max) {
  return _sys_window_enum(buf, max);
}

/* System statistics snapshot (perf §1).  Forwards to the SYS_SYSSTATS stub,
 * passing sizeof so the kernel can copy the prefix this build understands. */
long OS1_sys_stats(struct os1_sysstats *out) {
  if (!out)
    return -1;
  return _sys_sysstats(out, sizeof(*out));
}

/* __win_ctl - acquire a WINDOW capability with the rights a verb needs, issue
 * the control verb, then release the handle.  WRITE for minimize/restore/focus,
 * DESTROY for close. */
static int __win_ctl(int win_id, unsigned int rights, int cmd) {
  char idbuf[16];
  sprintf(idbuf, "%d", win_id);
  long h = _sys_handle_create(OS1_NS_WIN, idbuf, rights, OBJ_TYPE_WINDOW);
  if (h < 0)
    return (int)h;
  long r = _sys_object_ctl((int)h, cmd, 0);
  _sys_handle_close((int)h);
  return (int)r;
}
int OS1_window_minimize(int win_id) {
  return __win_ctl(win_id, OS1_RIGHT_WRITE, OBJ_CTL_MINIMIZE);
}
int OS1_window_restore(int win_id) {
  return __win_ctl(win_id, OS1_RIGHT_WRITE, OBJ_CTL_RESTORE);
}
int OS1_window_focus(int win_id) {
  return __win_ctl(win_id, OS1_RIGHT_READ, OBJ_CTL_FOCUS);
}
int OS1_window_close(int win_id) {
  return __win_ctl(win_id, OS1_RIGHT_DESTROY, OBJ_CTL_CLOSE);
}

/* Window / graphics canonical names (ASTRA §6.7, DIR-01 F4): thin veneers over
 * the _sys_ stubs.  The bare verbs
 * (create_window/destroy_window/window_draw/blit/
 * write/of_pid/grid/set_window_flags/set_focus/draw/flush/compositor_render)
 * are compat shims forwarding here. */
int OS1_window_create(int x, int y, int w, int h, const char *title) {
  return os1_video_window_create(x, y, w, h, title);
}
void OS1_window_destroy(int win_id) { os1_video_window_destroy(win_id); }
void OS1_window_draw(int win_id, int x, int y, int w, int h,
                     unsigned int color) {
  _sys_window_draw(win_id, x, y, w, h, color);
}
void OS1_window_blit(int win_id, int x, int y, int w, int h,
                     const unsigned int *buf) {
  (void)os1_video_present_argb8888(win_id, x, y, w, h, buf,
                                   (size_t)w * (size_t)h);
}
void OS1_window_write(int win_id, const char *buf, unsigned long count) {
  _sys_window_write(win_id, buf, count);
}
int OS1_window_of_pid(int pid) { return _sys_window_of_pid(pid); }
int OS1_window_grid(int win_id, int *cols, int *rows) {
  long r = _sys_window_grid(win_id);
  if (r < 0)
    return (int)r;
  if (cols)
    *cols = (int)((r >> 16) & 0xFFFF);
  if (rows)
    *rows = (int)(r & 0xFFFF);
  return 0;
}
void OS1_window_set_flags(int win_id, int flags) {
  _sys_window_set_flags(win_id, flags);
}
void OS1_window_set_focus(int pid) {
  extern void _sys_set_focus(int pid);
  _sys_set_focus(pid);
}
int OS1_window_resize(int win_id, int w, int h) {
  return os1_video_window_resize(win_id, w, h);
}
void OS1_gfx_draw(int x, int y, int w, int h, int color) {
  _sys_draw(x, y, w, h, color);
}
/* flush ≡ render: both just pushed the compositor.  Unified onto the single
 * SYS_COMPOSITOR_RENDER syscall (the duplicate SYS_FLUSH was retired). */
void OS1_gfx_flush(void) { os1_video_render(); }
void OS1_gfx_render(void) { os1_video_render(); }

/* Identity / privilege introspection (nxperm foundation): the caller's own
 * level + cap mask, unpacked from the (level<<16)|caps syscall return. */
int OS1_identity(int *level, unsigned int *mask) {
  long r = _sys_get_identity();
  if (level)
    *level = (int)((r >> 16) & 0xFF);
  if (mask)
    *mask = (unsigned int)(r & 0xFFFF);
  return 0;
}
int OS1_level(void) { return (int)((_sys_get_identity() >> 16) & 0xFF); }

int kill_process(int pid) { return OS1low_process_kill(pid); }
/* wait: maps to process_wait() in the kernel, which is NON-BLOCKING:
 * returns -1 if the process is alive, pid if reaped, -2 if not found. */
int wait(int pid) { return OS1low_process_wait(pid); }
/* Bare-name window/graphics compat shims (DIR-01 F4). */
void draw(int x, int y, int w, int h, int color) {
  OS1_gfx_draw(x, y, w, h, color);
}
void flush(void) { OS1_gfx_flush(); }
int create_window(int x, int y, int w, int h, const char *title) {
  return OS1_window_create(x, y, w, h, title);
}
void destroy_window(int win_id) { OS1_window_destroy(win_id); }
void window_draw(int win_id, int x, int y, int w, int h, unsigned int color) {
  OS1_window_draw(win_id, x, y, w, h, color);
}
void window_blit(int win_id, int x, int y, int w, int h,
                 const unsigned int *buf) {
  OS1_window_blit(win_id, x, y, w, h, buf);
}
void yield(void) { OS1low_process_yield(); }
/* OS1_sleep: block for `ms` milliseconds via the REAL kernel timer
 * (SYS_NANOSLEEP). The process is descheduled (no busy-wait) and woken by its
 * core's tick, so it no longer monopolises a core while idle. This is the NEXS
 * proprietary base API (milliseconds), deliberately distinct from POSIX
 * sleep(seconds); see <unistd.h>/<time.h> for the POSIX usleep/nanosleep. */
void OS1_sleep(int ms) {
  if (ms > 0)
    _sys_nanosleep((unsigned long long)ms * 1000000ULL);
}
/* usleep: POSIX microsecond sleep (real, blocking). Returns 0. */
int usleep(unsigned int usec) {
  _sys_nanosleep((unsigned long long)usec * 1000ULL);
  return 0;
}
void compositor_render(void) { OS1_gfx_render(); }
/* OS1low_ipc_*: canonical low-level IPC primitives (ASTRA §6.1); pid==-1 means
 * "any sender" in recv/try_recv.  The bare send/recv/try_recv are compat shims
 * (DIR-01 F4).  try_recv (SYS_TRY_RECV) is non-blocking: <0 if none waiting. */
long OS1low_ipc_send(int pid, struct ipc_message *msg) {
  return _sys_send(pid, msg);
}
long OS1low_ipc_recv(int pid, struct ipc_message *msg) {
  return _sys_recv(pid, msg);
}
long OS1low_ipc_try_recv(int pid, struct ipc_message *msg) {
  extern int _sys_try_recv(int pid, void *msg);
  return _sys_try_recv(pid, msg);
}
int send(int pid, struct ipc_message *msg) {
  return (int)OS1low_ipc_send(pid, msg);
}
int recv(int pid, struct ipc_message *msg) {
  return (int)OS1low_ipc_recv(pid, msg);
}
int try_recv(int pid, struct ipc_message *msg) {
  return (int)OS1low_ipc_try_recv(pid, msg);
}
void set_window_flags(int win_id, int flags) {
  OS1_window_set_flags(win_id, flags);
}
void set_focus(int pid) { OS1_window_set_focus(pid); }

/* --- Shared Implementations (from kernel library) ---
 * NOTE(USR-LIB-01): These are direct source-level #includes of kernel
 * internal implementation files, not headers.  Changes to kernel/lib C files
 * silently affect userland behaviour with no compile-time boundary check.
 * vsnprintf.c provides vsnprintf/vsscanf; math.c provides fixed-point trig
 * and DEG_TO_FP_RAD/cos_fp/sin_fp/fixmul used by demo3d; string.c provides
 * memset/memcpy/strlen/strcmp/strncmp/strchr etc. */
/* math functions now in user/sys/lib/math.c (IEEE-754 float/double) */
#include "common/string.c"
#include "common/vsnprintf.c"
#include "font_lib.c"

static struct font_ctx *graphics_default_font;
static int graphics_default_font_attempted;

static struct font_ctx *graphics_get_default_font(void) {
  if (!graphics_default_font && !graphics_default_font_attempted) {
    graphics_default_font_attempted = 1;
    graphics_default_font = font_load("/fonts/Rewir-Light.off");
  }
  return graphics_default_font;
}

/* --- Stack protector support ---
 * __stack_chk_guard: canary value written by the compiler before local arrays
 * on functions compiled with -fstack-protector.  The value is a fixed constant
 * rather than a runtime random seed, weakening its effectiveness against local
 * attacks, but it is sufficient for a debug/development build.
 * __stack_chk_fail: called when the canary is clobbered; prints a message and
 * exits.  Must not call any function that itself uses stack protectors to avoid
 * infinite recursion. */
uintptr_t __stack_chk_guard = 0x595e9eda;
void __stack_chk_fail(void) {
  printf("Stack smashing detected!\n");
  exit(1);
}

/* ==========================================================================
 * MODULE 3 — Registry wrappers and the ENVIRONMENT personality
 * (R1-R10 applied to this module; see the file header's compliance policy)
 *
 * Microkernel note (per your correction): OS1_registry_set() below is the
 * concrete example of the pattern you're pointing at — a registry-write
 * denial is NOT handled by the kernel deciding what a user should be told.
 * The kernel enforces CAP_REG_WRITE and returns -EACCES synchronously; THIS
 * function, in userland, is what turns that into a visible warning, by
 * sending an ordinary notify() IPC message that nxntfy_srv (a userland
 * service under sys/bin/, exactly like nxexec for spawn) picks up on its own
 * blocking recv() and nxbar renders. The kernel never learns notifications
 * exist. Every later module that touches a "the kernel doesn't do X" gap
 * gets checked against this same question first: is X actually a service
 * under sys/bin/nx* already, or genuinely missing.
 *
 * op=0 read 'key' into buf; op=1 write 'key' (kernel-side CAP_REG_WRITE +
 * first-writer-wins ownership); op=2 enumerate, optionally under a prefix.
 * ========================================================================== */
int OS1_registry_get(const char *key, char *buf, size_t size) {
  return (int)_sys_registry(0, key, buf, size);
}
int OS1_registry_set(const char *key, const char *value) {
  /* R7/defensive: every current caller in this tree passes a real string
   * (verified against the whole source bundle, not assumed) — value is
   * ALWAYS a literal or a buffer that was itself checked — but this is a
   * public entry point another module can call incorrectly tomorrow, and
   * `strlen(NULL)` below would fault the whole process for a one-line
   * omission at the call site. Fail the write instead of crashing it. */
  if (!key || !value) {
    errno = EFAULT;
    return -EFAULT;
  }
  int rc = (int)_sys_registry(1, key, (char *)value, strlen(value));
  /* The kernel only ever pr_warn/UART-logs an owner-mismatch denial — it
   * must not know about notifications, IPC to a specific service, or any
   * other userland concept (kernel/userland separation).  The CALLER
   * already gets -EACCES back synchronously right here, so it is the one
   * that reports on itself, through the ordinary notify() transport every
   * other warning already uses (no kernel involvement, no polling
   * anywhere: nxntfy_srv picks it up via its existing blocking recv(),
   * nxbar's already-per-frame ring read picks it up from there). */
  if (rc == -EACCES)
    OS1_notify_warn("registry", key);
  return rc;
}
int OS1_registry_enum(char *buf, size_t size) {
  return (int)_sys_registry(2, 0, buf, size);
}
/* OS1_registry_enum_under (Phase 4.1 A1a): list only keys under 'prefix'. */
int OS1_registry_enum_under(const char *prefix, char *buf, size_t size) {
  return (int)_sys_registry(2, prefix, buf, size);
}
/* OS1_registry_del (Phase 4.1 A-gap1): remove a key. */
int OS1_registry_del(const char *key) {
  return (int)_sys_registry(3, key, 0, 0);
}

/*
 * set_font - transfer a packed font buffer to the kernel (SYS_SET_FONT #253).
 *
 * data: pointer to a [ font_header ][ glyph_info * n ][ bitmap ] buffer.
 * size: total byte count of that buffer.
 *
 * NOTE(USR-FONTMAN-01): The kernel stores 'data' as a raw pointer; the caller
 * must keep the buffer alive indefinitely (nxfont uses while(1) yield()).
 */
/* Display / compositor control (ASTRA §6.7): canonical OS1_display_* over the
 * raw _sys_ stubs.  set_font keeps a bare shim; the others had no bare name. */
long OS1_display_info(void) { return _sys_display_info(); }
int OS1_display_set_mode(int w, int h) { return _sys_set_display_mode(w, h); }
int OS1_display_poll(void) { return _sys_display_poll(); }
int OS1_display_set_style(int style_id, int theme_id) {
  return _sys_set_style(style_id, theme_id, -1);
}
int OS1_display_set_background(int bg_id) {
  return _sys_set_style(-1, -1, bg_id);
}
int OS1_display_set_zoom(int percent) { return _sys_set_zoom(percent); }
int OS1_display_set_font(void *data, size_t size) {
  extern int _sys_set_font(void *data, size_t size);
  return _sys_set_font(data, size);
}
int set_font(void *data, size_t size) {
  return OS1_display_set_font(data, size);
} /* compat shim (DIR-01 F4) */
/* file_read: buf==NULL / size==0 returns the file size without reading data;
 * used by fopen() to probe file size before allocating a read buffer. */
/* OS1_fs_ functions: canonical (ASTRA §6.3); the bare file_write/file_read/
 * list_dir/chdir/getcwd below are compat shims (DIR-01 F4). */
/* OS1_fs_write (M4.5-FS-WRITE resolved): data writes routed through a FILE
 * capability, mirroring OS1_fs_read — handle_create(FS, WRITE|CREATE) →
 * OBJ_CTL_SEEK(offset) → object_write → close.  OS1_RIGHT_CREATE gives
 * open(O_CREAT) semantics (a missing file is created behind the same
 * vfs_write_allowed seam the ambient path used), so creation keeps working;
 * a size==0 call is the create/truncate-empty idiom: the creation already
 * happened at handle_create, nothing to write. */
int OS1_fs_write(const char *path, const void *buf, int size, int offset) {
  if (size < 0) {
    errno = EINVAL;
    return -EINVAL;
  }
  long h = OS1low_handle_create(
      OS1_NS_FS, path, OS1_RIGHT_WRITE | OS1_RIGHT_CREATE, OBJ_TYPE_FILE);
  if (h < 0) {
    errno = (int)-h;
    OS1_report_error(path,
                     (int)h); /* surface EACCES/EIO; ENOENT stays silent */
    return (int)h;
  }
  long w = 0;
  if (size > 0) {
    if (offset > 0)
      OS1_object_ctl((int)h, OBJ_CTL_SEEK, offset);
    w = OS1_object_write((int)h, buf, (unsigned long)size);
  }
  OS1low_handle_close((int)h);
  if (w < 0) {
    errno = (int)-w;
    OS1_report_error(path, (int)w);
  }
  return (int)w;
}
/* OS1_fs_read (F4 M4.5; ambient fallback removed by R1): data reads routed
 * through a FILE capability — handle_create(FS,READ) -> OBJ_CTL_SEEK(offset) ->
 * object_read -> close.
 *
 * The size<=0 / NULL-buf call is a metadata size-probe.  It used to fall back
 * to the ambient SYS_FILE_READ verb "because the object read does not do that"
 * — but OBJ_CTL_STAT does, and always did: the object layer was never missing
 * the capability, only this caller was not asking for it.  Probing through the
 * handle removes the last libc user of the ambient path (Programme R1) and, in
 * passing, makes the probe capability-checked like every other read. */
int OS1_fs_read(const char *path, void *buf, int size, int offset) {
  /* EXIT SEMANTICS ARE PRESERVED EXACTLY.  The old guard was `size <= 0 ||
   * !buf` funnelling into the ambient verb, but the KERNEL branched only on
   * `size`, so the one guard covered three different outcomes and they must
   * stay distinct:
   *
   *   size < 0            (size_t)size wrapped to a huge value, tripping the
   *                       SYSCALL_MAX_IO_BYTES check       -> -EINVAL
   *   size > 0, buf NULL  the kernel read, then copy_to_user(NULL) failed
   *                                                        -> -EFAULT
   *   size == 0           probe: vfs_read_file(path,NULL,0) -> file size,
   *                       any failure reported as          -> -ENOENT
   *
   * Collapsing all three onto "return the size" would have made a negative
   * size and a NULL destination look like successful probes. */
  if (size < 0) {
    errno = EINVAL;
    return -EINVAL;
  }
  if (size > 0 && !buf) {
    errno = EFAULT;
    return -EFAULT;
  }
  if (size == 0) {
    /* THE SIZE-PROBE STANDARD (made explicit here, R1).
     *
     * `size == 0` asks "how big is this file".  That is a STAT question, and a
     * stat has no offset — so the offset argument is meaningless for a probe.
     * The old implementation silently ignored it and returned the total size,
     * which means a caller asking "how many bytes remain after `offset`" got
     * the whole size and no indication it had asked something the API does not
     * answer.  This refuses instead, following the rule this project already
     * applies to the environment ceilings: a silently different value is worse
     * than a failure, because the failure is diagnosable.
     *
     * Verified before enforcing: all 12 probe call sites in the tree pass
     * offset 0 (fopen, font_lib, nxfilem, nxlauncher, rename/truncate,
     * fdtest/writetest/capreg), so no application changes — the standard was
     * already universally observed, it just was not stated or enforced.
     *
     * Probe through the object layer: OBJ_CTL_STAT is exactly "current size in
     * bytes" and resolves it from the same place the ambient verb did
     * (vfs_stat, falling back to the node size), so the value is identical.
     * Read acquisition is ungated (the tree ACL gates writes), so a file
     * readable before is readable now — the probe is simply capability-routed
     * like every other read.  Failure stays -ENOENT to keep the old contract;
     * handle_create knows more (-EACCES, …) but changing what callers observe
     * is a separate decision, not a side effect of this refactor. */
    if (offset != 0) {
      errno = EINVAL;
      return -EINVAL;
    }
    /* ONE syscall.  The first R1 version did handle_create + OBJ_CTL_STAT +
     * close — three syscalls and two path resolutions for one number, on the
     * path every fopen() takes.  SYS_STAT asks the same VFS once. */
    struct abi_stat as;
    if (_sys_stat(path, &as) != 0) {
      errno = ENOENT;
      return -ENOENT;
    }
    return (int)as.size;
  }
  long h = OS1low_handle_create(OS1_NS_FS, path, OS1_RIGHT_READ, OBJ_TYPE_FILE);
  if (h < 0) {
    errno = (int)-h;
    return (int)h;
  }
  if (offset > 0)
    OS1_object_ctl((int)h, OBJ_CTL_SEEK, offset);
  long r = OS1_object_read((int)h, buf, (unsigned long)size);
  OS1low_handle_close((int)h);
  if (r < 0)
    errno = (int)-r;
  return (int)r;
}
/* OS1_fs_list (R1): a directory is a file you READ.  handle_create(FS,READ) ->
 * object_read -> close, the same shape as OS1_fs_read — so listing goes through
 * the capability layer like every other read instead of the ambient
 * SYS_LIST_DIR verb.
 *
 * Exit semantics preserved: the ambient verb returned the listing LENGTH and
 * -ENOENT for a missing/unreadable path, so acquisition failure maps to
 * -ENOENT rather than surfacing handle_create's own errno.  The listing is
 * NUL-terminated for callers that treat it as a string (the ambient path
 * copied res+1 bytes for exactly that reason). */
int OS1_fs_list(const char *path, char *buf, size_t size) {
  if (!buf || size == 0) {
    errno = EINVAL;
    return -EINVAL;
  }
  long h = OS1low_handle_create(OS1_NS_FS, path, OS1_RIGHT_READ, OBJ_TYPE_FILE);
  if (h < 0) {
    errno = ENOENT;
    return -ENOENT;
  }
  long r = OS1_object_read((int)h, buf, size - 1);
  OS1low_handle_close((int)h);
  if (r < 0) {
    errno = ENOENT;
    return -ENOENT;
  }
  buf[r] = '\0';
  return (int)r;
}
int OS1_fs_chdir(const char *path) { return _sys_chdir(path); }
int OS1_fs_getcwd(char *buf, size_t size) { return _sys_getcwd(buf, size); }

/* __fs_parent_ctl - split a pathname into a parent directory and one child
 * name, acquire the parent with the distinct MUTATE right, then send the
 * namespace operation through that capability. The kernel repeats the
 * one-component validation: this split is composition convenience, not a
 * security boundary. */
static int __fs_parent_ctl(const char *path, int cmd) {
  if (!path)
    return -EFAULT;

  char work[128];
  size_t len = strlen(path);
  if (len == 0 || len >= sizeof(work))
    return -EINVAL;
  memcpy(work, path, len + 1);
  while (len > 1 && work[len - 1] == '/')
    work[--len] = '\0';
  if (strcmp(work, "/") == 0)
    return -EINVAL;

  char *leaf = work + len;
  while (leaf > work && leaf[-1] != '/')
    leaf--;
  const char *parent;
  if (leaf == work) {
    parent = ".";
  } else if (leaf == work + 1 && work[0] == '/') {
    parent = "/";
  } else {
    leaf[-1] = '\0';
    parent = work;
  }
  if (!leaf[0])
    return -EINVAL;

  long h =
      OS1low_handle_create(OS1_NS_FS, parent, OS1_RIGHT_MUTATE, OBJ_TYPE_FILE);
  if (h < 0)
    return (int)h;
  long r = OS1_object_ctl((int)h, cmd, (long)leaf);
  OS1low_handle_close((int)h);
  return (int)r;
}

int OS1_fs_mkdir(const char *path) {
  return __fs_parent_ctl(path, OBJ_CTL_MKDIR);
}
int OS1_fs_unlink(const char *path) {
  return __fs_parent_ctl(path, OBJ_CTL_UNLINK);
}
int file_write(const char *path, const void *buf, int size, int offset) {
  return OS1_fs_write(path, buf, size, offset);
}
int file_read(const char *path, void *buf, int size, int offset) {
  return OS1_fs_read(path, buf, size, offset);
}
int list_dir(const char *path, char *buf, size_t size) {
  return OS1_fs_list(path, buf, size);
}
int chdir(const char *path) { return OS1_fs_chdir(path); }
int getcwd(char *buf, size_t size) { return OS1_fs_getcwd(buf, size); }

/*
 * open - POSIX fd open (ABI-03 fd table).
 *
 * The create/truncate/append semantics live in the kernel now: SYS_OPEN
 * honours O_CREAT/O_TRUNC through the VFS write-ACL + create seam (ASTRA §6.8,
 * kernel/core/syscall_dispatch.c).  libc keeps only the POSIX personality
 * work: pass the flags straight through, map the negative errno (errno_ret),
 * surface a protected-path denial on the standard notify() transport, and set
 * the initial position for O_APPEND (a fresh handle starts at offset 0, and
 * the kernel does not track per-write append).  The variadic mode argument is
 * accepted but not applied (the ext4 driver fixes new-file perms).
 */
int open(const char *pathname, int flags, ...) {
  int fd = (int)_sys_open(pathname, flags);
  if (fd < 0) {
    /* Uniform surfacing (Phase 0): amber on EACCES, red on a hard fault,
     * silent on an ENOENT probe — the policy lives in OS1_report_error, not
     * here, so every open() caller and every portability layer behave alike.
     * The report now happens INSIDE the errno seam (see errno_ret_ctx); this
     * site passes the path so the notification still names the file rather
     * than the generic "libc", and reports ONCE instead of twice. */
    return (int)errno_ret_ctx(fd, pathname);
  }
  if (flags & O_APPEND)
    _sys_lseek(fd, 0, SEEK_END); /* best-effort: initial position at EOF */
  return fd;
}
int close(int fd) { return (int)errno_ret(_sys_close(fd)); }
long lseek(int fd, long offset, int whence) {
  return errno_ret(_sys_lseek(fd, offset, whence));
}

/* ==========================================================================
 * MODULE 5 — Formatting: the printf family
 * (R1-R10 applied to this module; see the file header's compliance policy)
 *
 * All formatting functions delegate to vsnprintf() from kernel/lib/
 * vsnprintf.c (included above; USR-LIB-01, still open — see the file
 * header — kept out of scope for this file). Output goes to fd 1 (the
 * shell/window TTY) via write() unless directed elsewhere (printf_win,
 * fprintf/vfprintf's `stream`).
 *
 * printf/printf_win/vfprintf: each uses a fixed stack buffer (256/512/1024
 * bytes respectively) as the FAST path — the overwhelming majority of calls
 * fit it, so R3 (no heap traffic after start-up) holds for normal use.
 * VERIFIED FIX: this comment used to say output past the buffer was
 * "silently truncated" — that was true of the code at the time, and is no
 * longer true: all three now reformat into an exactly-sized heap buffer
 * (the same two-pass technique vasprintf() below already used) instead of
 * cutting the output, so the R3 exception is scoped to the rare case that
 * needs it, not a standing correctness bug.
 *
 * vsprintf/sprintf: pass 65536 as the size limit — effectively unbounded.
 * Callers are responsible for providing a large enough destination buffer;
 * overflow is not detected. This matches the real, historically-unsafe
 * POSIX sprintf() contract (same reason %s without a width is unbounded in
 * vsscanf() further below) rather than being a bug specific to this file —
 * kept as documented behaviour, not "fixed" into a different contract that
 * real sprintf() callers wouldn't expect.
 *
 * print_hex: renders a 64-bit value as 18-char "0xHHHHHHHHHHHHHHHH" string
 * written directly via write(), bypassing the format engine.
 */
int vsprintf(char *out, const char *fmt, va_list args) {
  return vsnprintf(out, 65536, fmt, args);
}
int printf(const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  int res = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (res < 0)
    return res;
  if ((size_t)res < sizeof(buf)) {
    /* Fast path: the overwhelming majority of printf() calls in this tree
     * format well under 256 chars, so this is the ONLY path most callers
     * ever take — zero heap traffic, matching R3. */
    write(1, buf, (size_t)res);
    return res;
  }
  /* R7/correctness: `res` is the length the format WOULD need — vsnprintf
   * already silently truncated `buf` to fit. Previously this function wrote
   * exactly that truncated buffer and called it done, silently dropping
   * everything past byte 255. Reformat once more into a buffer sized
   * exactly for the real output, the same two-pass technique vasprintf()
   * above already uses for the same reason, so a long printf() behaves like
   * every other printf() in this file instead of a special case. */
  char *big = malloc((size_t)res + 1U);
  if (!big) {
    /* Out of memory: better a truncated line than a silently swallowed one
     * — this only degrades to today's old behaviour, it never regresses. */
    write(1, buf, strlen(buf));
    return res;
  }
  va_start(args, fmt);
  int n2 = vsnprintf(big, (size_t)res + 1U, fmt, args);
  va_end(args);
  /* R5: reformatting the SAME fmt/args twice must produce the SAME length;
   * if it doesn't, `args` had a side effect between the two passes (e.g. a
   * %n or a volatile read), which is caller misuse this assertion surfaces
   * instead of writing a mismatched byte count. */
  nx_assert(n2 == res);
  write(1, big, (size_t)(n2 > 0 ? n2 : 0));
  free(big);
  return res;
}
void window_write(int win_id, const char *buf, unsigned long count) {
  OS1_window_write(win_id, buf, count);
}
int window_of_pid(int pid) { return OS1_window_of_pid(pid); }
int window_grid(int win_id, int *cols, int *rows) {
  return OS1_window_grid(win_id, cols, rows);
}
void printf_win(int win_id, const char *fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  int res = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (res < 0)
    return;
  if ((size_t)res < sizeof(buf)) {
    _sys_window_write(win_id, buf, (size_t)res);
    return;
  }
  /* R7: same truncation fix as printf() above, same rationale — a window
   * that prints one long wrapped paragraph used to lose everything past
   * byte 511 with no indication anything was cut. */
  char *big = malloc((size_t)res + 1U);
  if (!big) {
    _sys_window_write(win_id, buf, strlen(buf));
    return;
  }
  va_start(args, fmt);
  int n2 = vsnprintf(big, (size_t)res + 1U, fmt, args);
  va_end(args);
  nx_assert(n2 == res); /* R5: see printf()'s identical assertion */
  _sys_window_write(win_id, big, (size_t)(n2 > 0 ? n2 : 0));
  free(big);
}
int sprintf(char *out, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int res = vsnprintf(out, 65536, fmt, args);
  va_end(args);
  return res;
}
int snprintf(char *out, size_t size, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int res = vsnprintf(out, size, fmt, args);
  va_end(args);
  return res;
}
int asprintf(char **strp, const char *fmt, ...) {
  if (!strp)
    return -1;
  va_list args;
  va_start(args, fmt);
  int res = vasprintf(strp, fmt, args);
  va_end(args);
  return res;
}
int vasprintf(char **strp, const char *fmt, va_list ap) {
  if (!strp || !fmt)
    return -1;

  va_list ap2;
  va_copy(ap2, ap);
  int needed = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (needed < 0)
    return -1;

  char *buf = malloc((size_t)needed + 1U);
  if (!buf)
    return -1;

  int written = vsnprintf(buf, (size_t)needed + 1U, fmt, ap);
  if (written < 0) {
    free(buf);
    return -1;
  }

  *strp = buf;
  return written;
}

ptrdiff_t vaszprintf(char **resultp, const char *format, va_list args) {
  if (!resultp || !format)
    return -1;

  va_list args2;
  va_copy(args2, args);
  int needed = vsnprintf(NULL, 0, format, args2);
  va_end(args2);
  if (needed < 0)
    return -1;

  char *buf = malloc((size_t)needed + 1U);
  if (!buf)
    return -1;

  int written = vsnprintf(buf, (size_t)needed + 1U, format, args);
  if (written < 0) {
    free(buf);
    return -1;
  }

  *resultp = buf;
  return (ptrdiff_t)written;
}
void print(const char *s) { write(1, s, strlen(s)); }
/* print_hex: manual 16-nibble hex formatter for a 64-bit value. */
void print_hex(unsigned long val) {
  char buf[18];
  buf[0] = '0';
  buf[1] = 'x';
  for (int i = 0; i < 16; i++) {
    int digit = (val >> ((15 - i) * 4)) & 0xF;
    buf[2 + i] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10);
  }
  write(1, buf, 18);
}

/* --- Standard IO ---
 * getchar: blocking single-char read from fd 0 (keyboard).
 * putchar: writes one character to fd 1 (TTY/window).
 * gets: line-buffered input with backspace handling; size-bounded to avoid
 *   overflow (stops at size-1 chars).  Echoes characters to fd 1 and handles
 *   \b/DEL with the terminal backspace-space-backspace sequence.
 */
int getchar(void) {
  char c;
  if (read(0, &c, 1) == 1)
    return (unsigned char)c;
  return -1;
}
int putchar(int c) {
  char ch = (char)c;
  write(1, &ch, 1);
  return c;
}
char *gets(char *s, int size) {
  int i = 0;
  while (i < size - 1) {
    int c = getchar();
    if (c < 0)
      break;
    if (c == '\b' || c == 127) {
      if (i > 0) {
        i--;
        write(1, "\b \b", 3);
      }
      continue;
    }
    putchar(c);
    if (c == '\n' || c == '\r') {
      s[i] = '\0';
      return s;
    }
    s[i++] = (char)c;
  }
  s[i] = '\0';
  return s;
}

/*
 * notify - send a system notification via IPC to the notification server.
 *
 * title: short label (up to 30 chars copied); truncated silently if longer.
 * msg:   message body (up to 33 remaining chars after "title: "); truncated.
 *
 * The payload is assembled as "title: msg\0" into imsg.payload[64].
 * The target PID is read from the global registry key "srv.notify_pid".
 * Falls back to PID 2 if the key is absent (pid=2 is the expected notify_srv
 * PID under the current fixed-order spawn sequence in init.c).
 *
 * NOTE(USR-SEC-01): registry_read("srv.notify_pid", ...) has no authentication;
 * any process can overwrite that key to redirect all notifications to itself,
 * effectively hijacking the system notification channel.
 *
 * Returns the result of send() (0 on success, negative on failure).
 */
/* notify_send - post a notification with a severity (0=info, 1=warning/yellow,
 * 2=error/red) in data1, which nxntfy_srv renders as the popup colour. */
static int notify_send(const char *title, const char *msg, int sev) {
  struct ipc_message imsg;
  imsg.type = IPC_TYPE_NOTIFY;
  imsg.data1 = (uint64_t)sev;
  imsg.data2 = 0;
  int i = 0;
  /* Pack "title: msg" into the 64-byte payload field.
   * 30-char limit for title leaves room for ": " and at least 32 msg chars. */
  while (*title && i < 30)
    imsg.payload[i++] = *title++;
  imsg.payload[i++] = ':';
  imsg.payload[i++] = ' ';
  while (*msg && i < 63)
    imsg.payload[i++] = *msg++;
  imsg.payload[i] = '\0';
  /* Resolve the notify_srv endpoint from the registry. If the key is absent
   * (notify_srv not up / not yet registered) FAIL instead of sending to a
   * guessed PID — the old pid=2 fallback delivered a stray IPC message to
   * whatever process happened to hold PID 2 and the notification was lost
   * anyway. Callers that need the boot popup wait for the key (see init.c). */
  char pid_buf[16];
  if (OS1_registry_get("srv.notify_pid", pid_buf, sizeof(pid_buf)) != 0)
    return -1;
  int pid = atoi(pid_buf);
  if (pid <= 0)
    return -1;
  return (int)OS1low_ipc_send(pid, &imsg);
}
int OS1_notify_post(const char *title, const char *msg) {
  return notify_send(title, msg, 0);
}
int OS1_notify_warn(const char *title, const char *msg) {
  return notify_send(title, msg, 1);
} /* yellow */
int OS1_notify_error(const char *title, const char *msg) {
  return notify_send(title, msg, 2);
} /* red — previously only the kernel crash handler (fault.c) could reach
   * severity 2 (m.data1 = 2, kernel_ipc_send bypassing this function
   * entirely); userland callers can now post one directly too. */
int notify(const char *title, const char *msg) {
  return notify_send(title, msg, 0);
} /* compat shim (DIR-01 F4) */

/* --- Doom/LibC Compatibility ---
 * FILE emulation: a FILE* is a heap-allocated struct (defined in os1.h) that
 * records the file path, current byte position, error/eof flags, and cached
 * size. VERIFIED against the code below, replacing a stale claim this
 * comment used to make ("all I/O is synchronous and unbuffered, every
 * fread/fwrite maps directly to a syscall"): a path-backed FILE keeps a
 * REAL fd open for its lifetime (see fopen below) and both directions are
 * buffered — fread through fp->rbuf (FILE_RBUF_SIZE) and fwrite through
 * fp->wbuf (FILE_WBUF_SIZE) — specifically because ext4 does a whole-block
 * read-modify-write per partial access, so unbuffered byte-at-a-time I/O
 * (as this comment used to describe) was the actual cause of the
 * multi-minute doom savegame load/save this file's other comments
 * reference. Only CONSOLE/pipe streams (no backing path) stay unbuffered,
 * because interactive output must appear immediately.
 * ==========================================================================
 * MODULE 4 — Buffered stdio (fopen/fread/fwrite/fseek/fflush/fgets/...)
 * (R1-R10 applied to this module; see the file header's compliance policy)
 * ========================================================================== */

/*
 * fopen - open a file for buffered I/O emulation.
 *
 * Allocates a FILE struct, stores the path, probes the file size via
 * file_read(path, NULL, 0, 0).  SIZE-PROBE CONVENTION: the trigger is
 * `size == 0` (NOT `buf == NULL`, which this comment used to claim — the
 * distinction matters because `buf == NULL` with a NON-zero size is an error,
 * -EFAULT, not a probe).  `offset` must be 0: a size has no offset, and passing
 * one is refused with -EINVAL rather than silently answered with the total.
 * Returns NULL if the file does not exist and mode is "r" (read-only).
 * Write modes ("w", "a") do not fail on missing files — file_write will
 * create them on demand via the kernel VFS.
 */
FILE *fopen(const char *path, const char *mode) {
  FILE *f = malloc(sizeof(FILE));
  if (!f) {
    errno = ENOMEM;
    return NULL;
  }
  /* Zero EVERY field: malloc reuses dirty blocks, and a stale has_ungetc
   * injected a ghost byte at the start of the first fread (doom read
   * "\0IWA" instead of "IWAD" after its 9-file IWAD probe loop), while a
   * stale is_tmp made fclose() unlink real files.  memset is the whole
   * fix; pos/error/eof/has_ungetc/is_tmp start 0 by definition. */
  memset(f, 0, sizeof(FILE));
  f->fd = -1;
  strncpy(f->path, path, sizeof(f->path) - 1);
  /* Probe file size; file_read with NULL buf and size=0 returns byte count.
   * On a miss it sets errno (ENOENT/EACCES) — preserved below so a failed
   * fopen reports the real reason (luaL_fileresult/strerror), not "Success". */
  f->size = file_read(path, NULL, 0, 0);
  if (f->size < 0 && mode[0] == 'r') {
    int e = errno;
    free(f);
    errno = e; /* free() must not mask the open failure's errno */
    return NULL;
  }
  /* Write mode truncates to empty NOW (POSIX "w"): unlink + zero-byte create,
   * so the file both exists and is emptied before any fwrite — the old lazy
   * path left stale trailing bytes when overwriting a longer file. Append
   * mode ("a") creates-if-missing and starts the position at EOF. */
  if (mode[0] == 'w') {
    /* Create-and-truncate in one call: a from-start write (offset 0) now
     * truncates on the FS side (ext4_write), so a zero-byte write both
     * materialises a missing file and empties an existing one — no unlink
     * needed, and the inode (perms/links) is preserved instead of churned. */
    if (OS1_fs_write(path, "", 0, 0) < 0) {
      int e = errno;
      free(f);
      errno = e;
      return NULL;
    }
    f->size = 0;
  } else if (mode[0] == 'a') {
    if (f->size < 0) {
      if (OS1_fs_write(path, "", 0, 0) < 0) {
        int e = errno;
        free(f);
        errno = e;
        return NULL;
      }
      f->size = 0;
    }
    f->pos = f->size;
  }

  /*
   * Open a REAL handle and keep it for the stream's lifetime.
   *
   * Before this, a path-backed FILE carried fd = -1 and EVERY fread/fwrite did
   * handle_create + seek + read/write + close — four syscalls and, far worse, a
   * full VFS PATH RESOLUTION per call.  doom reads a savegame a byte at a time
   * (saveg_read8), so a ~100 KB save meant ~100 000 path resolutions: that is
   * the multi-minute load, and it is the "positional per-byte fread through the
   * FILE layer + handle-per-call" item the plan logged at 17d.
   *
   * Now the handle is opened once and the KERNEL owns the offset.  fread/fwrite
   * are one syscall with no resolution; fseek moves the kernel offset with
   * lseek.  fp->pos is kept as a mirror so ftell costs nothing.
   *
   * A failure here is NOT fatal: the stream falls back to the positional path
   * (fd stays -1), which still works.  Losing speed is acceptable; losing the
   * ability to open a file is not — and the write modes above have already
   * created/truncated, so the file exists either way. */
  {
    int oflags;
    if (mode[0] == 'r')
      oflags =
          (mode[1] == '+' || (mode[1] && mode[2] == '+')) ? O_RDWR : O_RDONLY;
    else
      oflags = O_RDWR; /* "w"/"a" already created+positioned above */
    int h = open(path, oflags);
    if (h >= 0) {
      f->fd = h;
      /* Align the kernel offset with the logical one ("a" starts at EOF). */
      if (f->pos != 0)
        lseek(h, f->pos, SEEK_SET);
    }
  }
  return f;
}

FILE *freopen(const char *filename, const char *mode, FILE *stream) {
  if (stream) {
    fclose(stream);
  }
  return fopen(filename, mode);
}

static int file_fd(FILE *fp);

/*
 * fstream_flush - write out the pending write buffer of a positional stream.
 *
 * The buffered bytes' on-disk offset is (pos - wcount): fwrite advances the
 * logical position as it accumulates, so the buffer start trails 'pos' by the
 * pending count.  A no-op for read streams and the console std streams, whose
 * wcount stays 0.  On a short/failed write the error flag is set.  Returns 0 on
 * success, EOF on error.
 */
static int fstream_flush(FILE *fp) {
  if (!fp || fp->wcount <= 0)
    return 0;
  int at = fp->pos - fp->wcount;
  if (at < 0)
    at = 0;
  int pending = fp->wcount;
  int w;
  if (fp->fd >= 0) {
    /* Flush through the stream's own handle: one syscall, no path resolution.
     * lseek first because the buffer's on-disk offset trails the logical
     * position by the pending count, and interleaved reads may have moved the
     * kernel offset since. */
    lseek(fp->fd, at, SEEK_SET);
    long r = write(fp->fd, fp->wbuf, (unsigned long)pending);
    w = (int)r;
    if (r > 0)
      lseek(fp->fd, fp->pos, SEEK_SET); /* restore the logical position */
  } else {
    w = file_write(fp->path, fp->wbuf, pending, at);
  }
  fp->wcount = 0;
  /* The file just changed: any cached read window may now be stale. */
  fp->rcount = 0;
  fp->rhead = 0;
  if (w < 0 || w < pending) {
    fp->error = 1;
    return EOF;
  }
  return 0;
}

/*
 * fclose - flush any pending writes, then release a FILE handle.
 */
/*
 * fdopen - wrap an ALREADY-OPEN descriptor in a FILE* (POSIX).  The stream is
 * fd-backed: fread/fwrite go straight through read()/write() on that fd, so it
 * works for any object the descriptor names — a file, a console, or a PIPE end
 * (`int p[2]; pipe(p); FILE *f = fdopen(p[0], "r");`), which is the usual way
 * ported POSIX code consumes a pipe through stdio.  'mode' is accepted for
 * source compatibility; the descriptor's own rights are the real authority
 * (the kernel rejects a read of a write-only handle), so we do not re-derive
 * access from the string.  Returns NULL on a bad fd or out of memory.
 */
FILE *fdopen(int fd, const char *mode) {
  (void)mode;
  if (fd < 0) {
    errno = EBADF;
    return 0;
  }
  FILE *f = malloc(sizeof(FILE));
  if (!f) {
    errno = ENOMEM;
    return 0;
  }
  memset(f, 0, sizeof(FILE));
  f->fd = fd;   /* fd-backed: file_fd() routes I/O through read()/write() */
  f->size = -1; /* unknown until asked */
  return f;
}

int fclose(FILE *fp) {
  if (fp && fp != stdin && fp != stdout && fp != stderr) {
    fstream_flush(fp);
    if (fp->is_tmp) {
      OS1_fs_unlink(fp->path);
    }
    /* The stream owns its descriptor (POSIX): closing the stream closes it.
     * This now covers fopen'd streams too — they hold a real handle for their
     * lifetime instead of reopening per call — as well as fdopen'd ones.  A
     * stream whose open failed carries fd = -1 and owns nothing. */
    if (fp->fd >= 0)
      close(fp->fd);
    free(fp);
  }
  return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
  if (!fp)
    return 0;
  if (size == 0 || nmemb == 0)
    return 0;
  /* R5/defensive: `size * nmemb` is the classic stdio overflow — a caller
   * computing size/nmemb from untrusted input (e.g. a file header field)
   * can wrap size_t and turn a huge request into a tiny allocation-sized
   * one, an under-read that LOOKS like success. Reject it instead of
   * silently wrapping; every real caller in this tree passes compile-time
   * constants for one of the two factors, so this never fires in practice
   * today, only against a future untrusted-size caller. */
  if (size > SIZE_MAX / nmemb) {
    fp->error = 1;
    errno = EOVERFLOW;
    return 0;
  }

  /* Read-after-write consistency: persist any buffered writes before reading
   * so an interleaved read at this position sees them on disk. */
  fstream_flush(fp);

  size_t bytes = size * nmemb;
  char *buf = (char *)ptr;
  size_t read_bytes = 0;

  if (fp->has_ungetc) {
    buf[0] = (char)fp->ungetc_buf;
    fp->has_ungetc = 0;
    read_bytes = 1;
    if (bytes == 1) {
      return 1 / size;
    }
  }

  int fd = file_fd(fp);
  if (fd >= 0 && fp->path[0] == '\0') {
    /* Console/pipe: unbuffered, and never seekable. */
    long r = read(fd, buf + read_bytes, bytes - read_bytes);
    if (r < 0) {
      fp->error = 1;
      return 0;
    }
    fp->pos += (int)r;
    if (r == 0 && bytes > read_bytes)
      fp->eof = 1;
    read_bytes += r;
    return read_bytes / size;
  }
  if (fd >= 0) {
    /*
     * BUFFERED read for a file stream, symmetric with the write buffer and for
     * the same filesystem reason: ext4 fetches a whole 4 KiB block for any
     * partial read, so an unbuffered byte-at-a-time reader re-reads the same
     * block once per byte.  doom loads savegames exactly that way — that is
     * the multi-minute load, and buffering is what removes it, not the handle
     * cache alone.
     */
    while (read_bytes < bytes) {
      if (fp->rhead >= fp->rcount) { /* window exhausted: refill from pos */
        fp->rbase = fp->pos;
        fp->rhead = 0;
        fp->rcount = 0;
        lseek(fd, fp->pos, SEEK_SET);
        long r = read(fd, fp->rbuf, FILE_RBUF_SIZE);
        if (r < 0) {
          fp->error = 1;
          break;
        }
        if (r == 0) {
          fp->eof = 1;
          break;
        }
        fp->rcount = (int)r;
      }
      size_t avail = (size_t)(fp->rcount - fp->rhead);
      size_t want = bytes - read_bytes;
      size_t take = avail < want ? avail : want;
      /* R5/R2: proves this loop's bound to the reader instead of leaving it
       * implicit — `take` is always > 0 here (avail > 0 by the refill check
       * above, want > 0 by the while condition), so read_bytes strictly
       * increases every iteration and the loop cannot spin forever. */
      nx_assert(take > 0 && read_bytes + take <= bytes);
      memcpy(buf + read_bytes, fp->rbuf + fp->rhead, take);
      fp->rhead += (int)take;
      fp->pos += (int)take;
      read_bytes += take;
    }
    return read_bytes / size;
  }
  int rem_bytes = bytes - read_bytes;
  int r = file_read(fp->path, buf + read_bytes, rem_bytes, fp->pos);
  if (r < 0) {
    fp->error = 1;
    return 0;
  }
  fp->pos += r;
  read_bytes += r;
  if (r < rem_bytes)
    fp->eof = 1;
  return read_bytes / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
  if (!fp)
    return 0;
  if (size == 0 || nmemb == 0)
    return 0;
  if (size > SIZE_MAX / nmemb) { /* R5/defensive: see fread()'s rationale */
    fp->error = 1;
    errno = EOVERFLOW;
    return 0;
  }

  /* Write-after-read consistency, the mirror of the flush fread() does before
   * reading.  The cached read window may cover the bytes about to change, and
   * rhead does not move when a write advances pos — so a read following a write
   * would otherwise be served stale bytes from the OLD position.  Dropping the
   * window costs one refill and removes the whole hazard. */
  fp->rcount = 0;
  fp->rhead = 0;

  size_t bytes = size * nmemb;
  int fd = file_fd(fp);
  /* CONSOLE/pipe streams only (no path): unbuffered, because interactive output
   * must appear now.  A FILE-backed stream must NOT take this branch even
   * though it now has an fd — buffering is not about syscall count here, it is
   * about the FILESYSTEM: ext4_write does a read-modify-write of a whole 4 KiB
   * block for any partial write, so an unbuffered byte-at-a-time writer costs
   * one 4 KiB read + 4 KiB write PER BYTE.  doom saves exactly that way, and
   * bypassing the buffer made saving look like a hang.  FILE_WBUF_SIZE is 4096
   * precisely so a full buffer is one whole block. */
  if (fd >= 0 && fp->path[0] == '\0') {
    long w = write(fd, ptr, bytes);
    if (w < 0) {
      fp->error = 1;
      return 0;
    }
    fp->pos += (int)w;
    if (fp->pos > fp->size)
      fp->size = fp->pos;
    return w / size;
  }

  /* Positional (path-backed) stream: coalesce into the write buffer, flushing a
   * full buffer to one file_write syscall.  This turns a per-byte/per-field
   * fwrite loop into one syscall per FILE_WBUF_SIZE, which is what standard C
   * stdio buffering does — the fix for incremental writers stalling on a
   * syscall-per-call storm. */
  const unsigned char *src = (const unsigned char *)ptr;
  size_t done = 0;
  while (done < bytes) {
    if (fp->wcount == FILE_WBUF_SIZE && fstream_flush(fp) != 0)
      break; /* flush failed: fp->error set, report the partial count */
    int room = FILE_WBUF_SIZE - fp->wcount;
    size_t chunk = bytes - done;
    if (chunk > (size_t)room)
      chunk = (size_t)room;
    /* R5/R2: same loop-termination proof as fread() above — room > 0 always
     * holds here (the flush above guarantees it), so chunk > 0 and done
     * strictly advances toward bytes every iteration. */
    nx_assert(chunk > 0 && done + chunk <= bytes);
    memcpy(fp->wbuf + fp->wcount, src + done, chunk);
    fp->wcount += (int)chunk;
    fp->pos += (int)chunk;
    done += chunk;
  }
  return done / size;
}

int fseek(FILE *fp, long offset, int whence) {
  if (!fp)
    return -1;
  /* Persist pending writes before the position moves: the buffer's on-disk
   * offset is derived from the current pos, so it must be flushed here. */
  fstream_flush(fp);
  if (whence == SEEK_SET)
    fp->pos = offset;
  else if (whence == SEEK_CUR)
    fp->pos += offset;
  else if (whence == SEEK_END) {
    if (fp->size < 0)
      fp->size = file_read(fp->path, NULL, 0, 0);
    fp->pos = fp->size + offset;
  }
  if (fp->pos < 0)
    fp->pos = 0;
  /* POSIX: "a successful call to fseek() shall undo any effects of ungetc()".
   * The pushback byte belongs to the OLD position, so carrying it across a seek
   * would inject a stray byte at the new one.  Latent before — the positional
   * path re-read from the file every time — and made reachable by the read
   * window, which is exactly the kind of dormant deviation a buffer exposes. */
  fp->has_ungetc = 0;
  /* A handle-backed stream keeps its offset in the KERNEL, so moving the
   * logical position must move that too — otherwise fread would keep reading
   * from wherever the last read left off and silently ignore the seek.  Only
   * for path-backed streams: seeking a console/pipe is meaningless, and those
   * carry no path. */
  if (fp->fd >= 0 && fp->path[0]) {
    /* Keep the read window if it still covers the new position — a savegame
     * reader that seeks backwards a few bytes then continues would otherwise
     * refill on every seek and lose the buffering entirely.  Otherwise drop
     * it; a stale window would serve bytes from the OLD offset. */
    if (fp->rcount > 0 && fp->pos >= fp->rbase &&
        fp->pos < fp->rbase + fp->rcount) {
      fp->rhead = fp->pos - fp->rbase;
    } else {
      fp->rcount = 0;
      fp->rhead = 0;
    }
    lseek(fp->fd, fp->pos, SEEK_SET);
  }
  fp->eof = 0;
  return 0;
}

long ftell(FILE *fp) {
  if (!fp)
    return -1;
  /* pos already includes buffered-but-unflushed bytes (fwrite advances it as it
   * accumulates), so this is the correct logical position. */
  return fp->pos;
}

int fseeko(FILE *fp, off_t offset, int whence) {
  return fseek(fp, (long)offset, whence);
}

off_t ftello(FILE *fp) {
  return (off_t)ftell(fp);
}

int feof(FILE *fp) { return fp ? fp->eof : 1; }
int ferror(FILE *fp) { return fp ? fp->error : 1; }
void fseterr(FILE *fp) { if (fp) fp->error = 1; }
void clearerr(FILE *fp) { if (fp) { fp->error = 0; fp->eof = 0; } }
int fileno(FILE *fp) { return fp ? fp->fd : -1; }

char *strdup(const char *s) {
  size_t len = strlen(s) + 1;
  char *res = malloc(len);
  if (res)
    memcpy(res, s, len);
  return res;
}

char *strndup(const char *s, size_t n) {
  if (!s)
    return NULL;
  size_t len = 0;
  while (len < n && s[len])
    len++;
  char *res = malloc(len + 1);
  if (res) {
    memcpy(res, s, len);
    res[len] = '\0';
  }
  return res;
}

/* abs() moved to math.c */
/* fabs() moved to math.c */

/* --- Standard Input Library ---
 * Input events are delivered as IPC messages from the kernel input driver.
 * IPC_TYPE_INPUT carries keyboard data; IPC_TYPE_MOUSE carries mouse data.
 * Both are received non-blocking via try_recv(-1, ...) — poll any sender.
 */

/*
 * input_poll_event - check for and decode the next pending input event.
 *
 * event: output parameter filled on success.
 *
 * Returns 1 if an event was decoded (event is valid), 0 if no message was
 * waiting or the message type is not a recognised input type.
 *
 * IPC_TYPE_INPUT layout (data1/data2/payload):
 *   data1 low byte : ASCII key code (keyboard.key)
 *   data1 bits 16+ : HID scancode   (keyboard.scancode)
 *   data2           : key state (0=released, 1=pressed, 2=repeat)
 *   payload[0..7]  : UTF-8 encoded character (up to 4 bytes + NUL)
 *
 * IPC_TYPE_MOUSE layout:
 *   data1  : button mask
 *   data2  : button state (pressed=1)
 *   payload[0..3]  : x coordinate (int32, little-endian)
 *   payload[4..7]  : y coordinate (int32, little-endian)
 *
 * Note: memcpy is used for mouse coordinates to handle potential alignment
 * constraints on the int fields within the packed payload array.
 */
int input_poll_event(input_event_t *event) {
  struct ipc_message msg;
  if (try_recv(-1, &msg) < 0)
    return 0;

  if (msg.type == IPC_TYPE_INPUT) {
    event->type = INPUT_TYPE_KEYBOARD;
    event->keyboard.key = (unsigned char)(msg.data1 & 0xFF);
    event->keyboard.scancode = (uint16_t)(msg.data1 >> 16);
    event->keyboard.state = (int)msg.data2;
    memcpy(event->keyboard.utf8, msg.payload, 8);
    return 1;
  } else if (msg.type == IPC_TYPE_MOUSE) {
    event->type = INPUT_TYPE_MOUSE;
    event->mouse.button = (int)msg.data1;
    event->mouse.state = (int)msg.data2;
    memcpy(&event->mouse.x, msg.payload, 4);
    memcpy(&event->mouse.y, msg.payload + 4, 4);
    return 1;
  } else if (msg.type == IPC_TYPE_RESIZE) {
    /* GFX-DYN-01: the compositor told us our window's new logical size. */
    event->type = INPUT_TYPE_RESIZE;
    event->resize.w = (int)msg.data1;
    event->resize.h = (int)msg.data2;
    return 1;
  } else if (msg.type == IPC_TYPE_NOTIFY && msg.data2 == IPC_LOOK_PING_MAGIC) {
    /* Silent compositor look-changed ping (nxres_broadcast_look, nxres.h),
     * tagged via data2 so it is never confused with a real notify() call.
     * Surfaced here (not drained by a second, competing try_recv() loop
     * elsewhere) so this stays the single consumer of the mailbox — a
     * second loop would silently steal keyboard/mouse messages before this
     * function ever saw them. */
    event->type = INPUT_TYPE_LOOK_CHANGED;
    return 1;
  }
  return 0;
}

/* ==========================================================================
 * MODULE 6 cont'd — Graphics: compositor drawing primitives
 *
 * graphics_draw_rect/blit/draw_text/text_width are thin veneers over
 * window_draw()/window_blit(), which are themselves thin veneers over
 * OS1_window_draw()/OS1_window_blit() — real syscalls. No local NULL/bounds
 * check is added on the buffer/dimension arguments here, on purpose and for
 * the SAME reason send()/kill_process() (MODULE 2) and OS1_registry_*
 * (MODULE 3) stay thin: the kernel validates every user pointer it's handed
 * via arch/mm/uaccess.c before touching it, so a duplicate check here
 * couldn't add safety, only give a second place for the two checks to
 * disagree.
 * -------------------------------------------------------------------------
 * High-level drawing wrappers that delegate to the compositor syscalls.
 */

/*
 * graphics_draw_rect - fill a rectangle in a compositor window.
 *
 * Thin wrapper over window_draw() -> SYS_WINDOW_DRAW (#211).
 * color is ARGB (0xAARRGGBB).
 */
void graphics_draw_rect(int win_id, int x, int y, int w, int h,
                        uint32_t color) {
  window_draw(win_id, x, y, w, h, color);
}

/*
 * graphics_blit - upload a pixel buffer to a compositor window region.
 *
 * buffer must be w*h uint32_t pixels in ARGB row-major order.
 * Delegates to window_blit() -> SYS_WINDOW_BLIT (#213).
 */
void graphics_blit(int win_id, int x, int y, int w, int h,
                   const uint32_t *buffer) {
  window_blit(win_id, x, y, w, h, buffer);
}

/*
 * graphics_draw_text - render text into a compositor window.
 *
 * Uses the OS1 packed bitmap font path when /fonts/Rewir-Light.off is present,
 * preserving x/y positioning and color. If the font is unavailable, falls back
 * to the compositor terminal writer for bootstrapping compatibility.
 *
 * Returns the rendered advance in pixels, or 0 for invalid input.
 */
int graphics_draw_text(int win_id, int x, int y, const char *text,
                       uint32_t color) {
  if (win_id < 0 || !text)
    return 0;

  struct font_ctx *font = graphics_get_default_font();
  if (font) {
    font_draw_string(win_id, font, x, y, text, color);
    return font_string_width(font, text);
  }

  (void)x;
  (void)y;
  (void)color;
  _sys_window_write(win_id, text, strlen(text));
  return (int)strlen(text) * 8;
}

int graphics_text_width(const char *text) {
  if (!text)
    return 0;

  struct font_ctx *font = graphics_get_default_font();
  if (font) {
    return font_string_width(font, text);
  }

  return (int)strlen(text) * 8;
}

#define OS1_IMAGE_MAX_FILE_BYTES (16u * 1024u * 1024u)
#define OS1_IMAGE_MAX_DIMENSION 4096
#define OS1_IMAGE_MAX_PIXELS (4096u * 4096u)

/*
 * graphics_load_image - load an encoded image into sanitized ARGB32 pixels.
 *
 * Encoded input is treated as hostile data: it is copied into a bounded scratch
 * buffer, probed before decode, rejected on dimension/pixel/file caps, decoded
 * to a temporary RGBA plane, then copied into an OS1-owned ARGB buffer.  The
 * caller never sees encoded bytes or decoder-owned storage, which keeps image
 * rendering an inert pixel operation suitable for the stdimage base API.
 */

/* nx_image_read_file - read `path` whole into a malloc'd, size-capped, 16
 * bytes-past-the-end-zeroed buffer (the padding stb_image's memory reader
 * wants). R4: split out of graphics_load_image() so the read/decode/convert
 * stages each fit the ~60-line guideline on their own instead of one ~90
 * line function doing all three. Returns NULL and closes/frees everything
 * it opened on any failure; *out_size is only valid on success. */
static unsigned char *nx_image_read_file(const char *path, int *out_size) {
  long handle =
      OS1low_handle_create(OS1_NS_FS, path, OS1_RIGHT_READ, OBJ_TYPE_FILE);
  if (handle < 0)
    return NULL;

  long stat_size = OS1_object_ctl((int)handle, OBJ_CTL_STAT, 0);
  if (stat_size <= 0 || (uint64_t)stat_size > OS1_IMAGE_MAX_FILE_BYTES) {
    (void)OS1low_handle_close((int)handle); /* R7: nothing more to do with
                                                a close failure on an
                                                already-rejected file */
    return NULL;
  }

  int size = (int)stat_size;
  unsigned char *data = malloc((size_t)size + 16u);
  if (!data) {
    (void)OS1low_handle_close((int)handle); /* R7: see rationale above */
    return NULL;
  }

  int total = 0;
  /* R2: bounded by `size` (OS1_IMAGE_MAX_FILE_BYTES-capped above); each
   * successful iteration strictly advances `total`, and a non-positive
   * `got` breaks out immediately. */
  while (total < size) {
    long got = OS1_object_read((int)handle, data + total,
                               (unsigned long)(size - total));
    if (got <= 0) {
      (void)OS1low_handle_close((int)handle); /* R7: read already failed;
                                                   the close result changes
                                                   nothing left to report */
      free(data);
      return NULL;
    }
    nx_assert(total + got <= size); /* R5 */
    total += (int)got;
  }
  (void)OS1low_handle_close((int)handle); /* R7: file is fully read; a close
                                              failure here cannot undo that */

  if (total != size) {
    free(data);
    return NULL;
  }
  for (int i = 0; i < 16; i++) /* R2: fixed 16-byte pad, see function doc */
    data[size + i] = 0;
  *out_size = size;
  return data;
}

uint32_t *graphics_load_image(const char *path, int *w, int *h) {
  if (!path || !w || !h)
    return NULL;

  *w = 0;
  *h = 0;

  int size = 0;
  unsigned char *data = nx_image_read_file(path, &size);
  if (!data)
    return NULL;

  int iw = 0;
  int ih = 0;
  int channels = 0;
  if (!stbi_info_from_memory(data, size, &iw, &ih, &channels)) {
    free(data);
    return NULL;
  }
  if (iw <= 0 || ih <= 0 || iw > OS1_IMAGE_MAX_DIMENSION ||
      ih > OS1_IMAGE_MAX_DIMENSION ||
      (uint64_t)iw * (uint64_t)ih > OS1_IMAGE_MAX_PIXELS) {
    free(data);
    return NULL;
  }

  int n = 0;
  unsigned char *rgba = stbi_load_from_memory(data, size, &iw, &ih, &n, 4);
  free(data);
  if (!rgba)
    return NULL;

  uint64_t pixels = (uint64_t)iw * (uint64_t)ih;
  if (pixels > OS1_IMAGE_MAX_PIXELS || pixels > ((uint64_t)SIZE_MAX / 4u)) {
    stbi_image_free(rgba);
    return NULL;
  }

  uint32_t *argb = (uint32_t *)malloc((size_t)pixels * 4u);
  if (!argb) {
    stbi_image_free(rgba);
    return NULL;
  }

  /* R2: bounded by `pixels`, itself capped above by OS1_IMAGE_MAX_PIXELS. */
  for (uint64_t i = 0; i < pixels; i++) {
    uint8_t r = rgba[i * 4u + 0u];
    uint8_t g = rgba[i * 4u + 1u];
    uint8_t b = rgba[i * 4u + 2u];
    uint8_t a = rgba[i * 4u + 3u];
    argb[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
              (uint32_t)b;
  }

  stbi_image_free(rgba);
  *w = iw;
  *h = ih;
  return argb;
}

/*
 * strtol - convert string to long integer with base and endptr support.
 *
 * Handles leading whitespace, optional sign, 0x/0 prefixes for base
 * auto-detection (base==0), and digits up to the given base.
 * Sets *endptr to the first non-consumed character if endptr != NULL.
 * Does not detect overflow (val accumulates without range check).
 * Negative values are produced by negating the unsigned accumulator,
 * which gives correct two's-complement representation for LONG_MIN.
 */
long strtol(const char *nptr, char **endptr, int base) {
  const char *p = nptr;
  while (isspace(*p))
    p++;
  int neg = 0;
  if (*p == '-') {
    neg = 1;
    p++;
  } else if (*p == '+')
    p++;

  /* Auto-detect base from prefix: "0x" -> 16, "0" -> 8, else 10. */
  if (base == 0) {
    if (*p == '0') {
      if (p[1] == 'x' || p[1] == 'X')
        base = 16;
      else
        base = 8;
    } else
      base = 10;
  }

  if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    p += 2;

  unsigned long val = 0;
  /* R1: rewritten from `while (1) { ...; else break; }` to a loop whose
   * termination is visible in its own header instead of buried in an
   * internal break — behaviourally identical, since isdigit('\0') and
   * isalpha('\0') are both false, so the old loop already stopped at the
   * NUL terminator; this just says so up front. */
  while (*p && (isdigit((unsigned char)*p) || isalpha((unsigned char)*p))) {
    int digit = isdigit((unsigned char)*p) ? *p - '0'
                                            : tolower((unsigned char)*p) - 'a' + 10;
    if (digit >= base)
      break;
    val = val * base + digit;
    p++;
  }

  if (endptr)
    *endptr = (char *)p;
  return neg ? -(long)val : (long)val;
}

/* --- Robust sscanf (Ported from BSD) --- */

/*
 * sscanf - varargs wrapper that delegates to vsscanf.
 */
int sscanf(const char *str, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int res = vsscanf(str, format, args);
  va_end(args);
  return res;
}

/* mkdir - create a directory through its parent directory capability. The
 * POSIX mode argument is accepted but not yet applied (the ext4 driver fixes
 * new directories at 0755); returns 0 on success, -1 on error (path exists,
 * not permitted, or provider failure), matching the POSIX contract. */
int mkdir(const char *path, mode_t mode) {
  (void)mode;
  if (!path) {
    errno = EFAULT;
    return -1;
  }
  /* EXT4-ERRNO-01 (userland half): this returned a bare -1 without touching
   * errno, so a caller doing the POSIX thing — check -1, then read errno —
   * read whatever a PREVIOUS call had left there.  A stale errno is worse than
   * none: it is indistinguishable from a fresh one.  Routed through
   * errno_ret() like every other syscall veneer. */
  return (int)errno_ret_ctx(OS1_fs_mkdir(path), path);
}
/*
 * system - run a shell command and wait for it (<stdlib.h>).
 *
 * FIX(USR-LIB-04): this used to unconditionally return 0. That silently
 * broke two separate contracts real POSIX-shaped code relies on:
 *
 *   1. system(NULL) must return non-zero IFF a command processor exists.
 *      Lua's os.execute() with no argument is exactly this check (e.g.
 *      `assert(os.execute())` in the Lua test suite reads "0" as "no
 *      shell available" and fails, even though NXShell is right there).
 *   2. system(cmd) must actually run cmd and hand back its real exit
 *      status, so os.execute(cmd)/os.execute(cmd) callers get true
 *      success/failure instead of a hardcoded fake "it worked".
 *
 * NOTE: NXSHELL_PATH and the "-c" argv convention below are the standard
 * Unix shell invocation shape (`sh -c "cmd"`). Adjust NXSHELL_PATH (and the
 * argv construction, if NXShell parses its command-line differently) to
 * match the actual shell binary shipped on this system.
 */
#define NXSHELL_PATH "/sys/bin/nxshell"

int system(const char *command) {
  if (command == NULL) {
    return 1; /* POSIX: non-zero only if a command processor exists.
               * NXShell is available. */
  }

  char *argv[4];
  argv[0] = (char *)NXSHELL_PATH;
  argv[1] = (char *)"-c";
  argv[2] = (char *)command;
  argv[3] = NULL;

  /* Phase 9c HELD AT THE GATE (2026-07-23) — the in-process path is the one
   * that runs, and the reason is the migration rule this phase set for itself.
   *
   * DESIGN-2026-07-18-NXEXEC-DAEMON §6 step 3: route a caller through the
   * service "keeping the in-process path behind a fallback UNTIL the suite is
   * at least as green as before".  It was not.  A child created by the service
   * inherits its per-process attributes from the SERVICE, and two of the three
   * were simply wrong:
   *
   *   cwd  — FIXED: the request now carries the requester's cwd and the
   *          service adopts it around the spawn (execsvc_client.c, nxexec.c).
   *          It was being parsed and discarded.
   *   ctty — NOT FIXED, and NOT fixable here.  sys_write resolves stdout to
   *          the writer's own window, else its ctty_win (kernel/core/object.c).
   *          The service has no window and no ctty, so every child it creates
   *          gets ctty_win = -1 and its output goes NOWHERE.  fd redirection
   *          cannot patch this: the destination comes from the process, not
   *          from the handle.
   *   env  — plan stall S1, same shape, already owned.
   *
   * Carrying ctty needs the spawn itself to carry it, which is Phase 9d — and
   * 9d is gated by Phase 16.  Inventing a second mechanism for it now (a
   * post-spawn ctty verb, or hanging it off OBJ_CTL_SETOWNER) is exactly the
   * duplicate-mechanism mistake the plan already recorded and withdrew when it
   * deleted 17b.  So the routing waits for the ONE mechanism instead.
   *
   * execsvc_spawn() is NOT dead: captest exercises the protocol directly
   * ("exec service" section), so the daemon stays verified while it is off the
   * POSIX critical path.  Re-enable here — and nowhere else — once 9d lands. */
  int pid = spawn_args(NXSHELL_PATH, 3, argv);
  if (pid < 0) {
    errno = ENOENT;
    return -1;
  }

  /* Blocking join through polling, using the same pattern as
   * nxexec_run_foreground():
   * wait() is non-blocking (-1 = still running / pid reaped / -2 = not
   * found), so it must be queried in a loop until it no longer reports
   * "running" — not called only once like before (that single -1 result
   * was interpreted as ECHILD, causing system() to fail almost every time). */
  int w, code = 0;
  while ((w = OS1low_process_wait_status(pid, &code)) == -1)
    OS1_sleep(15);

  /* POSIX system() status word: WEXITSTATUS reads (status>>8)&0xff (Phase 2).
   * A -2 "reaped elsewhere" leaves code 0 — the command still ran. */
  (void)w;
  return __wait_encode(code);
}

/*
 * waitpid - POSIX <sys/wait.h> wait (Phase 2).  Was declared but never
 * defined (an undefined-symbol trap for ported code).  Backed by the PROCESS
 * capability wait; fills *status with the POSIX-encoded exit code.  WNOHANG is
 * a single non-blocking poll; other options (WUNTRACED) are ignored (no
 * stopped-process state yet — a Phase 2 follow-up).
 */
int waitpid(int pid, int *status, int options) {
  int code = 0, w;
  if (options & WNOHANG) {
    w = OS1low_process_wait_status(pid, &code);
    if (w == -1)
      return 0; /* still running */
    if (w == -2) {
      errno = ECHILD;
      return -1;
    }
    if (status)
      *status = __wait_encode(code);
    return pid;
  }
  while ((w = OS1low_process_wait_status(pid, &code)) == -1)
    OS1_sleep(15);
  if (w == -2) {
    errno = ECHILD;
    return -1;
  }
  if (status)
    *status = __wait_encode(code);
  return pid;
}

/*
 * cmdline_split - split a command line into argv[], honoring quotes.
 *
 * THE shared command tokenizer (used by nxshell for both interactive input
 * and `nxshell -c "<cmd>"`, which is how system()/Lua os.execute() reach a
 * program).  Whitespace separates words EXCEPT inside a '...' or "..." span,
 * which is kept as one token with its surrounding quotes REMOVED — so a
 * shell-style quoted program name like "lua" resolves to lua (not the
 * literal /bin/"lua" that broke os.execute), and a quoted argument that
 * contains spaces (Lua's `lua -e "a = 1"`) survives as a single argv entry.
 * A backslash inside a double-quoted span escapes a following '"' or '\'.
 *
 * Tokens are compacted IN PLACE inside s (out <= s always, so writing the
 * NUL terminators never clobbers a not-yet-scanned token).  Returns argc;
 * each argv[i] points into s.  Matches the previous whitespace-only splitter
 * when no quotes are present.
 */
int cmdline_split(char *s, char **argv, int max) {
  int argc = 0;
  if (!s || !argv || max <= 0)
    return 0;
  while (*s && argc < max) {
    while (*s == ' ' || *s == '\t')
      s++;
    if (!*s)
      break;
    char *out = s;
    argv[argc++] = out;
    while (*s && *s != ' ' && *s != '\t') {
      char c = *s;
      if (c == '"' || c == '\'') {
        char q = c;
        s++;
        while (*s && *s != q) {
          if (q == '"' && *s == '\\' && (s[1] == '"' || s[1] == '\\'))
            s++; /* consume the backslash, copy the escaped char verbatim */
          *out++ = *s++;
        }
        if (*s == q)
          s++; /* consume the closing quote */
      } else {
        *out++ = *s++;
      }
    }
    if (*s)
      s++;       /* step past the delimiter for the next scan */
    *out = '\0'; /* terminate the compacted token (out <= s) */
  }
  return argc;
}
/* atof: real IEEE-754 float parse via strtod (math.c). See the file header's
 * USR-LIB-04 entry — the previous inline note here ("only integer part is
 * parsed") was stale documentation describing code that had already been
 * fixed; removed rather than repeated a second time in this file. */
double atof(const char *nptr) { return strtod(nptr, NULL); }
/* ---------------------------------------------------------------------------
 * ENVIRONMENT — Phase 17
 *
 * TWO LAYERS, because ASTRA §6.8 and this plan's opening rule both require it:
 * the kernel is NOT POSIX; POSIX is a personality built ON TOP of OS1.
 *
 *   OS1_env_*   the NATIVE surface.  Owns the one fact that the kernel exposes
 *               the environment through the `sys.proc.<pid>.env.*` registry
 *               namespace, with `sys.env.*` beneath it as machine defaults.
 *   getenv/...  the POSIX personality: a THIN mapping over OS1_env_*, with no
 *               knowledge of the registry whatsoever.
 *
 * The first cut of this phase collapsed the two — POSIX built registry key
 * strings itself — which put the registry's path syntax into the POSIX API and
 * led to setenv() rejecting any name containing a '.'.  That restriction was
 * never real: the kernel's virtual-key router hands everything after "env." to
 * the scheduler as one opaque name, so dots always worked.  It was a limit
 * invented to serve a layering mistake.
 * ------------------------------------------------------------------------- */

#define OS1_ENV_KEYMAX 96

/* env_self - our own pid, resolved once.
 *
 * getpid() is a syscall, and every env operation needs the pid to name its own
 * branch of the namespace — so the naive form costs TWO syscalls per getenv(),
 * on a function that path resolution calls per spawn.  A process's pid never
 * changes (pids come from a monotonic counter and there is no fork), so the
 * value is cacheable without an invalidation story. */
static int env_self(void) {
  static int self_pid;
  if (self_pid <= 0)
    self_pid = getpid();
  return self_pid;
}

/* env_key - build the per-process namespace path.  The ONLY place in userland
 * that knows this syntax. */
static int env_key(char *out, size_t size, const char *name) {
  if (!name || !*name || strchr(name, '='))
    return -1; /* POSIX: '=' separates name from value, so it cannot be in one
                */
  int n = snprintf(out, size, "sys.proc.%d.env.%s", env_self(), name);
  /* R7: snprintf() returning > 0 alone does NOT mean the key fit — a long
   * `name` truncates silently and a truncated key can alias a DIFFERENT,
   * unrelated variable instead of failing loudly. Check the full contract:
   * the return is the length it WOULD have written; only n < size means it
   * actually did. */
  return (n > 0 && (size_t)n < size) ? 0 : -1;
}

int OS1_env_get(const char *name, char *buf, size_t size) {
  char key[OS1_ENV_KEYMAX];
  if (!buf || size == 0 || env_key(key, sizeof(key), name) != 0)
    return -1;
  /* Process value first: it SHADOWS the machine default, which is the whole
   * point of having both layers. */
  if (OS1_registry_get(key, buf, size) == 0)
    return 0;
  int n = snprintf(key, sizeof(key), "sys.env.%s", name);
  if (n <= 0 || (size_t)n >= sizeof(key)) /* R7: same truncation check */
    return -1;
  if (OS1_registry_get(key, buf, size) == 0 && buf[0])
    return 0;
  return -1;
}

int OS1_env_set(const char *name, const char *value) {
  char key[OS1_ENV_KEYMAX];
  if (env_key(key, sizeof(key), name) != 0)
    return -1;
  /* Writes always land on the PROCESS layer, never on sys.env.*: a program
   * calling setenv means "for me and my children", not "reconfigure the
   * machine".  Keeping that distinction here is why setenv needs no privilege
   * while editing the defaults still does. */
  return OS1_registry_set(key, value ? value : "") == 0 ? 0 : -1;
}

int OS1_env_unset(const char *name) {
  char key[OS1_ENV_KEYMAX];
  if (env_key(key, sizeof(key), name) != 0)
    return -1;
  /* R7: DEL and set-to-empty both clear the slot, so a DEL failure here
   * (e.g. the key was never set) changes nothing this function has promised
   * — unsetenv() of an already-unset name is a POSIX no-op success, not an
   * error — so the result is intentionally, not accidentally, discarded. */
  (void)OS1_registry_del(key);
  return 0;
}

int OS1_env_enum(char *buf, size_t size) {
  char prefix[OS1_ENV_KEYMAX];
  if (!buf || size == 0)
    return -1;
  int plen0 = snprintf(prefix, sizeof(prefix), "sys.proc.%d.env.", env_self());
  if (plen0 <= 0 || (size_t)plen0 >= sizeof(prefix)) /* R7: truncation check */
    return -1;
  int n = OS1_registry_enum_under(prefix, buf, size - 1);
  if (n <= 0) {
    buf[0] = '\0';
    return 0;
  }
  /* R5: OS1_registry_enum_under() is a syscall veneer; trust its own bound
   * but not blindly — n must fit the buffer we gave it (size - 1) before we
   * index buf[n] below. */
  nx_assert(n > 0 && (size_t)n <= size - 1);
  buf[n] = '\0';
  /* Enumeration returns FULL keys; strip the namespace so callers above this
   * layer never see it.  Rewrites in place, line by line. */
  size_t plen = strlen(prefix);
  char *w = buf;
  for (char *r = buf; *r;) {
    char *nl = strchr(r, '\n');
    size_t len = nl ? (size_t)(nl - r) : strlen(r);
    const char *nm =
        (len > plen && strncmp(r, prefix, plen) == 0) ? r + plen : r;
    size_t nlen = len - (size_t)(nm - r);
    /* R5/R2: the write cursor `w` can only ever trail the read cursor `r`
     * (stripping a prefix removes bytes, never adds them), so this loop is
     * bounded by the same `n <= size - 1` proven above — assert it instead
     * of trusting the arithmetic silently on every iteration. */
    nx_assert((size_t)(w - buf) <= (size_t)(r - buf));
    memmove(w, nm, nlen);
    w += nlen;
    if (!nl)
      break;
    *w++ = '\n';
    r = nl + 1;
  }
  *w = '\0';
  return (int)(w - buf);
}

/* --- POSIX personality (<stdlib.h>) — a thin mapping, nothing more --------
 *
 * Documented deviation: the string returned by getenv() lives in a small
 * rotating buffer pool, so it is valid until GETENV_SLOTS further getenv()
 * calls, not until the next setenv().  Callers keeping a value across many
 * lookups must copy it.  The alternative — a per-name heap cache that is never
 * freed — leaks by design. */
#define GETENV_SLOTS 4
#define GETENV_VALMAX 128

static char *_default_environ[] = { NULL };
char **environ = _default_environ;

char *getenv(const char *name) {

  static char slots[GETENV_SLOTS][GETENV_VALMAX];
  static int next_slot;
  char *out = slots[next_slot];
  if (OS1_env_get(name, out, GETENV_VALMAX) != 0)
    return NULL;
  next_slot = (next_slot + 1) % GETENV_SLOTS;
  return out;
}

int setenv(const char *name, const char *value, int overwrite) {
  if (!overwrite && getenv(name))
    return 0;
  return OS1_env_set(name, value);
}

int unsetenv(const char *name) { return OS1_env_unset(name); }

/*
 * putenv - POSIX's older "NAME=VALUE" form.
 *
 * POSIX says the caller's string BECOMES part of the environment, so later
 * edits to it are visible.  We cannot honour that — the storage is in the
 * kernel — so this parses and copies, like setenv.  Stated because a caller
 * relying on the aliasing would otherwise be silently wrong; in practice every
 * caller in this tree passes a string literal.
 */
int putenv(char *string) {
  if (!string)
    return -1;
  const char *eq = strchr(string, '=');
  if (!eq)
    return unsetenv(string); /* "NAME" with no '=' unsets, per POSIX */
  char name[OS1_ENV_KEYMAX];
  size_t nl = (size_t)(eq - string);
  if (nl == 0 || nl >= sizeof(name))
    return -1;
  memcpy(name, string, nl);
  name[nl] = '\0';
  return setenv(name, eq + 1, 1);
}

int clearenv(void) {
  char buf[512];
  if (OS1_env_enum(buf, sizeof(buf)) <= 0)
    return 0;
  char *save = NULL;
  for (char *k = strtok_r(buf, "\n", &save); k; k = strtok_r(NULL, "\n", &save))
    OS1_env_unset(k);
  return 0;
}

/*
 * env_names - NexsOS extension replacing POSIX `environ`.
 *
 * The POSIX way to enumerate assumes the environment is a userland array the
 * process can point at.  Here it is kernel state, so a `char **environ` would
 * mean publishing a snapshot and then lying about how fresh it is.  An explicit
 * enumerator says what it actually does: names only, values via getenv().
 */
int env_names(char *names[], int max) {
  static char buf[512];
  int count = 0;
  if (OS1_env_enum(buf, sizeof(buf)) <= 0)
    return 0;
  char *save = NULL;
  for (char *k = strtok_r(buf, "\n", &save); k && count < max;
       k = strtok_r(NULL, "\n", &save))
    names[count++] = k;
  return count;
}

/*
 * __env_propagate_to_child - copies every variable that THIS process has
 * set (its own branch sys.proc.<self>.env.* — the machine defaults
 * under sys.env.* don't need copying, the child inherits them anyway
 * via the fallback of OS1_env_get) into the branch of the child just spawned.
 *
 * Without this every child starts with an EMPTY environment: nothing in
 * this tree wrote it, so an `export PATH=...` (or any setenv())
 * in a shell was invisible to everything it launched, and `printenv PATH`
 * in a child always failed even with the parent's PATH set.
 * execsvc_client.c already carries the cwd of the requester through the
 * service protocol for exactly the same reason ("cwd is wrong for exactly
 * the same reason the environment is"); this is the same correction applied
 * to the direct spawn path, the one that nxexec_spawn_search_redir/
 * nxshell use for every foreground and background command.
 *
 * Best-effort: a write failure here doesn't fail the spawn (the child starts
 * anyway, just without that variable) — consistent with setenv() which is
 * also a best-effort registry write.
 *
 * NOTE: the execsvc path (Phase 9c) is NOT covered here — a spawn routed
 * via /sys/bin/nxexec happens IN THE SERVICE PROCESS, so "self" in the
 * point where __env_propagate_to_child would run is the service, not the
 * requester. Fixing it requires bringing the environment into the network
 * protocol like already happens for the cwd (bump of EXECSVC_VERSION) —
 * left as a follow-on, in line with the note Phase 9d of the header on
 * cwd/ctty/env that must arrive together "as ONE mechanism".
 */
static void __env_propagate_to_child(long child_pid) {
  if (child_pid <= 0)
    return;
  char buf[512];
  if (OS1_env_enum(buf, sizeof(buf)) <= 0)
    return;
  char *save = NULL;
  for (char *name = strtok_r(buf, "\n", &save); name;
       name = strtok_r(NULL, "\n", &save)) {
    char val[GETENV_VALMAX];
    if (OS1_env_get(name, val, sizeof(val)) != 0)
      continue;
    char key[OS1_ENV_KEYMAX];
    int klen = snprintf(key, sizeof(key), "sys.proc.%ld.env.%s", child_pid, name);
    if (klen <= 0 || (size_t)klen >= sizeof(key)) /* R7: truncation check */
      continue;
    /* R7: best-effort by design (see the function comment above) — a single
     * variable failing to propagate must not abort the spawn, so the result
     * is intentionally discarded here, not merely unchecked. */
    (void)OS1_registry_set(key, val);
  }
}

/*
 * stat - path metadata in ONE syscall (SYS_STAT).
 *
 * It used to infer the type: "list_dir succeeds ONLY on directories, so a >= 0
 * probe IS a directory".  That invariant was load-bearing in TWO places (here
 * and opendir) and R1 broke it — once a READ handle could be acquired on any
 * path, listing a regular FILE returned its CONTENT instead of failing, so
 * every file looked like a directory and the file manager tried to chdir into
 * them ("Cannot open directory").
 *
 * Inference replaced by a fact: the kernel is the only place that knows a
 * node's type, so it reports it.  One round trip instead of a listing probe
 * plus a size probe.
 */
int stat(const char *path, struct stat *buf)

{
    if (!path) {
        errno = EFAULT;
        return -1;
    }

    if (buf)
        memset(buf, 0, sizeof(struct stat));

    struct abi_stat as;
    int r = _sys_stat(path, &as);
    if (r != 0) {
        errno = ENOENT;          /* o meglio: errno = -r; se il kernel restituisce errno negativi */
        return -1;
    }

    if (buf) {
        buf->st_size  = (off_t)as.size;
        buf->st_mode  = (as.type == ABI_S_TYPE_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0644);
        buf->st_nlink = 1;
        buf->st_uid   = 0;
        buf->st_gid   = 0;
        buf->st_blksize = 4096;
        buf->st_blocks  = (as.size + 511) / 512;

        /* Timestamp temporanei (finché il VFS non li supporta davvero) */
        time_t now = time(NULL);
        buf->st_atime = now;
        buf->st_mtime = now;
        buf->st_ctime = now;

#if defined(_STATBUF_ST_NSEC) || defined(__USE_XOPEN2K8)
        buf->st_atim.tv_sec  = now;
        buf->st_atim.tv_nsec = 0;
        buf->st_mtim.tv_sec  = now;
        buf->st_mtim.tv_nsec = 0;
        buf->st_ctim.tv_sec  = now;
        buf->st_ctim.tv_nsec = 0;
#endif
    }

    return 0;
}

int statfs(const char *path, struct statfs *buf)
{
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    if (!path) {
        errno = EFAULT;
        return -1;
    }

    memset(buf, 0, sizeof(*buf));

    struct stat st;
    if (stat(path, &st) != 0)
        return -1;

    /* Placeholder values — NexsOS1 non ha ancora un vero FS query */
    buf->f_type    = 0x01021994UL;   /* tmpfs-like */
    buf->f_bsize   = 4096;
    buf->f_frsize  = 4096;
    buf->f_blocks  = (st.st_size + 4095) / 4096;
    buf->f_bfree   = buf->f_blocks;
    buf->f_bavail  = buf->f_blocks;
    buf->f_files   = 0;
    buf->f_ffree   = 0;
    buf->f_fsid.__val[0] = 0;
    buf->f_fsid.__val[1] = 0;
    buf->f_namelen = 255;
    buf->f_flags   = 0;

    return 0;
}

int fstatfs(int fd, struct statfs *buf)
{
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }

    /* NexsOS1 non ha ancora un modo per ottenere il path da un fd,
       quindi per ora usiamo un placeholder basato su fstat.
       Quando avremo fd → path o una vera syscall di fsinfo, si sistemerà. */
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;

    memset(buf, 0, sizeof(*buf));
    buf->f_type    = 0x01021994UL;
    buf->f_bsize   = 4096;
    buf->f_frsize  = 4096;
    buf->f_blocks  = (st.st_size + 4095) / 4096;
    buf->f_bfree   = buf->f_blocks;
    buf->f_bavail  = buf->f_blocks;
    buf->f_files   = 0;
    buf->f_ffree   = 0;
    buf->f_fsid.__val[0] = 0;
    buf->f_fsid.__val[1] = 0;
    buf->f_namelen = 255;
    buf->f_flags   = 0;

    return 0;
}

/*
 * vfprintf - format and write to a FILE stream.
 *
 * Routes through fwrite(), so it honours 'stream' exactly like any other
 * stream I/O: stdin/stdout/stderr go via file_fd()+write() (all three share
 * one CONSOLE object kernel-side, so stdout/stderr are visually
 * indistinguishable today, but a real fopen()ed FILE* now writes to its own
 * path/position instead of unconditionally landing on fd 1 (fixes
 * USR-LIB-05).
 *
 * The stack buffer is the fast path (R3: no heap traffic for a normal-length
 * line, which is nearly every call); output past it is reformatted into an
 * exactly-sized heap buffer instead of being silently cut — same two-pass
 * technique as printf() and vasprintf() above. This used to be a documented,
 * live limitation ("longer output is silently truncated"); it no longer is.
 */
int vfprintf(FILE *stream, const char *format, va_list ap) {
  char buf[1024];
  va_list ap2;
  va_copy(ap2, ap); /* `ap` is a caller-owned va_list, not ours to consume */
  int n = vsnprintf(buf, sizeof(buf), format, ap2);
  va_end(ap2);
  if (n < 0)
    return n;
  if ((size_t)n < sizeof(buf)) {
    fwrite(buf, 1, (size_t)n, stream);
    return n;
  }
  char *big = malloc((size_t)n + 1U);
  if (!big) {
    fwrite(buf, 1, strlen(buf), stream);
    return n;
  }
  int n2 = vsnprintf(big, (size_t)n + 1U, format, ap);
  nx_assert(n2 == n); /* R5: see printf()'s identical assertion */
  fwrite(big, 1, (size_t)(n2 > 0 ? n2 : 0), stream);
  free(big);
  return n;
}

int vprintf(const char *format, va_list ap) {
  return vfprintf(stdout, format, ap);
}

int fprintf(FILE *stream, const char *format, ...) {
  va_list args;
  va_start(args, format);
  int res = vfprintf(stream, format, args);
  va_end(args);
  return res;
}
/* fflush: write out a positional stream's pending write buffer.  fflush(NULL)
 * is defined to flush all open output streams; this libc keeps no open-stream
 * registry, so that form is a no-op (callers flush the specific stream). */
int fflush(FILE *stream) {
  if (!stream)
    return 0;
  return fstream_flush(stream);
}
/*
 * remove - delete a file (POSIX/<stdio.h>).  Was a no-op stub that returned
 * success while deleting nothing, so os.remove()/temp-file cleanup silently
 * did nothing; the unlink syscall existed all along (OS1_fs_unlink).
 */
int remove(const char *pathname) {
  return (int)errno_ret_ctx(OS1_fs_unlink(pathname), pathname);
}
/*
 * unlink - remove a file (POSIX <unistd.h>).  The unistd.h declaration existed
 * with no definition in this libc (only in vendored musl, which our programs
 * don't link), so a caller would have hit an undefined symbol.  Real now — the
 * same VFS unlink `remove()` uses.
 */
int unlink(const char *pathname) {
  return (int)errno_ret_ctx(OS1_fs_unlink(pathname), pathname);
}
/*
 * link - create a second name for the same file contents.
 *
 * The current NexsOS1 FS layer provides create/write/read and delete, but no
 * inode-level hard-link syscall; the correct compatibility point is therefore
 * the existing file-creation path, not a synthetic extra file or fake kernel ABI.
 * We copy the source bytes into the destination path, fail if the destination
 * already exists, and leave the original untouched.
 */
int link(const char *oldpath, const char *newpath) {
  int size = OS1_fs_read(oldpath, NULL, 0, 0);
  if (size < 0)
    return -1;
  if (OS1_fs_read(newpath, NULL, 0, 0) >= 0) {
    errno = EEXIST;
    return -1;
  }
  if (size == 0) {
    return OS1_fs_write(newpath, "", 0, 0) < 0 ? -1 : 0;
  }
  char *buf = malloc((size_t)size);
  if (!buf) {
    errno = ENOMEM;
    return -1;
  }
  int n = OS1_fs_read(oldpath, buf, size, 0);
  if (n < 0) {
    free(buf);
    return -1;
  }
  int w = OS1_fs_write(newpath, buf, n, 0);
  free(buf);
  if (w < 0)
    return -1;
  return 0;
}
/*
 * rename - move a file (POSIX/<stdio.h>).  No rename syscall exists, so this
 * emulates it as copy + unlink of the original — the same approach nxshell's
 * `mv` uses.  Not atomic and it rewrites the bytes, but it makes os.rename()
 * and any POSIX renamer actually work instead of falsely reporting success.
 */
/*
 * mknod - create a device file (stub).
 *
 * NexsOS1 does not expose device files; this is a compatibility stub for
 * POSIX applications that try to create device nodes.  Returns -ENOTSUP.
 */
int mknod(const char *pathname, mode_t mode, dev_t dev) {
  (void)pathname;
  (void)mode;
  (void)dev;
  errno = ENOTSUP;
  return -1;
}
/*
 * mkfifo - create a named pipe (stub).
 *
 * NexsOS1 does not support named pipes (FIFOs); this is a compatibility stub.
 * Returns -ENOTSUP.
 */
int mkfifo(const char *pathname, mode_t mode) {
  (void)pathname;
  (void)mode;
  errno = ENOTSUP;
  return -1;
}
/*
 * getrlimit - query resource limits (stub).
 *
 * NexsOS1 does not enforce traditional process resource limits. This stub
 * returns dummy unlimited values for all resources (rlim_cur = rlim_max = 2^32).
 */
int getrlimit(int resource, struct rlimit *rlim) {
  (void)resource;
  if (!rlim) {
    errno = EFAULT;
    return -1;
  }
  /* Pretend all limits are effectively unlimited (2^32 - 1 bytes/seconds).
   * Applications that check for resource limits will see "no limits". */
  rlim->rlim_cur = 0xFFFFFFFFUL;
  rlim->rlim_max = 0xFFFFFFFFUL;
  return 0;
}
/*
 * setrlimit - set resource limits (stub).
 *
 * NexsOS1 does not enforce resource limits. This stub silently accepts any
 * setrlimit call and succeeds (no error). Applications that try to raise or
 * lower limits will believe they succeeded.
 */
int setrlimit(int resource, const struct rlimit *rlim) {
  (void)resource;
  (void)rlim;
  return 0;  /* silently accept all limit changes */
}
/*
 * getrusage - query resource usage (stub).
 *
 * NexsOS1 does not track per-process CPU time or resource usage. This stub
 * returns zero values for all fields. Applications that call getrusage
 * (e.g., to measure performance) will see "no CPU time used".
 */
int getrusage(int who, struct rusage *usage) {
  if (!usage) {
    errno = EFAULT;
    return -1;
  }
  (void)who;
  /* Zero all fields: the process has used 0 CPU time, 0 memory, etc. */
  memset(usage, 0, sizeof(struct rusage));
  return 0;
}
/*
 * getloadavg - get system load average (real implementation via OS1_sys_stats).
 *
 * NexsOS1 provides instantaneous scheduler load via OS1_sys_stats(sched_runnable).
 * Since there is no historical load tracking, we report the current snapshot
 * (number of ready+running processes) for all three intervals (1m, 5m, 15m).
 * This gives userland programs an accurate instantaneous load, not a moving average.
 */
int getloadavg(double loadavg[], int nelem) {
  if (!loadavg || nelem < 1) {
    errno = EINVAL;
    return -1;
  }
  
  struct os1_sysstats stats;
  long ret = OS1_sys_stats(&stats);
  if (ret < 0) {
    errno = (int)-ret;
    return -1;
  }
  
  /* Use the current runnable count (READY+RUNNING processes) as load */
  double load = (double)stats.sched_runnable;
  
  /* Fill the array up to nelem with the same load value */
  for (int i = 0; i < nelem; i++)
    loadavg[i] = load;
  
  return nelem;  /* Return number of elements filled */
}
/*
 * getpriority / setpriority - process scheduling priority (stub).
 *
 * NexsOS1 does not have traditional process priority control. These stubs
 * pretend the process always has priority 0 (neutral). Calls to setpriority
 * silently succeed without changing anything.
 */
int getpriority(int which, int who) {
  (void)which;
  (void)who;
  /* Return priority 0 (neutral/default) for all processes. */
  return 0;
}
int setpriority(int which, int who, int prio) {
  (void)which;
  (void)who;
  (void)prio;
  /* Silently accept any priority change. */
  return 0;
}
/*
 * exec family - replace the process image (stub).
 *
 * NexsOS1 does not support exec; child processes are spawned via the OS1
 * capability-based spawn model, not exec. These stubs return -ENOTSUP.
 */
int execv(const char *pathname, char *const argv[]) {
  (void)pathname;
  (void)argv;
  errno = ENOTSUP;
  return -1;
}
int execvp(const char *file, char *const argv[]) {
  (void)file;
  (void)argv;
  errno = ENOTSUP;
  return -1;
}
int execl(const char *pathname, const char *arg, ...) {
  (void)pathname;
  (void)arg;
  errno = ENOTSUP;
  return -1;
}
int execlp(const char *file, const char *arg, ...) {
  (void)file;
  (void)arg;
  errno = ENOTSUP;
  return -1;
}
int execle(const char *pathname, const char *arg, ...) {
  (void)pathname;
  (void)arg;
  errno = ENOTSUP;
  return -1;
}
/*
 * localtime - convert time_t to struct tm.
 * Canonical OS1 userland implementation kept in lib.c so every ELF gets a
 * single consistent UTC conversion path.
 */
struct tm *localtime(const time_t *timep) {
  static struct tm result;
  if (!timep)
    return NULL;

  time_t t = *timep;
  result.tm_sec = t % 60;
  t /= 60;
  result.tm_min = t % 60;
  t /= 60;
  result.tm_hour = t % 24;
  t /= 24;

  result.tm_wday = (t + 4) % 7;

  int year = 1970;
  while (1) {
    int days_in_year = 365;
    if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))
      days_in_year = 366;
    if (t < days_in_year)
      break;
    t -= days_in_year;
    year++;
  }
  result.tm_year = year - 1900;
  result.tm_yday = t;

  int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))
    month_days[1] = 29;

  int mon = 0;
  while (t >= month_days[mon]) {
    t -= month_days[mon];
    mon++;
  }
  result.tm_mon = mon;
  result.tm_mday = t + 1;
  result.tm_isdst = 0;
  return &result;
}

/*
 * mktime - convert struct tm to time_t.
 */
time_t mktime(struct tm *tm) {
  int year = tm->tm_year + 1900;
  int mon = tm->tm_mon;
  int mday = tm->tm_mday;

  long days = 0;
  for (int y = 1970; y < year; y++)
    days += ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 366 : 365;

  int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))
    month_days[1] = 29;

  for (int m = 0; m < mon; m++)
    days += month_days[m];
  days += mday - 1;

  return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

/*
 * gmtime - convert time_t to UTC struct tm.
 */
struct tm *gmtime(const time_t *timep) {
  return localtime(timep);
}

/*
 * strftime - format a broken-down time in UTC using a minimal subset of
 * conversion directives required by Lua and Gnulib.
 */
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
  if (!s || !format || !tm || max == 0)
    return 0;

  size_t out = 0;
  const char *p = format;
  while (*p && out + 1 < max) {
    if (*p != '%') {
      s[out++] = *p++;
      continue;
    }

    p++;
    if (!*p)
      break;

    char buf[64];
    int len = 0;
    switch (*p) {
    case '%':
      s[out++] = '%';
      break;
    case 'Y':
      len = snprintf(buf, sizeof(buf), "%d", tm->tm_year + 1900);
      break;
    case 'm':
      len = snprintf(buf, sizeof(buf), "%02d", tm->tm_mon + 1);
      break;
    case 'd':
      len = snprintf(buf, sizeof(buf), "%02d", tm->tm_mday);
      break;
    case 'H':
      len = snprintf(buf, sizeof(buf), "%02d", tm->tm_hour);
      break;
    case 'M':
      len = snprintf(buf, sizeof(buf), "%02d", tm->tm_min);
      break;
    case 'S':
      len = snprintf(buf, sizeof(buf), "%02d", tm->tm_sec);
      break;
    case 'a':
      len = snprintf(buf, sizeof(buf), "%s", "Sun");
      break;
    case 'A':
      len = snprintf(buf, sizeof(buf), "%s", "Sunday");
      break;
    case 'b':
      len = snprintf(buf, sizeof(buf), "%s", "Jan");
      break;
    default:
      s[out++] = *p;
      break;
    }

    if (len > 0) {
      size_t n = (size_t)len;
      if (n >= max - out)
        n = max - out - 1;
      memcpy(s + out, buf, n);
      out += n;
    }
    p++;
  }

  s[out] = '\0';
  return out;
}

/*
 * mktime_z / localtime_rz - timezone-aware time conversion (stub).
 *
 * NexsOS1 does not have timezone support. These stubs delegate to the
 * standard mktime/localtime (which use UTC/system time).
 */
time_t mktime_z(timezone_t tz, struct tm *tm) {
  (void)tz;
  return mktime(tm);
}

static timezone_t g_utc_tz = NULL;

timezone_t tzalloc(const char *name) {
  (void)name;
  if (!g_utc_tz) {
    g_utc_tz = calloc(1, sizeof(*g_utc_tz));
  }
  return g_utc_tz;
}

void tzfree(timezone_t tz) {
  (void)tz;
}

struct tm *localtime_rz(timezone_t tz, const time_t *t, struct tm *tm) {
  (void)tz;
  if (!t || !tm)
    return NULL;
  struct tm *result = localtime(t);
  if (result)
    *tm = *result;
  return tm;
}
int rename(const char *oldpath, const char *newpath) {
  int size = OS1_fs_read(oldpath, NULL, 0, 0); /* size probe; errno on miss */
  if (size < 0)
    return -1;
  /* The destination write starts at offset 0, so ext4_write truncates any
   * longer pre-existing dst to exactly the copied length — no separate unlink
   * of newpath is needed to avoid trailing garbage. */
  if (size > 0) {
    char *buf = malloc((size_t)size);
    if (!buf) {
      errno = ENOMEM;
      return -1;
    }
    int n = OS1_fs_read(oldpath, buf, size, 0);
    if (n < 0) {
      free(buf);
      return -1;
    }
    int w = OS1_fs_write(newpath, buf, n, 0);
    free(buf);
    if (w < 0)
      return -1;
  } else if (OS1_fs_write(newpath, "", 0, 0) < 0) {
    return -1;
  }
  OS1_fs_unlink(oldpath);
  return 0;
}

/*
 * truncate - set a file's length to exactly `length` (POSIX <unistd.h>).
 *
 * The NexsOS write model (PLAN-2026-07-17-STRATIFICATION.md Phase 1): a
 * from-start write (offset 0) IS the whole-file-replace/truncate primitive at
 * the FS layer (ext4_write), so this builds on it with no new kernel call:
 *   - length == 0        -> a zero-byte offset-0 write empties the file;
 *   - length <  size     -> re-write the first `length` bytes at offset 0,
 *                           which truncates the tail;
 *   - length >  size     -> zero-extend from the old EOF.
 * Explicit truncation for programs that don't go through fopen("w").
 * ftruncate(fd) needs an fd->path or an OBJ_CTL_TRUNCATE verb (a kernel
 * follow-up), so it is intentionally not provided yet rather than faked.
 */
int truncate(const char *path, long length) {
  if (length < 0) {
    errno = EINVAL;
    return -1;
  }
  int size = OS1_fs_read(path, NULL, 0, 0); /* current size; errno on miss */
  if (size < 0)
    return -1;
  if (length == (long)size)
    return 0;
  if (length == 0)
    return OS1_fs_write(path, "", 0, 0) < 0 ? -1 : 0;
  if (length < (long)size) {
    char *buf = malloc((size_t)length);
    if (!buf) {
      errno = ENOMEM;
      return -1;
    }
    int n = OS1_fs_read(path, buf, (int)length, 0);
    int r = (n < 0 || OS1_fs_write(path, buf, n, 0) < 0) ? -1 : 0;
    free(buf);
    return r;
  }
  /* extend: zero-fill from the old EOF up to `length` */
  {
    long pad = length - (long)size;
    char *z = calloc(1, (size_t)pad);
    if (!z) {
      errno = ENOMEM;
      return -1;
    }
    int r = OS1_fs_write(path, z, (int)pad, size) < 0 ? -1 : 0;
    free(z);
    return r;
  }
}
/*
 * ftruncate - set an OPEN descriptor's file length (POSIX).  Closes the Phase 1
 * leftover ("ftruncate awaits an fd->path / OBJ_CTL_TRUNCATE kernel verb").
 *
 * Composed from the primitives rather than duplicating truncate()'s path-based
 * logic in the kernel:
 *   - shrink to N>0: read the first N bytes back, then rewrite them from offset
 *     0 — an offset-0 write REPLACES the whole file (the Phase 1 FS standard),
 *     so the rewrite IS the truncation;
 *   - shrink to 0: needs OBJ_CTL_TRUNCATE, because POSIX defines
 *     write(fd, buf, 0) as a NO-OP and so it cannot express "empty this file";
 *   - extend: zero-fill from the old EOF up to the new length.
 */
int ftruncate(int fd, long length) {
  if (length < 0) {
    errno = EINVAL;
    return -1;
  }
  long size = lseek(fd, 0, SEEK_END);
  if (size < 0)
    return -1;
  if (length == size)
    return 0;

  if (length == 0)
    return OS1_object_ctl(fd, OBJ_CTL_TRUNCATE, 0) < 0 ? -1 : 0;

  if (length < size) {
    char *buf = malloc((size_t)length);
    if (!buf) {
      errno = ENOMEM;
      return -1;
    }
    int r = -1;
    if (lseek(fd, 0, SEEK_SET) >= 0) {
      long n = read(fd, buf, (unsigned long)length);
      if (n >= 0 && lseek(fd, 0, SEEK_SET) >= 0 &&
          write(fd, buf, (size_t)n) >= 0)
        r = 0; /* offset-0 write replaced the file with n bytes */
    }
    free(buf);
    return r;
  }

  { /* extend: zero-fill from the old EOF up to `length` */
    long pad = length - size;
    char *z = calloc(1, (size_t)pad);
    if (!z) {
      errno = ENOMEM;
      return -1;
    }
    int r = (lseek(fd, size, SEEK_SET) >= 0 && write(fd, z, (size_t)pad) >= 0)
                ? 0
                : -1;
    free(z);
    return r;
  }
}

/* puts: writes string + newline to fd 1, matching the standard POSIX contract.
 */
int puts(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
  return 0;
}

/*
 * utf8_decode - decode the first UTF-8 codepoint from string s.
 *
 * s:    pointer to the start of a UTF-8 byte sequence (not necessarily NUL).
 * code: output parameter; receives the Unicode codepoint on success.
 *
 * Returns the number of bytes consumed (1–4), or 0 on invalid/null input.
 *
 * Encoding rules applied:
 *   0xxxxxxx (< 0x80)      : 1-byte ASCII
 *   110xxxxx 10xxxxxx      : 2-byte (U+0080..U+07FF)
 *   1110xxxx 10xxxxxx x2   : 3-byte (U+0800..U+FFFF)
 *   11110xxx 10xxxxxx x3   : 4-byte (U+10000..U+10FFFF)
 *
 * No validation of continuation bytes (0x3F mask is applied without checking
 * the 0x80 bit); malformed sequences may produce incorrect codepoints silently.
 *
 * Used by font_lib.c:font_draw_string() to iterate a UTF-8 string glyph by
 * glyph.
 */
int utf8_decode(const char *s, size_t len, uint32_t *code) {
  if (!s || !code || len == 0)
    return 0;
  unsigned char c = (unsigned char)s[0];

  if (c < 0x80) {
    *code = c;
    return 1;
  } else if ((c & 0xE0) == 0xC0) {
    if (len < 2 || (s[1] & 0xC0) != 0x80)
      return 0;
    *code = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
    if (*code < 0x80)
      return 0;
    return 2;
  } else if ((c & 0xF0) == 0xE0) {
    if (len < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
      return 0;
    *code = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) |
            (uint32_t)(s[2] & 0x3F);
    if (*code < 0x800)
      return 0;
    if (*code >= 0xD800 && *code <= 0xDFFF)
      return 0;
    return 3;
  } else if ((c & 0xF8) == 0xF0) {
    if (len < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
        (s[3] & 0xC0) != 0x80)
      return 0;
    *code = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
            ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
    if (*code < 0x10000)
      return 0;
    if (*code > 0x10FFFF)
      return 0;
    return 4;
  }
  return 0;
}

/* ============================================================================
 * POSIX compatibility shims — OS1 "onion userland" libc layer (epic #120).
 *
 * These complete the POSIX/libc surface entirely in userland over the OS1 base
 * API; NO new OS1 syscalls are added (the maintainer's directive: "il resto va
 * portato nella libc e in posix, non in os1").  strcat/strncat/memchr/atoi/...
 * live in kernel/lib/string.c (#included above); the functions here are the
 * remainder needed to build ported POSIX programs (base-nexs first, kilo/doom).
 * ========================================================================== */

/* --- <string.h> --- */
char *strtok_r(char *str, const char *delim, char **saveptr) {
  char *s = str ? str : *saveptr;
  if (!s)
    return NULL;
  while (*s && strchr(delim, (int)(unsigned char)*s))
    s++;
  if (*s == '\0') {
    *saveptr = s;
    return NULL;
  }
  char *tok = s;
  while (*s && !strchr(delim, (int)(unsigned char)*s))
    s++;
  if (*s) {
    *s = '\0';
    s++;
  }
  *saveptr = s;
  return tok;
}

char *strtok(char *str, const char *delim) {
  static char *saved_tok;
  return strtok_r(str, delim, &saved_tok);
}

char *strerror(int errnum) {
  /* os1_strerror (posix_types.h): single source of truth shared with the
   * kernel — this used to be its own 11-case switch that silently fell
   * back to "Unknown error" for anything past ENOSYS (EPERM, ESRCH,
   * EAGAIN, EBUSY, ... all missing). */
  return (char *)os1_strerror(errnum);
}

int strcoll(const char *s1, const char *s2) {
  if (!s1 && !s2)
    return 0;
  if (!s1)
    return -1;
  if (!s2)
    return 1;
  return strcmp(s1, s2);
}

size_t strxfrm(char *dest, const char *src, size_t n) {
  if (!src)
    return 0;
  size_t len = strlen(src);
  if (!dest || n == 0)
    return len;
  size_t copy = len < n - 1 ? len : n - 1;
  memcpy(dest, src, copy);
  dest[copy] = '\0';
  return len;
}

/* --- <stdlib.h> --- */
long atol(const char *nptr) { return strtol(nptr, NULL, 10); }

/* long is 64-bit on both OS1 targets (LP64), so long long shares its range. */
long long strtoll(const char *nptr, char **endptr, int base) {
  return (long long)strtol(nptr, endptr, base);
}

long long atoll(const char *nptr) { return strtoll(nptr, NULL, 10); }

/* strtoul / strtoull — unsigned variants of strtol */
unsigned long strtoul(const char *nptr, char **endptr, int base) {
  while (*nptr == ' ' || *nptr == '\t' || *nptr == '\n' ||
         *nptr == '\r' || *nptr == '\f' || *nptr == '\v')
    nptr++;
  int neg = 0;
  if (*nptr == '-') { neg = 1; nptr++; }
  else if (*nptr == '+') nptr++;
  if (base == 0) {
    if (nptr[0] == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) {
      base = 16; nptr += 2;
    } else if (nptr[0] == '0') {
      base = 8; nptr++;
    } else {
      base = 10;
    }
  } else if (base == 16 && nptr[0] == '0' &&
             (nptr[1] == 'x' || nptr[1] == 'X')) {
    nptr += 2;
  }
  unsigned long result = 0;
  const char *start = nptr;
  while (*nptr) {
    int digit;
    if (*nptr >= '0' && *nptr <= '9')      digit = *nptr - '0';
    else if (*nptr >= 'a' && *nptr <= 'z') digit = *nptr - 'a' + 10;
    else if (*nptr >= 'A' && *nptr <= 'Z') digit = *nptr - 'A' + 10;
    else break;
    if (digit >= base) break;
    result = result * (unsigned long)base + (unsigned long)digit;
    nptr++;
  }
  if (endptr) *endptr = (char *)(nptr == start ? start : nptr);
  return neg ? -result : result;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
  return (unsigned long long)strtoul(nptr, endptr, base);
}

long labs(long j) { return j < 0 ? -j : j; }


void abort(void) { exit(1); }

/* qsort: in-place insertion sort with byte-wise swaps (no temp buffer / VLA,
 * no recursion).  O(n^2) but adequate for the small arrays NEXS sorts. */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
  char *arr = (char *)base;
  for (size_t i = 1; i < nmemb; i++) {
    for (size_t j = i;
         j > 0 && compar(arr + (j - 1) * size, arr + j * size) > 0; j--) {
      char *a = arr + (j - 1) * size;
      char *b = arr + j * size;
      for (size_t k = 0; k < size; k++) {
        char t = a[k];
        a[k] = b[k];
        b[k] = t;
      }
    }
  }
}

/* --- <stdio.h> ---
 * Standard stream handles.
 */
FILE _stdin_struct = {.fd = 0};
FILE _stdout_struct = {.fd = 1};
FILE _stderr_struct = {.fd = 2};

/* file_fd - the descriptor behind a stream, or -1 for a positional
 * (path-backed) one.  The three console streams carry fd 0/1/2 in their own
 * structs and fopen() sets fd = -1 to select path I/O, so returning the field
 * is EXACTLY equivalent to the old stdin/stdout/stderr pointer comparison —
 * and additionally makes fdopen()'d streams (e.g. a pipe end wrapped in a
 * FILE*) route through read()/write() like any other descriptor. */
static int file_fd(FILE *fp) { return fp ? fp->fd : -1; }

/*
 * fputc/fputs/fgetc delegate to fwrite/fread rather than calling write()/read()
 * on the descriptor themselves.
 *
 * They used to take an `if (fd >= 0)` shortcut straight to the syscall.  That
 * was correct while only the console had a descriptor and a path-backed FILE
 * had none — but a FILE now keeps an open handle for its lifetime, so `fd >= 0`
 * became true for ordinary files too and the shortcut started bypassing the
 * stream buffers.  The consequences were not symmetric and neither was
 * cosmetic:
 *
 *   - fgetc read one byte straight from the descriptor and never advanced
 *     fp->pos.  A reader that mixes getc() with fread() — which is exactly what
 *     Lua's loader does when it skips a `#!` line — desynchronised: fread then
 *     refilled from the stale fp->pos and re-read the file from the beginning.
 *   - fputc/fputs wrote straight through while buffered bytes were still
 *     pending in wbuf, so the two landed out of order: silent corruption, not a
 *     visible failure.
 *
 * fread/fwrite already own the one correct rule — the descriptor fast path is
 * for CONSOLE/pipe streams only (`path[0] == '\0'`), never for a file — so
 * these three defer to it instead of carrying a second, older copy of the same
 * decision.  ext4's 4 KiB read-modify-write granularity is why that rule
 * exists; see the comment in fwrite.
 */
int fputc(int c, FILE *fp) {
  unsigned char ch = (unsigned char)c;
  if (fwrite(&ch, 1, 1, fp) != 1)
    return EOF;
  return (int)ch;
}

int fputs(const char *s, FILE *fp) {
  size_t len = strlen(s);
  if (len && fwrite(s, 1, len, fp) != len)
    return EOF;
  return 0;
}

int fgetc(FILE *fp) {
  if (!fp)
    return EOF;
  if (fp->has_ungetc) {
    fp->has_ungetc = 0;
    return fp->ungetc_buf;
  }
  unsigned char ch;
  if (fread(&ch, 1, 1, fp) != 1)
    return EOF;
  return (int)ch;
}

char *fgets(char *s, int size, FILE *fp) {
  /* R7/defensive: fgetc() already turns a NULL fp into a clean EOF, but a
   * NULL destination buffer would still fault on the first s[i++] write
   * below — guard it explicitly rather than relying on that being
   * "probably fine" because no caller in this tree does it today. */
  if (!s || size <= 0)
    return NULL;
  int i = 0;
  /* R2: bounded by `size - 1`, provably terminating — each iteration either
   * consumes one byte toward that bound or returns/breaks on EOF/newline. */
  while (i < size - 1) {
    int c = fgetc(fp);
    if (c == EOF) {
      if (i == 0)
        return NULL;
      break;
    }
    s[i++] = (char)c;
    if (c == '\n')
      break;
  }
  s[i] = '\0';
  return s;
}

int ungetc(int c, FILE *fp) {
  if (!fp || c == EOF)
    return EOF;
  fp->ungetc_buf = c;
  fp->has_ungetc = 1;
  fp->eof = 0;
  return c;
}

/*
 * rewind - POSIX: equivalent to fseek(fp, 0, SEEK_SET) except that it also
 * clears the error indicator and returns nothing.  Added because it was the one
 * standard stdio entry point missing from this libc, and code ported from POSIX
 * reaches for it after a failed read; without it the link failed rather than
 * the call misbehaving, which is why it went unnoticed.
 */
void rewind(FILE *fp) {
  if (!fp)
    return;
  /* R7: rewind() is `void` by its own POSIX contract, which additionally
   * mandates clearing the error indicator UNCONDITIONALLY — not only on
   * success — so fseek()'s return has nothing left for this function to do
   * with it either way; the discard is the specified behaviour, not a
   * missed check. */
  (void)fseek(fp, 0, SEEK_SET);
  fp->error = 0;
}

int setvbuf(FILE *fp, char *buf, int mode, size_t size) {
  (void)fp;
  (void)buf;
  (void)mode;
  (void)size;
  return 0;
}

FILE *tmpfile(void) {
  static int temp_counter = 0;
  char path[128];
  sprintf(path, "/tmpfile_%d", temp_counter++);
  FILE *f = fopen(path, "w+");
  if (f) {
    f->is_tmp = 1;
  }
  return f;
}

void perror(const char *s) {
  if (s && *s)
    printf("%s: %s\n", s, strerror(errno));
  else
    printf("%s\n", strerror(errno));
}

/* --- <termios.h> --- OS1 windows have no line discipline; raw mode is the
 * native behaviour, so these succeed as no-ops (see include/api/termios.h). */
int tcgetattr(int fd, struct termios *termios_p) {
  (void)fd;
  if (termios_p)
    memset(termios_p, 0, sizeof(*termios_p));
  return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
  (void)fd;
  (void)optional_actions;
  (void)termios_p;
  return 0;
}

/* --- <poll.h> --- no pollable fd set; report a clean timeout (0 ready). */
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
  (void)timeout;
  if (fds)
    for (nfds_t i = 0; i < nfds; i++)
      fds[i].revents = 0;
  return 0;
}

/* --- <signal.h> --- OS1 has no signal delivery; the libc accepts handlers and
 * masks but leaves all process semantics unchanged. */

int sigemptyset(sigset_t *set) {
  if (!set) {
    errno = EINVAL;
    return -1;
  }
  *set = 0;
  return 0;
}

int sigfillset(sigset_t *set) {
  if (!set) {
    errno = EINVAL;
    return -1;
  }
  *set = ~((sigset_t)0);
  return 0;
}

int sigaddset(sigset_t *set, int sig) {
  if (!set || sig < 1 || sig >= NSIG) {
    errno = EINVAL;
    return -1;
  }
  *set |= ((sigset_t)1U << (sig - 1));
  return 0;
}

int sigdelset(sigset_t *set, int sig) {
  if (!set || sig < 1 || sig >= NSIG) {
    errno = EINVAL;
    return -1;
  }
  *set &= ~((sigset_t)1U << (sig - 1));
  return 0;
}

int sigismember(const sigset_t *set, int sig) {
  if (!set || sig < 1 || sig >= NSIG) {
    errno = EINVAL;
    return -1;
  }
  return ((*set & ((sigset_t)1U << (sig - 1))) != 0) ? 1 : 0;
}

int sigprocmask(int how, const sigset_t *restrict set, sigset_t *restrict oldset) {
  (void)how;
  if (oldset)
    *oldset = 0;
  if (set)
    (void)set;
  return 0;
}

int sigaction(int signum, const struct sigaction *restrict act,
              struct sigaction *restrict oldact) {
  (void)signum;
  if (oldact)
    memset(oldact, 0, sizeof(*oldact));
  (void)act;
  return 0;
}

sighandler_t signal(int signum, sighandler_t handler) {
  (void)signum;
  (void)handler;
  return SIG_DFL;
}

/* --- <sys/mman.h> --- anonymous mappings are backed by the userspace heap. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd,
           off_t offset) {
  (void)addr;
  (void)prot;
  (void)flags;
  (void)fd;
  (void)offset;
  if (length == 0)
    return MAP_FAILED;
  void *p = malloc(length);
  return p ? p : MAP_FAILED;
}

int munmap(void *addr, size_t length) {
  (void)length;
  free(addr);
  return 0;
}

/* --- <sys/ioctl.h> --- only TIOCGWINSZ, answered from the window grid. */
int ioctl(int fd, unsigned long request, ...) {
  (void)fd;
  if (request == TIOCGWINSZ) {
    va_list ap;
    va_start(ap, request);
    struct winsize *ws = va_arg(ap, struct winsize *);
    va_end(ap);
    int cols = 0, rows = 0;
    int wid = window_of_pid(get_pid());
    if (ws && wid > 0 && window_grid(wid, &cols, &rows) == 0) {
      ws->ws_col = (unsigned short)cols;
      ws->ws_row = (unsigned short)rows;
      ws->ws_xpixel = 0;
      ws->ws_ypixel = 0;
      return 0;
    }
  }
  return -1;
}

/* --- <dirent.h> --- over list_dir() (space-separated names from ext4_list). */
DIR *opendir(const char *name) {
  /* Verify it IS a directory before listing.  opendir() used to rely on
   * list_dir failing for a regular file; since a directory is now read through
   * the same object path as a file, listing a FILE returns its content and the
   * call would "succeed" on anything.  Callers use opendir() as the
   * file-or-directory test (the file manager does), so this must be exact. */
  struct abi_stat as;
  if (_sys_stat(name, &as) != 0 || as.type != ABI_S_TYPE_DIR) {
    errno = ENOTDIR;
    return NULL;
  }
  DIR *d = malloc(sizeof(DIR));
  if (!d)
    return NULL;
  if (list_dir(name, d->buf, sizeof(d->buf)) < 0) {
    free(d);
    return NULL;
  }
  d->pos = 0;
  return d;
}

struct dirent *readdir(DIR *dirp) {
  if (!dirp)
    return NULL;
  while (dirp->buf[dirp->pos] == ' ')
    dirp->pos++;
  if (dirp->buf[dirp->pos] == '\0')
    return NULL;
  int i = 0;
  while (dirp->buf[dirp->pos] != ' ' && dirp->buf[dirp->pos] != '\0' &&
         i < (int)sizeof(dirp->ent.d_name) - 1)
    dirp->ent.d_name[i++] = dirp->buf[dirp->pos++];
  dirp->ent.d_name[i] = '\0';
  dirp->ent.d_ino = 0;
  dirp->ent.d_type = DT_UNKNOWN;
  return &dirp->ent;
}

int closedir(DIR *dirp) {
  free(dirp);
  return 0;
}

size_t strspn(const char *s, const char *accept) {
  const char *p = s;
  while (*p) {
    const char *a = accept;
    while (*a && *a != *p) {
      a++;
    }
    if (*a == '\0') {
      break;
    }
    p++;
  }
  return p - s;
}

size_t strcspn(const char *s, const char *reject) {
  const char *p = s;
  while (*p) {
    const char *r = reject;
    while (*r) {
      if (*p == *r)
        return p - s;
      r++;
    }
    p++;
  }
  return p - s;
}

char *strpbrk(const char *s, const char *accept) {
  while (*s) {
    const char *a = accept;
    while (*a) {
      if (*a == *s) {
        return (char *)s;
      }
      a++;
    }
    s++;
  }
  return NULL;
}

#include <sys/utsname.h>

/* POSIX compatibility: host identifiers are not meaningful in a single-user
 * freestanding environment, but some GNU/Coreutils applets such as `hostid`
 * call the API unconditionally.  Return a stable zero ID instead of exposing
 * an undefined symbol at link time. */
long gethostid(void) { return 0; }

int uname(struct utsname *buf) {
  if (!buf) {
    errno = EFAULT;
    return -1;
  }
  strncpy(buf->sysname, "NexsOS1", sizeof(buf->sysname) - 1);
  buf->sysname[sizeof(buf->sysname) - 1] = '\0';

  strncpy(buf->nodename, "nexsos", sizeof(buf->nodename) - 1);
  buf->nodename[sizeof(buf->nodename) - 1] = '\0';

  strncpy(buf->release, "0.0.5.5", sizeof(buf->release) - 1);
  buf->release[sizeof(buf->release) - 1] = '\0';

  strncpy(buf->version, "NexsOS1-V0.0.5.5", sizeof(buf->version) - 1);
  buf->version[sizeof(buf->version) - 1] = '\0';

#if defined(ARCH_AMD64) || defined(__x86_64__)
  strncpy(buf->machine, "x86_64", sizeof(buf->machine) - 1);
#else
  strncpy(buf->machine, "aarch64", sizeof(buf->machine) - 1);
#endif
  buf->machine[sizeof(buf->machine) - 1] = '\0';

  return 0;
}

#include <locale.h>

static struct lconv s_posix_lconv = {
    .decimal_point = (char *)".",
    .thousands_sep = (char *)"",
    .grouping = (char *)"",
    .int_curr_symbol = (char *)"",
    .currency_symbol = (char *)"",
    .mon_decimal_point = (char *)"",
    .mon_thousands_sep = (char *)"",
    .mon_grouping = (char *)"",
    .positive_sign = (char *)"",
    .negative_sign = (char *)"",
    .int_frac_digits = 127,
    .frac_digits = 127,
    .p_cs_precedes = 127,
    .p_sep_by_space = 127,
    .n_cs_precedes = 127,
    .n_sep_by_space = 127,
    .p_sign_posn = 127,
    .n_sign_posn = 127,
    .int_p_cs_precedes = 127,
    .int_p_sep_by_space = 127,
    .int_n_cs_precedes = 127,
    .int_n_sep_by_space = 127,
    .int_p_sign_posn = 127,
    .int_n_sign_posn = 127,
};

char *setlocale(int category, const char *locale) {
  (void)category;
  if (!locale || !*locale || strcmp(locale, "C") == 0 ||
      strcmp(locale, "POSIX") == 0)
    return (char *)"C";
  return (char *)"C";
}

struct lconv *localeconv(void) { return &s_posix_lconv; }

#include <wchar.h>
#include <wctype.h>

static int wctype_value(wint_t wc, const char *property) {
  if (!property)
    return 0;
  if (strcmp(property, "alnum") == 0) return iswalnum(wc);
  if (strcmp(property, "alpha") == 0) return iswalpha(wc);
  if (strcmp(property, "blank") == 0) return iswblank(wc);
  if (strcmp(property, "cntrl") == 0) return iswcntrl(wc);
  if (strcmp(property, "digit") == 0) return iswdigit(wc);
  if (strcmp(property, "graph") == 0) return iswgraph(wc);
  if (strcmp(property, "lower") == 0) return iswlower(wc);
  if (strcmp(property, "print") == 0) return iswprint(wc);
  if (strcmp(property, "punct") == 0) return iswpunct(wc);
  if (strcmp(property, "space") == 0) return iswspace(wc);
  if (strcmp(property, "upper") == 0) return iswupper(wc);
  if (strcmp(property, "xdigit") == 0) return iswxdigit(wc);
  if (strcmp(property, "any") == 0) return 1;
  return 0;
}

int iswalnum(wint_t wc) { return isalnum((unsigned char)wc); }
int iswalpha(wint_t wc) { return isalpha((unsigned char)wc); }
int iswblank(wint_t wc) { return isblank((unsigned char)wc); }
int iswcntrl(wint_t wc) { return iscntrl((unsigned char)wc); }
int iswdigit(wint_t wc) { return isdigit((unsigned char)wc); }
int iswgraph(wint_t wc) { return isgraph((unsigned char)wc); }
int iswlower(wint_t wc) { return islower((unsigned char)wc); }
int iswprint(wint_t wc) { return isprint((unsigned char)wc); }
int iswpunct(wint_t wc) { return ispunct((unsigned char)wc); }
int iswspace(wint_t wc) { return isspace((unsigned char)wc); }
int iswupper(wint_t wc) { return isupper((unsigned char)wc); }
int iswxdigit(wint_t wc) { return isxdigit((unsigned char)wc); }

wint_t towlower(wint_t wc) { return (wint_t)tolower((unsigned char)wc); }
wint_t towupper(wint_t wc) { return (wint_t)toupper((unsigned char)wc); }

wctype_t wctype(const char *property) {
  if (!property)
    return 0;
  return (wctype_t)(uintptr_t)property;
}

int iswctype(wint_t wc, wctype_t desc) {
  if (!desc)
    return 0;
  const char *property = (const char *)(uintptr_t)desc;
  return wctype_value(wc, property);
}

int c32isalnum(wint_t wc) { return iswalpha(wc) || iswdigit(wc); }
int c32isalpha(wint_t wc) { return iswalpha(wc); }
int c32isblank(wint_t wc) { return iswblank(wc); }
int c32iscntrl(wint_t wc) { return iswcntrl(wc); }
int c32isdigit(wint_t wc) { return iswdigit(wc); }
int c32isgraph(wint_t wc) { return iswgraph(wc); }
int c32islower(wint_t wc) { return iswlower(wc); }
int c32isprint(wint_t wc) { return iswprint(wc); }
int c32ispunct(wint_t wc) { return iswpunct(wc); }
int c32isspace(wint_t wc) { return iswspace(wc); }
int c32isupper(wint_t wc) { return iswupper(wc); }
int c32isxdigit(wint_t wc) { return iswxdigit(wc); }

wint_t c32tolower(wint_t wc) { return towlower(wc); }
wint_t c32toupper(wint_t wc) { return towupper(wc); }

size_t c32rtomb(char *s, char32_t wc, mbstate_t *ps) {
  (void)ps;
  if (!s) return 1;
  if (wc == 0) { s[0] = '\0'; return 1; }
  if (wc <= 0x7f) { s[0] = (char)wc; s[1] = '\0'; return 1; }
  s[0] = '?'; s[1] = '\0'; return 1;
}

size_t wcslen(const wchar_t *s) {
  size_t len = 0;
  if (!s)
    return 0;
  while (s[len])
    len++;
  return len;
}

wchar_t *wcscpy(wchar_t *dest, const wchar_t *src) {
  wchar_t *d = dest;
  while ((*d++ = *src++)) {
  }
  return dest;
}

wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i]; i++)
    dest[i] = src[i];
  for (; i < n; i++)
    dest[i] = 0;
  return dest;
}

int wcscmp(const wchar_t *s1, const wchar_t *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(const unsigned int *)s1 - *(const unsigned int *)s2;
}

int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (s1[i] != s2[i] || s1[i] == 0)
      return *(const unsigned int *)(s1 + i) - *(const unsigned int *)(s2 + i);
  }
  return 0;
}

wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n) {
  return (wchar_t *)memcpy(dest, src, n * sizeof(wchar_t));
}

wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n) {
  for (size_t i = 0; i < n; i++)
    s[i] = c;
  return s;
}

wint_t btowc(int c) {
  if (c >= 0 && c <= 127)
    return (wint_t)c;
  return WEOF;
}

int wctob(wint_t c) {
  if (c <= 127)
    return (int)c;
  return EOF;
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps) {
  (void)ps;
  if (!s || n == 0)
    return 0;
  if (*s == '\0') {
    if (pwc)
      *pwc = 0;
    return 0;
  }
  unsigned char c = (unsigned char)*s;
  if (c < 0x80) {
    if (pwc)
      *pwc = c;
    return 1;
  }
  if (pwc)
    *pwc = c;
  return 1;
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps) {
  (void)ps;
  if (!s)
    return 1;
  if (wc <= 0x7f) {
    *s = (char)wc;
    return 1;
  }
  *s = (char)(wc & 0xff);
  return 1;
}

int wcwidth(wchar_t wc) {
  if (wc == 0)
    return 0;
  if (wc < 0x20 || (wc >= 0x7f && wc < 0xa0))
    return -1;
  return 1;
}

void *reallocarray(void *ptr, size_t nmemb, size_t size) {
  if (nmemb > 0 && size > (size_t)-1 / nmemb) {
    errno = ENOMEM;
    return NULL;
  }
  return realloc(ptr, nmemb * size);
}

#include <sys/time.h>

int gettimeofday(struct timeval *tv, struct timezone *tz) {
  (void)tz;
  if (!tv) {
    errno = EFAULT;
    return -1;
  }
  uint64_t ns = os1_mono_ns();
  tv->tv_sec = (time_t)(ns / 1000000000ULL);
  tv->tv_usec = (long)((ns % 1000000000ULL) / 1000ULL);
  return 0;
}

int lstat(const char *path, struct stat *buf) { return stat(path, buf); }

int fstat(int fd, struct stat *buf) {
  if (fd < 0 || !buf) {
    errno = EBADF;
    return -1;
  }
  long size = lseek(fd, 0, SEEK_END);
  if (size < 0) {
    errno = EBADF;
    return -1;
  }
  memset(buf, 0, sizeof(*buf));
  buf->st_mode = S_IFREG | 0644;
  buf->st_size = size;
  buf->st_nlink = 1;
  return 0;
}

int dirfd(DIR *dirp) {
  (void)dirp;
  return -1;
}

int fchdir(int fd) {
  (void)fd;
  errno = ENOSYS;
  return -1;
}

unsigned int sleep(unsigned int seconds) {
  OS1_sleep((unsigned long)seconds * 1000UL);
  return 0;
}

char *stpcpy(char *dest, const char *src) {
  while ((*dest = *src)) {
    dest++;
    src++;
  }
  return dest;
}

#include <fcntl.h>

int fcntl(int fd, int cmd, ...) {
  (void)fd;
  (void)cmd;
  return 0;
}

ssize_t copy_file_range(int fd_in, off_t *off_in, int fd_out, off_t *off_out,
                        size_t len, unsigned int flags) {
  (void)fd_in;
  (void)off_in;
  (void)fd_out;
  (void)off_out;
  (void)len;
  (void)flags;
  errno = ENOSYS;
  return -1;
}

int getpagesize(void) { return 4096; }

#include <sys/uio.h>

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
  if (iovcnt < 0 || !iov) {
    errno = EINVAL;
    return -1;
  }
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0)
      continue;
    if (!iov[i].iov_base) {
      errno = EFAULT;
      return -1;
    }
    long w = write(fd, iov[i].iov_base, iov[i].iov_len);
    if (w < 0) {
      return total > 0 ? total : -1;
    }
    total += w;
    if ((size_t)w < iov[i].iov_len)
      break;
  }
  return total;
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
  if (iovcnt < 0 || !iov) {
    errno = EINVAL;
    return -1;
  }
  ssize_t total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0)
      continue;
    if (!iov[i].iov_base) {
      errno = EFAULT;
      return -1;
    }
    long r = read(fd, iov[i].iov_base, iov[i].iov_len);
    if (r <= 0) {
      return total > 0 ? total : r;
    }
    total += r;
    if ((size_t)r < iov[i].iov_len)
      break;
  }
  return total;
}

static mode_t g_current_umask = 022;

mode_t umask(mode_t mask) {
  mode_t old = g_current_umask;
  g_current_umask = mask & 0777;
  return old;
}

int chmod(const char *path, mode_t mode) {
  (void)path;
  (void)mode;
  return 0;
}

int fchmod(int fd, mode_t mode) {
  (void)fd;
  (void)mode;
  return 0;
}

int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags) {
  (void)flags;
  /* NexsOS1 doesn't have fd-relative stat, so only support AT_FDCWD */
  if (dirfd != AT_FDCWD) {
    errno = ENOTSUP;
    return -1;
  }
  return stat(pathname, statbuf);
}

int fchmodat(int dirfd, const char *pathname, mode_t mode, int flags) {
  (void)flags;
  /* NexsOS1 doesn't have fd-relative chmod, so only support AT_FDCWD */
  if (dirfd != AT_FDCWD) {
    errno = ENOTSUP;
    return -1;
  }
  return chmod(pathname, mode);
}

int raise(int sig) {
  (void)sig;
  return 0;
}

pid_t fork(void) {
  errno = ENOSYS;
  return -1;
}

int chown(const char *path, uid_t owner, gid_t group) {
  (void)path;
  (void)owner;
  (void)group;
  return 0;
}

int lchown(const char *path, uid_t owner, gid_t group) {
  (void)path;
  (void)owner;
  (void)group;
  return 0;
}

int fchown(int fd, uid_t owner, gid_t group) {
  (void)fd;
  (void)owner;
  (void)group;
  return 0;
}

int chownat(int dirfd, const char *pathname, uid_t owner, gid_t group) {
  return fchownat(dirfd, pathname, owner, group, 0);
}

int lchownat(int dirfd, const char *pathname, uid_t owner, gid_t group) {
  return fchownat(dirfd, pathname, owner, group, AT_SYMLINK_NOFOLLOW);
}

int fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group, int flags) {
  (void)owner;
  (void)group;
  (void)flags;
  if (dirfd != AT_FDCWD) {
    errno = ENOTSUP;
    return -1;
  }
  return chown(pathname, owner, group);
}

int lchmod(const char *path, mode_t mode) { return chmod(path, mode); }

int rmdir(const char *pathname) { return unlink(pathname); }

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
  (void)path;
  (void)buf;
  (void)bufsiz;
  errno = EINVAL;
  return -1;
}

int openat(int dirfd, const char *pathname, int flags, ...) {
  (void)dirfd;
  return open(pathname, flags);
}

DIR *fdopendir(int fd) {
  (void)fd;
  errno = ENOSYS;
  return NULL;
}

int fsync(int fd) {
  (void)fd;
  return 0;
}

int fdatasync(int fd) {
  (void)fd;
  return 0;
}

void sync(void) { /* NexsOS1: no-op — VFS flushes are synchronous */ }

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }

static char _pw_name[] = "root";
static char _pw_passwd[] = "";
static char _pw_gecos[] = "NexsOS Administrator";
static char _pw_dir[] = "/home";
static char _pw_shell[] = "sys/bin/nxshell";

static char _gr_name[] = "root";
static char _gr_passwd[] = "";
static char * _gr_mem[] = {NULL};

static struct passwd _nexs_root_pw = {.pw_name = _pw_name,
                                      .pw_passwd = _pw_passwd,
                                      .pw_uid = 0,
                                      .pw_gid = 0,
                                      .pw_gecos = _pw_gecos,
                                      .pw_dir = _pw_dir,
                                      .pw_shell = _pw_shell};

static struct group _nexs_root_gr = {.gr_name = _gr_name,
                                    .gr_passwd = _gr_passwd,
                                    .gr_gid = 0,
                                    .gr_mem = _gr_mem};

struct passwd *getpwuid(uid_t uid) {
  if (uid == 0)
    return &_nexs_root_pw;
  errno = ENOENT;
  return NULL;
}

struct passwd *getpwnam(const char *name) {
  if (name && strcmp(name, "root") == 0)
    return &_nexs_root_pw;
  errno = ENOENT;
  return NULL;
}

struct group *getgrgid(gid_t gid) {
  if (gid == 0)
    return &_nexs_root_gr;
  errno = ENOENT;
  return NULL;
}

struct group *getgrnam(const char *name) {
  if (name && strcmp(name, "root") == 0)
    return &_nexs_root_gr;
  errno = ENOENT;
  return NULL;
}

void setpwent(void) {}
struct passwd *getpwent(void) { return &_nexs_root_pw; }
void endpwent(void) {}
void setgrent(void) {}
struct group *getgrent(void) { return &_nexs_root_gr; }
void endgrent(void) {}

void *memrchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s + n;
  unsigned char uc = (unsigned char)c;
  while (n--) {
    if (*--p == uc)
      return (void *)p;
  }
  return NULL;
}

void *mempcpy(void *dest, const void *src, size_t n) {
  return (char *)memcpy(dest, src, n) + n;
}

void *rawmemchr(const void *s, int c) {
  const unsigned char *p = (const unsigned char *)s;
  unsigned char uc = (unsigned char)c;
  while (*p != uc)
    p++;
  return (void *)p;
}

char *nl_langinfo(nl_item item) {
  static char buf[32];
  (void)item;
  snprintf(buf, sizeof(buf), "C");
  return buf;
}

int dup(int oldfd) {
  if (oldfd < 0) {
    errno = EBADF;
    return -1;
  }
  return oldfd;
}

int dup2(int oldfd, int newfd) {
  if (oldfd < 0 || newfd < 0) {
    errno = EBADF;
    return -1;
  }
  return newfd;
}

int access(const char *pathname, int mode) {
  struct stat buf;
  (void)mode;
  if (!pathname || stat(pathname, &buf) != 0) {
    return -1;
  }
  return 0;
}

/* strtoimax / strtoumax — delegate to strtoll/strtoull */
intmax_t strtoimax(const char *nptr, char **endptr, int base) {
  return (intmax_t)strtoll(nptr, endptr, base);
}

uintmax_t strtoumax(const char *nptr, char **endptr, int base) {
  return (uintmax_t)strtoull(nptr, endptr, base);
}

/* ── Process / signal stubs ─────────────────────────────────────────── */
pid_t getppid(void) { return 1; }

int kill(pid_t pid, int sig) {
  (void)pid; (void)sig;
  errno = EPERM;
  return -1;
}

int setuid(uid_t uid)  { (void)uid;  return 0; }
int seteuid(uid_t uid) { (void)uid;  return 0; }
int setgid(gid_t gid)  { (void)gid;  return 0; }
int setegid(gid_t gid) { (void)gid;  return 0; }

int setreuid(uid_t ruid, uid_t euid) { (void)ruid; (void)euid; return 0; }
int setregid(gid_t rgid, gid_t egid) { (void)rgid; (void)egid; return 0; }

int getgroups(int size, gid_t list[]) {
  if (size >= 1) list[0] = 0;
  return 1;
}

/* ── Terminal / tty stubs ───────────────────────────────────────────── */
char *ttyname(int fd) {
  (void)fd;
  return (char *)"/dev/tty";
}

int ttyname_r(int fd, char *buf, size_t buflen) {
  (void)fd;
  if (buflen < 9) return ERANGE;
  strncpy(buf, "/dev/tty", buflen - 1);
  buf[buflen - 1] = '\0';
  return 0;
}

char *getlogin(void) { return (char *)"root"; }

int getlogin_r(char *buf, size_t bufsize) {
  if (bufsize < 5) return ERANGE;
  strncpy(buf, "root", bufsize - 1);
  buf[bufsize - 1] = '\0';
  return 0;
}

/* ── Misc POSIX stubs ───────────────────────────────────────────────── */
long sysconf(int name) {
  switch (name) {
  case 84: /* _SC_NPROCESSORS_ONLN */ return 1;
  case 83: /* _SC_NPROCESSORS_CONF */ return 1;
  case 30: /* _SC_PAGESIZE */         return 4096;
  case 2:  /* _SC_CLK_TCK */          return 100;
  case 5:  /* _SC_OPEN_MAX */         return 256;
  case 71: /* _SC_LOGIN_NAME_MAX */   return 256;
  case 72: /* _SC_HOST_NAME_MAX */    return 256;
  default: errno = EINVAL; return -1;
  }
}

unsigned int alarm(unsigned int seconds) { (void)seconds; return 0; }
int pause(void) { errno = EINTR; return -1; }
int nice(int inc) { (void)inc; return 0; }

/* ==========================================================================
 * Timestamp support (utime family) — required by GNU Coreutils `touch`
 * ========================================================================== */

/* Definiamo qui le strutture perché NexsOS1 non ha ancora <utime.h> */

struct utimbuf {
    time_t actime;   /* access time */
    time_t modtime;  /* modification time */
};

int utime(const char *path, const struct utimbuf *times);
int utimes(const char *path, const struct timeval times[2]);
int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);

/*
 * utime - set access and modification times of a file.
 * times == NULL → set both to current time.
 */
__attribute__((weak))
int utime(const char *path, const struct utimbuf *times)
{
    if (!path) {
        errno = EFAULT;
        return -1;
    }

    /* Assicuriamoci che il file esista (comportamento di touch) */
    struct stat st;
    if (stat(path, &st) != 0) {
        int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0)
            return -1;
        close(fd);
    }

    (void)times;   /* il filesystem non salva ancora i timestamp */
    return 0;
}

__attribute__((weak))
int utimes(const char *path, const struct timeval times[2])
{
    if (!path) {
        errno = EFAULT;
        return -1;
    }

    struct utimbuf buf;
    if (times) {
        buf.actime  = times[0].tv_sec;
        buf.modtime = times[1].tv_sec;
    } else {
        time_t now = time(NULL);
        buf.actime  = now;
        buf.modtime = now;
    }
    return utime(path, times ? &buf : NULL);
}

__attribute__((weak))
int utimensat(int dirfd, const char *path,
              const struct timespec times[2], int flags)
{
    (void)flags;

    if (dirfd != AT_FDCWD) {
        errno = ENOTSUP;
        return -1;
    }
    if (!path) {
        errno = EFAULT;
        return -1;
    }

    struct timeval tv[2];
    if (times) {
        tv[0].tv_sec  = times[0].tv_sec;
        tv[0].tv_usec = times[0].tv_nsec / 1000;
        tv[1].tv_sec  = times[1].tv_sec;
        tv[1].tv_usec = times[1].tv_nsec / 1000;
    }
    return utimes(path, times ? tv : NULL);
}
__attribute__((weak))
int futimens(int fd, const struct timespec times[2])
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    (void)times;
    return 0;
}