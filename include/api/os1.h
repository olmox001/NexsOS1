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

/* --- System Constants --- */
#define PROCESS_NAME_MAX 32
#define STACK_SIZE       131072
#define MAX_PROCESSES    64

/* --- Syscall Numbers --- */
#define SYS_READ               63
#define SYS_WRITE              64
#define SYS_EXIT               93
#define SYS_GET_TIME           169
#define SYS_GETPID             172
#define SYS_DRAW               200
#define SYS_FLUSH              201
#define SYS_CREATE_WINDOW      210
#define SYS_WINDOW_DRAW        211
#define SYS_COMPOSITOR_RENDER  212
#define SYS_WINDOW_BLIT        213
#define SYS_WINDOW_SET_FLAGS   214
#define SYS_DESTROY_WINDOW     215
#define SYS_SBRK               216
#define SYS_SPAWN              220
#define SYS_KILL               221
#define SYS_GETPROCS           222
#define SYS_YIELD              223
#define SYS_SEND               230
#define SYS_RECV               231
#define SYS_WAIT               247
#define SYS_REGISTRY           250

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
extern void _sys_write(int fd, const char *buf, size_t count);
extern long _sys_get_time(void);
extern int  _sys_get_pid(void);
extern void _sys_exit(int status);
extern int  _sys_spawn(const char *path);
extern int  _sys_kill(int pid);
extern int  _sys_wait(int pid);
extern void _sys_yield(void);
extern void _sys_draw(int x, int y, int w, int h, int color);
extern void _sys_flush(void);
extern int  _sys_create_window(int x, int y, int w, int h, const char *title);
extern void _sys_destroy_window(int win_id);
extern void _sys_window_draw(int win_id, int x, int y, int w, int h, unsigned int color);
extern void _sys_window_blit(int win_id, int x, int y, int w, int h, const unsigned int *buf);
extern void _sys_compositor_render(void);
extern void _sys_window_set_flags(int win_id, int flags);
extern void* _sys_sbrk(intptr_t increment);
extern long _sys_registry(int op, const char *key, char *value, size_t size);
extern long _sys_get_procs(void *procs, size_t max_count);
extern int  _sys_file_write(const char *path, const void *buf, int size, int offset);
extern int  _sys_file_read(const char *path, void *buf, int size, int offset);
extern int  _sys_send(int pid, struct ipc_message *msg);
extern int  _sys_recv(int pid, struct ipc_message *msg);

/* Standard C-like Library Functions */
long read(int fd, char *buf, unsigned long count);
void write(int fd, const char *buf, size_t count);
long get_time(void);
int  get_pid(void);
void exit(int status);
int  spawn(const char *path);
int  kill_process(int pid);
int  wait(int pid);
void yield(void);
void sleep(int ticks);

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

/* Filesystem Helpers */
int file_write(const char *path, const void *buf, int size, int offset);
int file_read(const char *path, void *buf, int size, int offset);

/* Formatting & Printing */
void print(const char *s);
void print_hex(unsigned long val);
int  printf(const char *fmt, ...);
void printf_win(int win_id, const char *fmt, ...);
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
