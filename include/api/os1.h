/*
 * include/api/os1.h
 * Public OS1 API and Syscall Definitions
 */
#ifndef _OS1_API_H
#define _OS1_API_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include "posix_types.h"
/* Syscall numbers: single source of truth shared with the kernel dispatch
 * switch and the .S stubs (ABI-01/ABI-SYS-01).
 * Error model (ABI-02): syscalls return negative errno values from
 * posix_types.h on failure (-EFAULT, -ENOMEM, ...), >= 0 on success. */
#include "syscall_nums.h"
/* Privilege levels (PLVL_*) and capabilities (CAP_*) for spawn_caps (#79). */
#include "caps.h"
/* Object/handle/capability ABI (OBJ_TYPE_*, OS1_RIGHT_*, OS1_NS_*) — ASTRA
 * §6.1/6.2/6.5. The OS1low_/OS1_object_ surface below operates on these. */
#include "object.h"

/* --- System Constants --- */
#define PROCESS_NAME_MAX 32
#define STACK_SIZE       131072
#define MAX_PROCESSES    64

/* --- Data Structures --- */

/* Process information for diagnostics */
struct ps_info {
    int pid;
    char name[PROCESS_NAME_MAX];
    int state;
    int priority;
    uint64_t cpu_time;
    int on_cpu;
};

/* --- Public API Functions --- */

/* Syscall Wrappers (Low-level) */
extern long _sys_read(int fd, char *buf, unsigned long count);
extern long _sys_write(int fd, const char *buf, size_t count);
extern long _sys_get_time(void);
extern int  _sys_get_pid(void);
extern void _sys_exit(int status);
extern int  _sys_spawn(const char *path, int argc, char *const argv[]);
extern long _sys_spawn_caps(const char *path, int level, unsigned long caps);
extern int  _sys_kill(int pid);
extern int  _sys_wait(int pid);
extern void _sys_yield(void);
extern void _sys_nanosleep(unsigned long long ns);
extern long _sys_clock_gettime(int clk);
extern void _sys_draw(int x, int y, int w, int h, int color);
extern void _sys_flush(void);
extern int  _sys_create_window(int x, int y, int w, int h, const char *title);
extern void _sys_destroy_window(int win_id);
extern void _sys_window_draw(int win_id, int x, int y, int w, int h, unsigned int color);
extern long _sys_window_write(int win_id, const char *buf, unsigned long count);
extern int  _sys_window_of_pid(int pid);
extern long _sys_window_grid(int win_id);
extern void _sys_window_blit(int win_id, int x, int y, int w, int h, const unsigned int *buf);
extern void _sys_compositor_render(void);
extern void _sys_window_set_flags(int win_id, int flags);
/* Display / framebuffer (GFX-DYN-01) */
extern long _sys_display_info(void);              /* current desktop (w<<16)|h */
extern int  _sys_set_display_mode(int w, int h);  /* set resolution; desktop adapts */
extern int  _sys_window_resize(int win_id, int w, int h); /* resize own window surface */
extern int  _sys_display_poll(void);              /* apply pending host resize; 1 if changed */
extern int  _sys_set_style(int style_id, int theme_id); /* compositor look; -1 = keep */
extern int  _sys_set_zoom(int percent);           /* desktop HiDPI/zoom percent */
extern void* _sys_sbrk(intptr_t increment);
extern long _sys_registry(int op, const char *key, char *value, size_t size);
extern long _sys_get_procs(void *procs, size_t max_count);
extern int  _sys_file_write(const char *path, const void *buf, int size, int offset);
extern int  _sys_file_read(const char *path, void *buf, int size, int offset);
extern int  _sys_send(int pid, struct ipc_message *msg);
extern int  _sys_recv(int pid, struct ipc_message *msg);
extern int  _sys_list_dir(const char *path, char *buf, size_t size);
extern int  _sys_chdir(const char *path);
extern int  _sys_getcwd(char *buf, size_t size);
extern int  _sys_open(const char *path, int flags);
extern int  _sys_close(int fd);
extern long _sys_lseek(int fd, long offset, int whence);
/* Object / capability ABI low-level stubs (ASTRA §6.1/6.2/6.5). */
extern long _sys_handle_create(int ns, const char *path, unsigned int rights, int type);
extern long _sys_handle_dup(int handle, unsigned int new_rights);
extern long _sys_handle_close(int handle);
extern long _sys_cap_query(int handle);
extern long _sys_cap_grant(int target_pid, int handle, unsigned int rights);
extern long _sys_object_read(int handle, void *buf, unsigned long n);
extern long _sys_object_write(int handle, const void *buf, unsigned long n);
extern long _sys_object_wait(int handle, long arg);

/* Standard C-like Library Functions */
long read(int fd, char *buf, unsigned long count);
/* write: returns the number of bytes written (POSIX ssize_t-style), matching
 * read().  Was void; widened to long so ported POSIX code can check the count
 * (libc-layer change only — the SYS_WRITE syscall already returns it). */
long write(int fd, const char *buf, size_t count);
long get_time(void);
int  get_pid(void);
void exit(int status);
int  spawn(const char *path);
/* spawn_args: like spawn(), but hands the child an argv vector (the shell
 * uses it to pass a filename, e.g. `kilo notes.txt`).  argv[0] is the program
 * name as invoked; argv[argc] need not be NULL (the kernel terminates the
 * copy at argc).  The kernel marshals the strings onto the child's stack and
 * sets argc/argv as main()'s first two arguments per the C ABI. */
int  spawn_args(const char *path, int argc, char *const argv[]);
/* Sandboxed spawn (USR-SEC-03 #79).  level = PLVL_*; caps = OR of CAP_*.
 * The kernel clamps both: a child is never more privileged than its parent,
 * never above the level's ceiling, never more than the parent holds.
 * spawn_level() uses the level's default capability preset. */
long spawn_caps(const char *path, int level, unsigned long caps);
long spawn_level(const char *path, int level);
int  kill_process(int pid);
int  wait(int pid);
void yield(void);
int utf8_decode(const char *s, uint32_t *code);
/* OS1_sleep: proprietary BASE-API blocking sleep, in MILLISECONDS (NOT POSIX
 * seconds). Prefixed OS1_ to keep the NEXS base API distinct from the POSIX/libc
 * surface built on top of it (usleep/nanosleep in <unistd.h>/<time.h>); the bare
 * name `sleep` is reserved for a future real POSIX sleep(unsigned seconds). */
void OS1_sleep(int ms);

/* Tier 3 time primitives (docs/TIMER-MODEL.md §4). os1.h is the proprietary
 * base API; POSIX clock_gettime() (<time.h>) is built on top of these.
 *   os1_mono_ns(): monotonic nanoseconds since boot (real-time, both arches).
 *   os1_cpu_ns():  this process's consumed CPU time in real nanoseconds. */
unsigned long long os1_mono_ns(void);
unsigned long long os1_cpu_ns(void);

void *sbrk(intptr_t increment);
void *malloc(size_t size);
void  free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);

/* IPC API */
int send(int pid, struct ipc_message *msg);
int recv(int pid, struct ipc_message *msg);
int try_recv(int pid, struct ipc_message *msg);
int notify(const char *title, const char *msg);

/* Window Management & Graphics */
int  create_window(int x, int y, int w, int h, const char *title);
void destroy_window(int win_id);
void window_draw(int win_id, int x, int y, int w, int h, unsigned int color);
void window_blit(int win_id, int x, int y, int w, int h, const unsigned int *buf);
void compositor_render(void);
void set_window_flags(int win_id, int flags);
void set_focus(int pid);
void draw(int x, int y, int w, int h, int color);
void flush(void);

/* Registry API */
int registry_read(const char *key, char *buf, size_t size);
int registry_write(const char *key, const char *value);
int set_font(void *data, size_t size);

/* Filesystem Helpers */
int file_write(const char *path, const void *buf, int size, int offset);
int file_read(const char *path, void *buf, int size, int offset);
int list_dir(const char *path, char *buf, size_t size);
int chdir(const char *path);
int getcwd(char *buf, size_t size);

/* POSIX-style fd I/O (ABI-03 fd table; open() is declared in fcntl.h, the
 * O_ and SEEK_ values in posix_types.h).  read()/write() above work on any
 * fd: 0=stdin, 1/2=own window, open()ed files >= 3. */
int  close(int fd);
long lseek(int fd, long offset, int whence);

/* Object / capability API (ASTRA §6.1/6.2) — the OS1 NATIVE base surface.
 * Authority is an unforgeable handle to a kernel object, not a PID/ambient
 * mask.  OS1low_ = stable low-level primitives; OS1_object_* = uniform object
 * I/O.  POSIX (open/read/write) is a personality layered ON TOP of these, not
 * the reverse.  ns/rights/type constants live in <object.h>. */
long OS1low_handle_create(int ns, const char *path, unsigned int rights, int type);
long OS1low_handle_duplicate(int handle, unsigned int new_rights);
long OS1low_handle_close(int handle);
long OS1low_cap_query(int handle);
long OS1low_cap_grant(int target_pid, int handle, unsigned int rights);
long OS1_object_read(int handle, void *buf, unsigned long n);
long OS1_object_write(int handle, const void *buf, unsigned long n);
long OS1_object_wait(int handle, long arg);

/* Formatting & Printing */
void print(const char *s);
void print_hex(unsigned long val);
int  printf(const char *fmt, ...);
void printf_win(int win_id, const char *fmt, ...);
/* window_write: write text straight to a window you own, by id (#123).
 * Replaces the old fd>=100 overload on write(). */
void window_write(int win_id, const char *buf, unsigned long count);
/* window_of_pid: compositor window id of a pid, 0 if it has none (#123).
 * The shell uses it to run windowless programs in-shell (foreground). */
int  window_of_pid(int pid);
/* window_grid: terminal character grid of a window you own, as (cols<<16)|rows
 * (the compositor terminal cell size depends on the active font, so a windowed
 * TTY app must query this instead of assuming a fixed 80x25).  < 0 on error. */
int  window_grid(int win_id, int *cols, int *rows);
int  sprintf(char *out, const char *fmt, ...);
int  snprintf(char *out, size_t size, const char *fmt, ...);
int  vsnprintf(char *out, size_t size, const char *fmt, va_list args);
int  vsprintf(char *out, const char *fmt, va_list args);

/* Fixed-point Math */
int32_t fixmul(int32_t a, int32_t b);
int32_t sin_fp(int32_t x);
int32_t cos_fp(int32_t x);
int32_t lerp_fp(int32_t a, int32_t b, int32_t t);
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
size_t strlen(const char *s);
char *strncpy(char *dest, const char *src, size_t n);
int strncmp(const char *s1, const char *s2, size_t n);
int strcasecmp(const char *s1, const char *s2);
int atoi(const char *s);

/* Standard I/O */
int   getchar(void);
int   putchar(int c);
char *gets(char *s, int size);

/* Fixed-Point Math (16.16) */
#define FP_SHIFT 16
#define FP_ONE   (1 << FP_SHIFT)
#define FP_PI    205887
#define DEG_TO_FP_RAD(d) (((d) * 1144))

int sin_fp(int x);
int cos_fp(int x);
int fixmul(int a, int b);

void __stack_chk_fail(void);

#endif /* _OS1_API_H */
