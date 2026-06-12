/*
 * user/syscall.S
 * AArch64 System Call Wrappers
 *
 * Implements the ABI:
 * x8: Syscall Number
 * x0-x5: Arguments
 * svc #0: Trap to Kernel
 */
/* Syscall numbers come from the shared ABI header (ABI-01/ABI-SYS-01):
 * the same macros the kernel dispatch switch compiles against. */
#include <syscall_nums.h>

.text
.global _start
_start:
    /* SP is already set by kernel */
    bl main
    /* Exit with main's return value */
    bl exit
    /* Should never reach here */
1:  b 1b

.global _sys_open
.global _sys_close
.global _sys_lseek
.global _sys_read
.global _sys_write
.global _sys_get_pid
.global _sys_exit
.global _sys_get_time
.global _sys_spawn
.global _sys_kill
.global _sys_wait
.global _sys_yield
.global _sys_send
.global _sys_recv
.global _sys_draw
.global _sys_flush
.global _sys_create_window
.global _sys_window_draw
.global _sys_window_blit
.global _sys_compositor_render
.global _sys_file_write
.global _sys_file_read
.global _sys_sbrk

/* 
 * Generic Syscall Wrapper Macros could be used, but manual is clearer for now
 */

/* int _sys_open(const char *path, int flags) */
_sys_open:
    mov x8, #SYS_OPEN
    svc #0
    ret

/* int _sys_close(int fd) */
_sys_close:
    mov x8, #SYS_CLOSE
    svc #0
    ret

/* long _sys_lseek(int fd, long off, int whence) */
_sys_lseek:
    mov x8, #SYS_LSEEK
    svc #0
    ret

/* long _sys_read(int fd, char *buf, unsigned long count) */
/* x0=fd, x1=buf, x2=count */
_sys_read:
    mov x8, #SYS_READ
    svc #0
    ret

/* void _sys_write(int fd, const char *buf, size_t count) */
_sys_write:
    mov x8, #SYS_WRITE
    svc #0
    ret

/* long _sys_get_time(void) */
_sys_get_time:
    mov x8, #SYS_GET_TIME
    svc #0
    ret

/* int _sys_get_pid(void) */
_sys_get_pid:
    mov x8, #SYS_GETPID
    svc #0
    ret

/* void _sys_exit(int status) */
_sys_exit:
    mov x8, #SYS_EXIT
    svc #0
    ret

/* int _sys_spawn(const char *path) */
_sys_spawn:
    mov x8, #SYS_SPAWN
    svc #0
    ret

/* int _sys_kill(int pid) */
_sys_kill:
    mov x8, #SYS_KILL
    svc #0
    ret

/* int _sys_wait(int pid) */
_sys_wait:
    mov x8, #SYS_WAIT
    svc #0
    ret

/* void _sys_yield(void) */
_sys_yield:
    mov x8, #SYS_YIELD
    svc #0
    ret

/* void _sys_draw(int x, int y, int w, int h, int color) */
/* Args: x0, x1, x2, x3, x4 */
_sys_draw:
    mov x8, #SYS_DRAW
    svc #0
    ret

/* void _sys_flush(void) */
_sys_flush:
    mov x8, #SYS_FLUSH
    svc #0
    ret

/* int _sys_create_window(int x, int y, int w, int h, const char *title) */
/* Args: x0, x1, x2, x3, x4 */
_sys_create_window:
    mov x8, #SYS_CREATE_WINDOW
    svc #0
    ret

/* void _sys_window_draw(int win_id, int x, int y, int w, int h, uint color) */
/* Args: x0..x5 */
_sys_window_draw:
    mov x8, #SYS_WINDOW_DRAW
    svc #0
    ret

/* void _sys_window_blit(int win_id, int x, int y, int w, int h, const uint *buf) */
_sys_window_blit:
    mov x8, #SYS_WINDOW_BLIT
    svc #0
    ret

/* void _sys_compositor_render(void) */
_sys_compositor_render:
    mov x8, #SYS_COMPOSITOR_RENDER
    svc #0
    ret

/* int _sys_send(int pid, struct ipc_message *msg) */
_sys_send:
    mov x8, #SYS_SEND
    svc #0
    ret

.global _sys_window_set_flags
_sys_window_set_flags:
    mov x8, #SYS_WINDOW_SET_FLAGS
    svc #0
    ret

.global _sys_destroy_window
_sys_destroy_window:
    mov x8, #SYS_DESTROY_WINDOW
    svc #0
    ret

/* int _sys_recv(int pid, struct ipc_message *msg) */
_sys_recv:
    mov x8, #SYS_RECV
    svc #0
    ret

/* long _sys_registry(int op, const char *key, char *value, size_t size) */
.global _sys_registry
_sys_registry:
    mov x8, #SYS_REGISTRY
    svc #0
    ret

.global _sys_get_procs
_sys_get_procs:
    mov x8, #SYS_GETPROCS
    svc #0
    ret


/* int _sys_file_write(const char *path, const void *buf, int size, int offset) */
_sys_file_write:
    mov x8, #SYS_FILE_WRITE
    svc #0
    ret

/* int _sys_file_read(const char *path, void *buf, int size, int offset)  */
_sys_file_read:
    mov x8, #SYS_FILE_READ
    svc #0
    ret

/* void* _sys_sbrk(intptr_t increment) */
_sys_sbrk:
    mov x8, #SYS_SBRK
    svc #0
    ret

/* void _sys_set_focus(int pid) */
.global _sys_set_focus
_sys_set_focus:
    mov x8, #SYS_SET_FOCUS
    svc #0
    ret

.global _sys_try_recv
_sys_try_recv:
    mov x8, #SYS_TRY_RECV
    svc #0
    ret
