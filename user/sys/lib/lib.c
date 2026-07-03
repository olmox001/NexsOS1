/*
 * user/sys/lib/lib.c
 * Userland C runtime and system call wrapper library
 *
 * This file is the sole C runtime for all userland processes.  It is
 * compiled into lib.o and linked into every ELF (there is no shared library
 * mechanism).  It provides:
 *
 *   - Thin C wrappers around every _sys_*() assembly stub from syscall.S.
 *   - Standard I/O emulation (fopen/fclose/fread/fwrite/fseek/ftell) backed
 *     by file_read/file_write syscalls.
 *   - Formatting (printf, snprintf, sprintf, vsnprintf, vsscanf, sscanf).
 *   - Input event decoding (input_poll_event: keyboard and mouse IPC msgs).
 *   - Graphics helpers (graphics_draw_rect, graphics_blit, graphics_draw_text,
 *     graphics_text_width, graphics_load_image).
 *   - Partial POSIX-like shims (strdup, strtol, abs, fabs, atof, getenv,
 *     mkdir, system, stat, puts, fflush, remove, rename, vfprintf).
 *   - UTF-8 decoder (utf8_decode).
 *   - Stack smash protector stub (__stack_chk_guard, __stack_chk_fail).
 *
 * STB libraries (NOTE USR-BLOAT-01/02):
 *   STB_IMAGE_IMPLEMENTATION is compiled unconditionally here. Text rendering
 *   now uses the OS1 packed bitmap font path instead of stb_easy_font.
 *
 * Kernel source inclusion (NOTE USR-LIB-01):
 *   vsnprintf.c, math.c, string.c are sourced directly from kernel/lib/ via
 *   relative #include paths.  Any internal change to those kernel files
 *   silently changes userland behaviour.
 *
 * Known issues:
 *   USR-LIB-01  (W2 BAD-IMPL) Directly #includes kernel/lib C sources;
 *               breaks the userland/kernel boundary.
 *   USR-LIB-02  (W2 BAD-IMPL) fclose() guards against NULL with
 *               `(size_t)fp > 10`, a fragile magic-value check.
 *   USR-LIB-03  (fixed locally) graphics_draw_text used to declare a 100KB
 *               static buffer and fall back to terminal text rendering.
 *   USR-LIB-04  (W1 STUB) mkdir/system/getenv are no-ops; atof truncates
 *               decimal fractions via (double)atoi().
 *   USR-LIB-05  (W1 DOC) vfprintf ignores the stream arg and always writes
 *               to fd 1; stderr goes to stdout silently.
 *   USR-SEC-01  (W3 SECURITY) registry_read/write have no authentication;
 *               any process can overwrite any key.
 *   USR-SEC-02  (W3 SECURITY) send()/kill_process() accept arbitrary PIDs
 *               with no capability check.
 *   USR-BLOAT-01 (W2 BAD-IMPL·PERF) STB libs always compiled in; no gc-sections.
 *   USR-BLOAT-02 (W2 BAD-IMPL) -g DWARF retained in every ELF; not stripped.
 */
#include <os1.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <input.h>
#include <graphics.h>
#include <errno.h>
/* POSIX compatibility shims implemented at the bottom of this file (the OS1
 * onion-userland libc layer, epic #120; no new OS1 syscalls). */
#include <termios.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-braces"
/*
 * STB_IMAGE_IMPLEMENTATION: embed the full stb_image decoder.
 * STBI_NO_STDIO/LINEAR/HDR disable file-I/O and HDR format support that
 * are unavailable or unnecessary in a freestanding environment.
 * STBI_NO_THREAD_LOCALS/STBI_NO_FAILURE_STRINGS are required by OS1 userland:
 * there is no initialized TLS block behind TPIDR_EL0, so stb's __thread failure
 * state would fault in ordinary decoder error paths.
 * NOTE(USR-BLOAT-01): Adds ~50KB of .text to every ELF regardless of use.
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

/* errno: global error variable expected by POSIX-style libc callers.
 * Not set by any syscall wrapper currently; placeholder only. */
int errno = 0;

/* --- Syscall Wrappers ---
 * Each function below is a thin C-callable veneer over an assembly stub in
 * user/arch/<arch>/syscall.S.  Arguments are passed in the arch ABI registers
 * (x0-x5 on AArch64, rdi/rsi/rdx/r10/r8/r9 on x86-64) by the C compiler;
 * the stub moves the syscall number into x8/rax and issues svc/syscall.
 *
 * NOTE(USR-SEC-02): send(), kill_process(), and spawn() accept arbitrary PIDs
 * and paths with no capability check; any process has full authority.
 */
long read(int fd, char *buf, unsigned long count) { return _sys_read(fd, buf, count); }
long write(int fd, const char *buf, size_t count) { return _sys_write(fd, buf, count); }
long OS1_time_now(void) { return _sys_get_time(); }
long get_time(void) { return OS1_time_now(); } /* compat shim (DIR-01 F4) */
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
  unsigned long long ns =
      (unsigned long long)req->tv_sec * 1000000000ULL + (unsigned long long)req->tv_nsec;
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
long OS1low_process_spawn(const char *path, int argc, char *const argv[]) { return _sys_spawn(path, argc, argv); }
long OS1low_process_spawn_caps(const char *path, int level, unsigned long caps) { return _sys_spawn_caps(path, level, caps); }
int  OS1low_process_kill(int pid) { return _sys_kill(pid); }
/* OS1low_process_wait (F4 M4.5): wait via a PROCESS capability + OS1_object_wait.
 * A WAIT-only handle is acquirable for any live process (wait-right is separable
 * from kill-right); if the process is already gone, acquisition fails and we fall
 * back to the ambient SYS_WAIT so the legacy "not found" (-2) result is preserved. */
int  OS1low_process_wait(int pid) {
  char idbuf[16];
  sprintf(idbuf, "%d", pid);
  long h = OS1low_handle_create(OS1_NS_PROC, idbuf, OS1_RIGHT_WAIT, OBJ_TYPE_PROCESS);
  if (h < 0)
    return _sys_wait(pid);
  long r = OS1_object_wait((int)h, 0);
  OS1low_handle_close((int)h);
  return (int)r;
}
void OS1low_process_yield(void) { _sys_yield(); }
int  OS1low_process_self(void) { return _sys_get_pid(); }
void OS1low_process_exit(int status) { _sys_exit(status); while (1); }

/* Bare-name compat shims (DIR-01). */
int get_pid(void) { return OS1low_process_self(); }
void exit(int status) { OS1low_process_exit(status); }
int spawn(const char *path) { return (int)OS1low_process_spawn(path, 0, 0); }
int spawn_args(const char *path, int argc, char *const argv[]) {
  return (int)OS1low_process_spawn(path, argc, argv);
}
/* spawn_caps: explicit capability mask; spawn_level: the level's default
 * preset (request CAP_ALL and let the kernel clamp to the level ceiling). */
long spawn_caps(const char *path, int level, unsigned long caps) { return OS1low_process_spawn_caps(path, level, caps); }
long spawn_level(const char *path, int level) { return OS1low_process_spawn_caps(path, level, CAP_ALL); }

/* Object / capability API (ASTRA §6.1/6.2) — thin veneers over the _sys_ stubs.
 * OS1 native base surface; POSIX layers on top of these, not vice versa. */
long OS1low_handle_create(int ns, const char *path, unsigned int rights, int type) { return _sys_handle_create(ns, path, rights, type); }
long OS1low_handle_duplicate(int handle, unsigned int new_rights) { return _sys_handle_dup(handle, new_rights); }
long OS1low_handle_close(int handle) { return _sys_handle_close(handle); }
long OS1low_cap_query(int handle) { return _sys_cap_query(handle); }
long OS1low_cap_grant(int target_pid, int handle, unsigned int rights) { return _sys_cap_grant(target_pid, handle, rights); }
long OS1_object_read(int handle, void *buf, unsigned long n) { return _sys_object_read(handle, buf, n); }
long OS1_object_write(int handle, const void *buf, unsigned long n) { return _sys_object_write(handle, buf, n); }
long OS1_object_wait(int handle, long arg) { return _sys_object_wait(handle, arg); }
long OS1_object_ctl(int handle, int cmd, long arg) { return _sys_object_ctl(handle, cmd, arg); }

/* Window manager surface (ASTRA §6.7: windows as objects).  Enumeration is a
 * direct read syscall; control goes through an OBJ_TYPE_WINDOW capability
 * (acquire → ctl → close), so authority is the unforgeable handle, not ambient
 * identity — an app drives its OWN window freely, a WM drives any window. */
long OS1_window_enum(struct window_info *buf, unsigned long max) { return _sys_window_enum(buf, max); }

/* System statistics snapshot (perf §1).  Forwards to the SYS_SYSSTATS stub,
 * passing sizeof so the kernel can copy the prefix this build understands. */
long OS1_sys_stats(struct os1_sysstats *out) {
  if (!out) return -1;
  return _sys_sysstats(out, sizeof(*out));
}

/* __win_ctl - acquire a WINDOW capability with the rights a verb needs, issue the
 * control verb, then release the handle.  WRITE for minimize/restore/focus,
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
int OS1_window_minimize(int win_id) { return __win_ctl(win_id, OS1_RIGHT_WRITE, OBJ_CTL_MINIMIZE); }
int OS1_window_restore(int win_id)  { return __win_ctl(win_id, OS1_RIGHT_WRITE, OBJ_CTL_RESTORE); }
int OS1_window_focus(int win_id)    { return __win_ctl(win_id, OS1_RIGHT_READ, OBJ_CTL_FOCUS); }
int OS1_window_close(int win_id)    { return __win_ctl(win_id, OS1_RIGHT_DESTROY, OBJ_CTL_CLOSE); }

/* Window / graphics canonical names (ASTRA §6.7, DIR-01 F4): thin veneers over the
 * _sys_ stubs.  The bare verbs (create_window/destroy_window/window_draw/blit/
 * write/of_pid/grid/set_window_flags/set_focus/draw/flush/compositor_render) are
 * compat shims forwarding here. */
int  OS1_window_create(int x, int y, int w, int h, const char *title) { return _sys_create_window(x, y, w, h, title); }
void OS1_window_destroy(int win_id) { _sys_destroy_window(win_id); }
void OS1_window_draw(int win_id, int x, int y, int w, int h, unsigned int color) { _sys_window_draw(win_id, x, y, w, h, color); }
void OS1_window_blit(int win_id, int x, int y, int w, int h, const unsigned int *buf) { _sys_window_blit(win_id, x, y, w, h, buf); }
void OS1_window_write(int win_id, const char *buf, unsigned long count) { _sys_window_write(win_id, buf, count); }
int  OS1_window_of_pid(int pid) { return _sys_window_of_pid(pid); }
int  OS1_window_grid(int win_id, int *cols, int *rows) {
  long r = _sys_window_grid(win_id);
  if (r < 0)
    return (int)r;
  if (cols)
    *cols = (int)((r >> 16) & 0xFFFF);
  if (rows)
    *rows = (int)(r & 0xFFFF);
  return 0;
}
void OS1_window_set_flags(int win_id, int flags) { _sys_window_set_flags(win_id, flags); }
void OS1_window_set_focus(int pid) { extern void _sys_set_focus(int pid); _sys_set_focus(pid); }
int  OS1_window_resize(int win_id, int w, int h) { return _sys_window_resize(win_id, w, h); }
void OS1_gfx_draw(int x, int y, int w, int h, int color) { _sys_draw(x, y, w, h, color); }
/* flush ≡ render: both just pushed the compositor.  Unified onto the single
 * SYS_COMPOSITOR_RENDER syscall (the duplicate SYS_FLUSH was retired). */
void OS1_gfx_flush(void) { _sys_compositor_render(); }
void OS1_gfx_render(void) { _sys_compositor_render(); }

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
void draw(int x, int y, int w, int h, int color) { OS1_gfx_draw(x, y, w, h, color); }
void flush(void) { OS1_gfx_flush(); }
int create_window(int x, int y, int w, int h, const char *title) { return OS1_window_create(x, y, w, h, title); }
void destroy_window(int win_id) { OS1_window_destroy(win_id); }
void window_draw(int win_id, int x, int y, int w, int h, unsigned int color) { OS1_window_draw(win_id, x, y, w, h, color); }
void window_blit(int win_id, int x, int y, int w, int h, const unsigned int *buf) { OS1_window_blit(win_id, x, y, w, h, buf); }
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
long OS1low_ipc_send(int pid, struct ipc_message *msg) { return _sys_send(pid, msg); }
long OS1low_ipc_recv(int pid, struct ipc_message *msg) { return _sys_recv(pid, msg); }
long OS1low_ipc_try_recv(int pid, struct ipc_message *msg) { extern int _sys_try_recv(int pid, void *msg); return _sys_try_recv(pid, msg); }
int send(int pid, struct ipc_message *msg) { return (int)OS1low_ipc_send(pid, msg); }
int recv(int pid, struct ipc_message *msg) { return (int)OS1low_ipc_recv(pid, msg); }
int try_recv(int pid, struct ipc_message *msg) { return (int)OS1low_ipc_try_recv(pid, msg); }
void set_window_flags(int win_id, int flags) { OS1_window_set_flags(win_id, flags); }
void set_focus(int pid) { OS1_window_set_focus(pid); }

/* --- Shared Implementations (from kernel library) ---
 * NOTE(USR-LIB-01): These are direct source-level #includes of kernel
 * internal implementation files, not headers.  Changes to kernel/lib C files
 * silently affect userland behaviour with no compile-time boundary check.
 * vsnprintf.c provides vsnprintf/vsscanf; math.c provides fixed-point trig
 * and DEG_TO_FP_RAD/cos_fp/sin_fp/fixmul used by demo3d; string.c provides
 * memset/memcpy/strlen/strcmp/strncmp/strchr etc. */
#include "../../kernel/lib/vsnprintf.c"
#include "../../kernel/lib/math.c"
#include "../../kernel/lib/string.c"
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
void __stack_chk_fail(void) { printf("Stack smashing detected!\n"); exit(1); }

/* --- Registry Wrappers (OS1_registry_*, ASTRA §6.6) ---
 * op=0 read 'key' into buf; op=1 write 'key' (kernel-side CAP_REG_WRITE +
 * first-writer-wins ownership); op=2 enumerate, optionally under a prefix. */
int OS1_registry_get(const char *key, char *buf, size_t size) { return (int)_sys_registry(0, key, buf, size); }
int OS1_registry_set(const char *key, const char *value) { return (int)_sys_registry(1, key, (char *)value, strlen(value)); }
int OS1_registry_enum(char *buf, size_t size) { return (int)_sys_registry(2, 0, buf, size); }
/* OS1_registry_enum_under (Phase 4.1 A1a): list only keys under 'prefix'. */
int OS1_registry_enum_under(const char *prefix, char *buf, size_t size) { return (int)_sys_registry(2, prefix, buf, size); }
/* OS1_registry_del (Phase 4.1 A-gap1): remove a key. */
int OS1_registry_del(const char *key) { return (int)_sys_registry(3, key, 0, 0); }

/*
 * set_font - transfer a packed font buffer to the kernel (SYS_SET_FONT #253).
 *
 * data: pointer to a [ font_header ][ glyph_info * n ][ bitmap ] buffer.
 * size: total byte count of that buffer.
 *
 * NOTE(USR-FONTMAN-01): The kernel stores 'data' as a raw pointer; the caller
 * must keep the buffer alive indefinitely (fontman uses while(1) yield()).
 */
/* Display / compositor control (ASTRA §6.7): canonical OS1_display_* over the raw
 * _sys_ stubs.  set_font keeps a bare shim; the others had no bare name. */
long OS1_display_info(void) { return _sys_display_info(); }
int  OS1_display_set_mode(int w, int h) { return _sys_set_display_mode(w, h); }
int  OS1_display_poll(void) { return _sys_display_poll(); }
int  OS1_display_set_style(int style_id, int theme_id) { return _sys_set_style(style_id, theme_id); }
int  OS1_display_set_zoom(int percent) { return _sys_set_zoom(percent); }
int  OS1_display_set_font(void *data, size_t size) {
  extern int _sys_set_font(void *data, size_t size);
  return _sys_set_font(data, size);
}
int set_font(void *data, size_t size) { return OS1_display_set_font(data, size); } /* compat shim (DIR-01 F4) */
/* file_read: buf==NULL / size==0 returns the file size without reading data;
 * used by fopen() to probe file size before allocating a read buffer. */
/* OS1_fs_ functions: canonical (ASTRA §6.3); the bare file_write/file_read/
 * list_dir/chdir/getcwd below are compat shims (DIR-01 F4). */
int OS1_fs_write(const char *path, const void *buf, int size, int offset) {
  /* NOTE(M4.5-FS-WRITE): capability-routed write is a follow-up.  handle_create(FS)
   * requires the file to already exist (vfs_open), so routing here would break file
   * CREATION; it needs handle_create O_CREAT support (ASTRA 6.8 open(O_CREAT) ->
   * handle_create).  Until then write stays on the ambient path. */
  return _sys_file_write(path, buf, size, offset);
}
/* OS1_fs_read (F4 M4.5): data reads routed through a FILE capability
 * (handle_create(FS,READ) -> OBJ_CTL_SEEK(offset) -> object_read -> close).  A
 * size<=0 / NULL-buf call is a metadata size-probe (returns the file size) which the
 * object read does not do, so it stays on the ambient SYS_FILE_READ path. */
int OS1_fs_read(const char *path, void *buf, int size, int offset) {
  if (size <= 0 || !buf)
    return _sys_file_read(path, buf, size, offset);
  long h = OS1low_handle_create(OS1_NS_FS, path, OS1_RIGHT_READ, OBJ_TYPE_FILE);
  if (h < 0)
    return (int)h;
  if (offset > 0)
    OS1_object_ctl((int)h, OBJ_CTL_SEEK, offset);
  long r = OS1_object_read((int)h, buf, (unsigned long)size);
  OS1low_handle_close((int)h);
  return (int)r;
}
int OS1_fs_list(const char *path, char *buf, size_t size) { return _sys_list_dir(path, buf, size); }
int OS1_fs_chdir(const char *path) { return _sys_chdir(path); }
int OS1_fs_getcwd(char *buf, size_t size) { return _sys_getcwd(buf, size); }
int OS1_fs_unlink(const char *path) { return _sys_unlink(path); }
int file_write(const char *path, const void *buf, int size, int offset) { return OS1_fs_write(path, buf, size, offset); }
int file_read(const char *path, void *buf, int size, int offset) { return OS1_fs_read(path, buf, size, offset); }
int list_dir(const char *path, char *buf, size_t size) { return OS1_fs_list(path, buf, size); }
int chdir(const char *path) { return OS1_fs_chdir(path); }
int getcwd(char *buf, size_t size) { return OS1_fs_getcwd(buf, size); }

/* POSIX-style fd I/O (ABI-03 fd table).  open() matches the variadic
 * declaration in fcntl.h; the optional mode argument is ignored because the
 * VFS cannot create files yet (the kernel rejects O_CREAT with -EINVAL). */
int open(const char *pathname, int flags, ...) { return _sys_open(pathname, flags); }
int close(int fd) { return _sys_close(fd); }
long lseek(int fd, long offset, int whence) { return _sys_lseek(fd, offset, whence); }

/* --- Formatting & Printing ---
 * All formatting functions delegate to vsnprintf() from kernel/lib/vsnprintf.c
 * (included above).  Output goes to fd 1 (the shell/window TTY) via write().
 *
 * printf: uses a 256-byte stack buffer; output longer than 255 chars is
 * silently truncated by vsnprintf.
 *
 * printf_win: like printf but writes to a specific compositor window by id
 * via window_write() (the dedicated SYS_WINDOW_WRITE, #123) — no longer the
 * fd>=100 overload on write().
 *
 * vsprintf/sprintf: pass 65536 as the size limit — effectively unbounded.
 * Callers are responsible for providing a large enough destination buffer;
 * overflow is not detected.
 *
 * print_hex: renders a 64-bit value as 18-char "0xHHHHHHHHHHHHHHHH" string
 * written directly via write(), bypassing the format engine.
 */
int vsprintf(char *out, const char *fmt, va_list args) { return vsnprintf(out, 65536, fmt, args); }
int printf(const char *fmt, ...) { char buf[256]; va_list args; va_start(args, fmt); int res = vsnprintf(buf, sizeof(buf), fmt, args); va_end(args); write(1, buf, strlen(buf)); return res; }
void window_write(int win_id, const char *buf, unsigned long count) { OS1_window_write(win_id, buf, count); }
int window_of_pid(int pid) { return OS1_window_of_pid(pid); }
int window_grid(int win_id, int *cols, int *rows) { return OS1_window_grid(win_id, cols, rows); }
void printf_win(int win_id, const char *fmt, ...) { char buf[512]; va_list args; va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args); _sys_window_write(win_id, buf, strlen(buf)); }
int sprintf(char *out, const char *fmt, ...) { va_list args; va_start(args, fmt); int res = vsnprintf(out, 65536, fmt, args); va_end(args); return res; }
int snprintf(char *out, size_t size, const char *fmt, ...) { va_list args; va_start(args, fmt); int res = vsnprintf(out, size, fmt, args); va_end(args); return res; }
void print(const char *s) { write(1, s, strlen(s)); }
/* print_hex: manual 16-nibble hex formatter for a 64-bit value. */
void print_hex(unsigned long val) { char buf[18]; buf[0] = '0'; buf[1] = 'x'; for (int i = 0; i < 16; i++) { int digit = (val >> ((15 - i) * 4)) & 0xF; buf[2 + i] = (digit < 10) ? ('0' + digit) : ('A' + digit - 10); } write(1, buf, 18); }

/* --- Standard IO ---
 * getchar: blocking single-char read from fd 0 (keyboard).
 * putchar: writes one character to fd 1 (TTY/window).
 * gets: line-buffered input with backspace handling; size-bounded to avoid
 *   overflow (stops at size-1 chars).  Echoes characters to fd 1 and handles
 *   \b/DEL with the terminal backspace-space-backspace sequence.
 */
int getchar(void) { char c; if (read(0, &c, 1) == 1) return (unsigned char)c; return -1; }
int putchar(int c) { char ch = (char)c; write(1, &ch, 1); return c; }
char *gets(char *s, int size) {
    int i = 0;
    while (i < size - 1) {
        int c = getchar();
        if (c < 0) break;
        if (c == '\b' || c == 127) { if (i > 0) { i--; write(1, "\b \b", 3); } continue; }
        putchar(c);
        if (c == '\n' || c == '\r') { s[i] = '\0'; return s; }
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
  while (*title && i < 30) imsg.payload[i++] = *title++;
  imsg.payload[i++] = ':'; imsg.payload[i++] = ' ';
  while (*msg && i < 63) imsg.payload[i++] = *msg++;
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
int OS1_notify_post(const char *title, const char *msg) { return notify_send(title, msg, 0); }
int OS1_notify_warn(const char *title, const char *msg) { return notify_send(title, msg, 1); } /* yellow */
int notify(const char *title, const char *msg) { return notify_send(title, msg, 0); } /* compat shim (DIR-01 F4) */

/* --- Doom/LibC Compatibility ---
 * FILE emulation: a FILE* is a heap-allocated struct (defined in os1.h) that
 * records the file path, current byte position, error/eof flags, and cached
 * size.  All I/O is synchronous and unbuffered; every fread/fwrite call maps
 * directly to a file_read/file_write syscall with the current offset.
 */

/*
 * fopen - open a file for buffered I/O emulation.
 *
 * Allocates a FILE struct, stores the path, probes the file size via
 * file_read(path, NULL, 0, 0) (size-probe convention: buf==NULL returns size).
 * Returns NULL if the file does not exist and mode is "r" (read-only).
 * Write modes ("w", "a") do not fail on missing files — file_write will
 * create them on demand via the kernel VFS.
 */
FILE *fopen(const char *path, const char *mode) {
  FILE *f = malloc(sizeof(FILE));
  if (!f) return NULL;
  strncpy(f->path, path, sizeof(f->path) - 1);
  f->pos = 0;
  f->error = 0;
  f->eof = 0;
  /* Probe file size; file_read with NULL buf and size=0 returns byte count. */
  f->size = file_read(path, NULL, 0, 0);
  if (f->size < 0 && mode[0] == 'r') {
    free(f);
    return NULL;
  }
  return f;
}

/*
 * fclose - release a FILE handle.
 *
 * NOTE(USR-LIB-02): Guards against NULL with `(size_t)fp > 10`, which is
 * intended to catch sentinel/invalid values stored as small integers in some
 * callers (e.g. fileno tricks).  A proper NULL check (`fp != NULL`) would be
 * more idiomatic; this heuristic silently allows freeing invalid pointers with
 * addresses 1-10, which would corrupt the heap.
 */
int fclose(FILE *fp) {
  if (fp && (size_t)fp > 10) free(fp);
  return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
  if (!fp || (size_t)fp <= 10) return 0;
  int bytes = size * nmemb;
  int read_bytes = file_read(fp->path, ptr, bytes, fp->pos);
  if (read_bytes < 0) {
    fp->error = 1;
    return 0;
  }
  fp->pos += read_bytes;
  if (read_bytes < bytes) fp->eof = 1;
  return read_bytes / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
  if (!fp || (size_t)fp <= 10) return 0;
  int bytes = size * nmemb;
  int written = file_write(fp->path, ptr, bytes, fp->pos);
  if (written < 0) {
    fp->error = 1;
    return 0;
  }
  fp->pos += written;
  return written / size;
}

int fseek(FILE *fp, long offset, int whence) {
  if (!fp || (size_t)fp <= 10) return -1;
  if (whence == SEEK_SET) fp->pos = offset;
  else if (whence == SEEK_CUR) fp->pos += offset;
  else if (whence == SEEK_END) {
    if (fp->size < 0) fp->size = file_read(fp->path, NULL, 0, 0);
    fp->pos = fp->size + offset;
  }
  if (fp->pos < 0) fp->pos = 0;
  fp->eof = 0;
  return 0;
}

long ftell(FILE *fp) {
  if (!fp || (size_t)fp <= 10) return -1;
  return fp->pos;
}

int feof(FILE *fp) { return fp ? fp->eof : 1; }
int ferror(FILE *fp) { return fp ? fp->error : 1; }

char *strdup(const char *s) {
  size_t len = strlen(s) + 1;
  char *res = malloc(len);
  if (res) memcpy(res, s, len);
  return res;
}

int abs(int x) { return x < 0 ? -x : x; }
double fabs(double x) { return x < 0 ? -x : x; }

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
  if (try_recv(-1, &msg) < 0) return 0;

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
  }
  return 0;
}

/* --- Graphics Library ---
 * High-level drawing wrappers that delegate to the compositor syscalls.
 */

/*
 * graphics_draw_rect - fill a rectangle in a compositor window.
 *
 * Thin wrapper over window_draw() -> SYS_WINDOW_DRAW (#211).
 * color is ARGB (0xAARRGGBB).
 */
void graphics_draw_rect(int win_id, int x, int y, int w, int h, uint32_t color) {
  window_draw(win_id, x, y, w, h, color);
}

/*
 * graphics_blit - upload a pixel buffer to a compositor window region.
 *
 * buffer must be w*h uint32_t pixels in ARGB row-major order.
 * Delegates to window_blit() -> SYS_WINDOW_BLIT (#213).
 */
void graphics_blit(int win_id, int x, int y, int w, int h, const uint32_t *buffer) {
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
int graphics_draw_text(int win_id, int x, int y, const char *text, uint32_t color) {
  if (win_id < 0 || !text) return 0;

  struct font_ctx *font = graphics_get_default_font();
  if (font) {
    font_draw_string(win_id, font, x, y, text, color);
    return font_string_width(font, text);
  }

  (void)x; (void)y; (void)color;
  _sys_window_write(win_id, text, strlen(text));
  return (int)strlen(text) * 8;
}

int graphics_text_width(const char *text) {
  if (!text) return 0;

  struct font_ctx *font = graphics_get_default_font();
  if (font) {
    return font_string_width(font, text);
  }

  return (int)strlen(text) * 8;
}

#define OS1_IMAGE_MAX_FILE_BYTES (16u * 1024u * 1024u)
#define OS1_IMAGE_MAX_DIMENSION  4096
#define OS1_IMAGE_MAX_PIXELS     (4096u * 4096u)

/*
 * graphics_load_image - load an encoded image into sanitized ARGB32 pixels.
 *
 * Encoded input is treated as hostile data: it is copied into a bounded scratch
 * buffer, probed before decode, rejected on dimension/pixel/file caps, decoded
 * to a temporary RGBA plane, then copied into an OS1-owned ARGB buffer.  The
 * caller never sees encoded bytes or decoder-owned storage, which keeps image
 * rendering an inert pixel operation suitable for the stdimage base API.
 */
uint32_t *graphics_load_image(const char *path, int *w, int *h) {
  if (!path || !w || !h)
    return NULL;

  *w = 0;
  *h = 0;

  long handle =
      OS1low_handle_create(OS1_NS_FS, path, OS1_RIGHT_READ, OBJ_TYPE_FILE);
  if (handle < 0)
    return NULL;

  long stat_size = OS1_object_ctl((int)handle, OBJ_CTL_STAT, 0);
  if (stat_size <= 0 || (uint64_t)stat_size > OS1_IMAGE_MAX_FILE_BYTES) {
    OS1low_handle_close((int)handle);
    return NULL;
  }

  int size = (int)stat_size;
  unsigned char *data = malloc((size_t)size + 16u);
  if (!data) {
    OS1low_handle_close((int)handle);
    return NULL;
  }

  int total = 0;
  while (total < size) {
    long got = OS1_object_read((int)handle, data + total,
                               (unsigned long)(size - total));
    if (got <= 0) {
      OS1low_handle_close((int)handle);
      free(data);
      return NULL;
    }
    total += (int)got;
  }
  OS1low_handle_close((int)handle);

  if (total != size) {
    free(data);
    return NULL;
  }
  for (int i = 0; i < 16; i++)
    data[size + i] = 0;

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

  for (uint64_t i = 0; i < pixels; i++) {
    uint8_t r = rgba[i * 4u + 0u];
    uint8_t g = rgba[i * 4u + 1u];
    uint8_t b = rgba[i * 4u + 2u];
    uint8_t a = rgba[i * 4u + 3u];
    argb[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
              ((uint32_t)g << 8) | (uint32_t)b;
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
  while (isspace(*p)) p++;
  int neg = 0;
  if (*p == '-') { neg = 1; p++; }
  else if (*p == '+') p++;

  /* Auto-detect base from prefix: "0x" -> 16, "0" -> 8, else 10. */
  if (base == 0) {
    if (*p == '0') {
      if (p[1] == 'x' || p[1] == 'X') base = 16;
      else base = 8;
    } else base = 10;
  }

  if (base == 16 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

  unsigned long val = 0;
  while (1) {
    int digit;
    if (isdigit(*p)) digit = *p - '0';
    else if (isalpha(*p)) digit = tolower(*p) - 'a' + 10;
    else break;

    if (digit >= base) break;
    val = val * base + digit;
    p++;
  }

  if (endptr) *endptr = (char *)p;
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

/*
 * vsscanf - simplified format scanner supporting %d, %x/%X, %s with widths.
 *
 * Supports:
 *   %d  - signed decimal integer -> int *
 *   %x, %X - unsigned hex integer -> unsigned int *; skips optional 0x prefix
 *   %s  - whitespace-delimited string; width limits chars consumed
 *
 * Whitespace in the format string matches zero or more whitespace chars in
 * the input.  Literal format characters must match exactly (returns early on
 * mismatch).  Returns the count of successfully assigned conversions.
 *
 * Missing specifiers: %f, %c, %ld, %u, %p, etc.  Callers using unsupported
 * format specifiers will silently skip the conversion.
 */
int vsscanf(const char *inp, const char *fmt0, va_list ap) {
  int nassigned = 0;
  const unsigned char *fmt = (const unsigned char *)fmt0;
  const char *p_inp = inp;

  while (*fmt) {
    if (isspace(*fmt)) {
      while (isspace(*p_inp)) p_inp++;
      fmt++;
      continue;
    }
    if (*fmt != '%') {
      if (*p_inp != *fmt) return nassigned;
      p_inp++; fmt++;
      continue;
    }
    fmt++; /* skip % */
    /* Parse optional field width */
    int width = 0;
    while (isdigit(*fmt)) {
      width = width * 10 + (*fmt - '0');
      fmt++;
    }

    char c = *fmt++;
    if (c == 'd') {
      while (isspace(*p_inp)) p_inp++;
      int *res = va_arg(ap, int *);
      *res = atoi(p_inp);
      nassigned++;
      while (isdigit(*p_inp) || *p_inp == '-') p_inp++;
    } else if (c == 'x' || c == 'X') {
      while (isspace(*p_inp)) p_inp++;
      unsigned int *res = va_arg(ap, unsigned int *);
      unsigned int val = 0;
      if (p_inp[0] == '0' && (p_inp[1] == 'x' || p_inp[1] == 'X')) p_inp += 2;
      while (isxdigit(*p_inp)) {
        char dc = *p_inp++;
        if (isdigit(dc)) val = (val << 4) | (dc - '0');
        else val = (val << 4) | (tolower(dc) - 'a' + 10);
      }
      *res = val;
      nassigned++;
    } else if (c == 's') {
      while (isspace(*p_inp)) p_inp++;
      char *res = va_arg(ap, char *);
      while (*p_inp && !isspace(*p_inp)) {
        *res++ = *p_inp++;
        if (width > 0 && --width == 0) break;
      }
      *res = '\0';
      nassigned++;
    }
    /* NOTE(USR-LIB-04): Other format specifiers (%f, %c, %ld, %u, etc.) are
     * silently skipped; the corresponding va_arg is NOT consumed, which may
     * desync the va_list for subsequent conversions. */
  }
  return nassigned;
}

/* NOTE(USR-LIB-04): The following four functions are stubs.
 * mkdir/system/getenv return no-op values; atof truncates decimal fractions
 * by delegating to atoi() and casting.  Callers expecting correct behaviour
 * (e.g. a floating-point string "3.14" -> 3.14) silently receive 3.0. */
int mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return 0; }
int system(const char *command) { (void)command; return 0; }
/* atof: NOTE(USR-LIB-04) only integer part is parsed; decimal digits ignored. */
double atof(const char *nptr) { return (double)atoi(nptr); }
char *getenv(const char *name) { (void)name; return NULL; }
int stat(const char *path, struct stat *buf) {
  if (buf) memset(buf, 0, sizeof(struct stat));
  int size = file_read(path, NULL, 0, 0);
  if (size < 0) return -1;
  if (buf) {
    buf->st_size = size;
    buf->st_mode = S_IFREG;
  }
  return 0;
}

/*
 * vfprintf - format and write to a FILE stream.
 *
 * NOTE(USR-LIB-05): The 'stream' argument is ignored; output always goes to
 * fd 1 (stdout/TTY).  Any code that writes to stderr (e.g. fprintf(stderr, ...))
 * will silently produce output on stdout instead of the error channel.
 *
 * Output is limited to 1023 chars by the stack buffer; longer output is
 * silently truncated.  Always returns 0 (not the character count).
 */
int vfprintf(FILE *stream, const char *format, va_list ap) {
  (void)stream;  /* NOTE(USR-LIB-05): stream ignored; always writes to fd 1 */
  char buf[1024];
  vsnprintf(buf, sizeof(buf), format, ap);
  write(1, buf, strlen(buf));
  return 0;
}
/* fflush: no-op (no userland buffer to flush; writes are unbuffered). */
int fflush(FILE *stream) { (void)stream; return 0; }
/* remove/rename: stubs returning success; no VFS deletion/rename syscall yet. */
int remove(const char *pathname) { (void)pathname; return 0; }
int rename(const char *oldpath, const char *newpath) { (void)oldpath; (void)newpath; return 0; }
/* puts: writes string + newline to fd 1, matching the standard POSIX contract. */
int puts(const char *s) { write(1, s, strlen(s)); write(1, "\n", 1); return 0; }

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
 * Used by font_lib.c:font_draw_string() to iterate a UTF-8 string glyph by glyph.
 */
int utf8_decode(const char *s, size_t len, uint32_t *code) {
  if (!s || !code || len == 0) return 0;
  unsigned char c = (unsigned char)s[0];

  if (c < 0x80) {
    *code = c;
    return 1;
  } else if ((c & 0xE0) == 0xC0) {
    if (len < 2 || (s[1] & 0xC0) != 0x80) return 0;
    *code = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
    if (*code < 0x80) return 0;
    return 2;
  } else if ((c & 0xF0) == 0xE0) {
    if (len < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
    *code = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
    if (*code < 0x800) return 0;
    if (*code >= 0xD800 && *code <= 0xDFFF) return 0;
    return 3;
  } else if ((c & 0xF8) == 0xF0) {
    if (len < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
    *code = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
            ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
    if (*code < 0x10000) return 0;
    if (*code > 0x10FFFF) return 0;
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
  switch (errnum) {
  case 0:       return (char *)"Success";
  case ENOENT:  return (char *)"No such file or directory";
  case EBADF:   return (char *)"Bad file descriptor";
  case EACCES:  return (char *)"Permission denied";
  case EEXIST:  return (char *)"File exists";
  case ENOTDIR: return (char *)"Not a directory";
  case EISDIR:  return (char *)"Is a directory";
  case EINVAL:  return (char *)"Invalid argument";
  case ENOSPC:  return (char *)"No space left on device";
  case ENOMEM:  return (char *)"Cannot allocate memory";
  case EFAULT:  return (char *)"Bad address";
  case ENOSYS:  return (char *)"Function not implemented";
  default:      return (char *)"Unknown error";
  }
}

/* --- <stdlib.h> --- */
long atol(const char *nptr) { return strtol(nptr, NULL, 10); }

/* long is 64-bit on both OS1 targets (LP64), so long long shares its range. */
long long strtoll(const char *nptr, char **endptr, int base) {
  return (long long)strtol(nptr, endptr, base);
}

long long atoll(const char *nptr) { return strtoll(nptr, NULL, 10); }

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
 * The std streams are encoded as (FILE*)0/1/2 (stdin/stdout/stderr); fread/
 * fwrite reject those (they need fp->path), so route them straight to the
 * fd-based read()/write().  Real fopen()ed handles (addr > 2) use fread/fwrite. */
static int file_fd(FILE *fp) {
  size_t v = (size_t)fp;
  return v <= 2 ? (int)v : -1;
}

int fputc(int c, FILE *fp) {
  unsigned char ch = (unsigned char)c;
  int fd = file_fd(fp);
  if (fd >= 0) {
    write(fd, (const char *)&ch, 1);
    return (int)ch;
  }
  if (fwrite(&ch, 1, 1, fp) != 1)
    return EOF;
  return (int)ch;
}

int fputs(const char *s, FILE *fp) {
  size_t len = strlen(s);
  int fd = file_fd(fp);
  if (fd >= 0) {
    if (len)
      write(fd, s, len);
    return 0;
  }
  if (len && fwrite(s, 1, len, fp) != len)
    return EOF;
  return 0;
}

int fgetc(FILE *fp) {
  unsigned char ch;
  int fd = file_fd(fp);
  if (fd >= 0) {
    if (read(fd, (char *)&ch, 1) != 1)
      return EOF;
    return (int)ch;
  }
  if (fread(&ch, 1, 1, fp) != 1)
    return EOF;
  return (int)ch;
}

char *fgets(char *s, int size, FILE *fp) {
  if (size <= 0)
    return NULL;
  int i = 0;
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

/* --- <signal.h> --- OS1 has no signal delivery; installing a handler is a
 * no-op that reports the previous (always default) disposition. */
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
