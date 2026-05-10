/*
 * user/lib.h
 * User-space library header
 */
#ifndef _LIB_H
#define _LIB_H

#include <stddef.h>
#include <stdint.h>

/* Syscall constants (Linux AArch64 compatible) */
#define PROCESS_NAME_MAX 32
#define STACK_SIZE 65536 /* 64KB stack for user processes */

/* Process Info for Diagnostics */
struct ps_info {
  int pid;
  char name[32];
  int state;
  int priority;
  uint64_t cpu_time;
  int on_cpu;
};

#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_EXIT 93
#define SYS_GET_TIME 169
#define SYS_GETPID 172
#define SYS_DRAW 200
#define SYS_FLUSH 201
#define SYS_CREATE_WINDOW 210
#define SYS_WINDOW_DRAW 211
#define SYS_COMPOSITOR_RENDER 212
#define SYS_WINDOW_BLIT 213
#define SYS_WINDOW_SET_FLAGS 214
#define SYS_DESTROY_WINDOW 215
#define SYS_SPAWN 220
#define SYS_KILL 221
#define SYS_GETPROCS 222
#define SYS_YIELD 223
#define SYS_SEND 230
#define SYS_RECV 231
#define SYS_WAIT 247
#define SYS_REGISTRY 250

/* External Syscall Wrappers (defined in user/syscall.S) */
extern long _sys_read(int fd, char *buf, unsigned long count);
extern void _sys_write(int fd, const char *buf, size_t count);
extern long _sys_get_time(void);
extern int _sys_get_pid(void);
extern void _sys_exit(int status);
extern int _sys_spawn(const char *path);
extern int _sys_kill(int pid);
extern int _sys_wait(int pid);
extern void _sys_yield(void);
extern void _sys_draw(int x, int y, int w, int h, int color);
extern void _sys_flush(void);
extern int _sys_create_window(int x, int y, int w, int h, const char *title);
extern void _sys_destroy_window(int win_id);
extern void _sys_window_draw(int win_id, int x, int y, int w, int h,
                             unsigned int color);
extern void _sys_window_blit(int win_id, int x, int y, int w, int h,
                             const unsigned int *buf);
extern void _sys_compositor_render(void);
extern void _sys_window_set_flags(int win_id, int flags);
extern long _sys_registry(int op, const char *key, char *value, size_t size);
extern long _sys_registry(int op, const char *key, char *value, size_t size);
extern long _sys_get_procs(void *procs, size_t max_count);
extern int _sys_file_write(const char *path, const void *buf, int size,
                           int offset);
extern int _sys_file_read(const char *path, void *buf, int size, int offset);

/* IPC constants */
#define IPC_TYPE_RAW 0
#define IPC_TYPE_NOTIFY 0x100

struct ipc_message {
  int from;
  int type;
  uint64_t data1;
  uint64_t data2;
  char payload[64];
};

extern int _sys_send(int pid, struct ipc_message *msg);
extern int _sys_recv(int pid, struct ipc_message *msg);

/* Basic syscalls */
long read(int fd, char *buf, unsigned long count);
void write(int fd, const char *buf, size_t count);
long get_time(void);
int get_pid(void);
void exit(int status);
int spawn(const char *path);
int kill_process(int pid);
int wait(int pid);
void yield(void);
void sleep(int ticks);

/* IPC */
int send(int pid, struct ipc_message *msg);
int recv(int pid, struct ipc_message *msg);
int try_recv(int pid, struct ipc_message *msg);
int notify(const char *title, const char *msg);
void set_window_flags(int win_id, int flags);
void set_focus(int pid);

/* Graphics syscalls (direct framebuffer) */
void draw(int x, int y, int w, int h, int color);
void flush(void);

/* Compositor syscalls */
int create_window(int x, int y, int w, int h, const char *title);
void destroy_window(int win_id);
void window_draw(int win_id, int x, int y, int w, int h, unsigned int color);
void window_blit(int win_id, int x, int y, int w, int h,
                 const unsigned int *buf);
void compositor_render(void);

/* Registry API */
int registry_read(const char *key, char *buf, size_t size);
int registry_write(const char *key, const char *value);

/* Helper functions */
void print(const char *s);
void print_hex(unsigned long val);
void printf(const char *fmt, ...);
void printf_win(int win_id, const char *fmt, ...);
void sprintf(char *out, const char *fmt, ...);
void snprintf(char *out, size_t size, const char *fmt, ...);
void vsnprintf(char *out, size_t size, const char *fmt, __builtin_va_list args);
void vsprintf(char *out, const char *fmt, __builtin_va_list args);
void *memset(void *s, int c, size_t n);
size_t strlen(const char *s);

/* Async print buffer */
void flush_print(void);

/* Fixed-point math (16.16) */
#define FP_SHIFT 16
#define FP_ONE (1 << FP_SHIFT)
#define FP_PI 205887
#define DEG_TO_FP_RAD(d) (((d) * 1144)) /* approx d * PI / 180 in 16.16 */

int sin_fp(int x);
int cos_fp(int x);
int fixmul(int a, int b);

/* Standard IO */
int getchar(void);
int putchar(int c);
int getchar(void);
int putchar(int c);
char *gets(char *s, int size);

int file_write(const char *path, const void *buf, int size, int offset);
int file_read(const char *path, void *buf, int size, int offset);

#endif /* _LIB_H */
