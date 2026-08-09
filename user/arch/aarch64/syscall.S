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
.global _sys_spawn_redir
.global _sys_spawn_caps
.global _sys_kill
.global _sys_wait
.global _sys_yield
.global _sys_nanosleep
.global _sys_clock_gettime
.global _sys_send
.global _sys_recv
.global _sys_draw
.global _sys_create_window
.global _sys_window_draw
.global _sys_window_write
.global _sys_window_of_pid
.global _sys_window_blit
.global _sys_compositor_render
.global _sys_set_font
.global _sys_stat

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

/* int _sys_spawn(const char *path, int argc, char *const argv[], uint flags)
 * The kernel now also reads arg4/arg5 (fd-redirection list + count, Phase 4);
 * a plain spawn has none, so ZERO x4/x5 here — otherwise leftover register
 * values would be misread as a redirection array. */
_sys_spawn:
    mov x4, #0
    mov x5, #0
    mov x8, #SYS_SPAWN
    svc #0
    ret

/* long _sys_spawn_redir(const char *path, int argc, char *const argv[],
 *                       uint flags, const struct spawn_redir *redir, int n)
 * Same SYS_SPAWN, but x4/x5 carry the redirection list — pass them through. */
_sys_spawn_redir:
    mov x8, #SYS_SPAWN
    svc #0
    ret

/* long _sys_spawn_caps(const char *path, int level, unsigned long caps) */
_sys_spawn_caps:
    mov x8, #SYS_SPAWN_CAPS
    svc #0
    ret

/* int _sys_kill(int pid) */
_sys_kill:
    mov x8, #SYS_KILL
    svc #0
    ret

/* int _sys_wait(int pid)
 * The kernel now also reads arg1 (an OPTIONAL user int* for the exit status,
 * Phase 9b); this form has none, so ZERO x1 here.  x1 is a scratch register
 * with undefined contents at a one-argument call, so leftover values would be
 * misread as a status pointer and the kernel would write four bytes into a
 * semi-random address in the caller's own address space.  Exactly the hazard
 * _sys_spawn zeroes x4/x5 for. */
_sys_wait:
    mov x1, #0
    mov x8, #SYS_WAIT
    svc #0
    ret

/* int _sys_wait_status(int pid, int *code) — same SYS_WAIT, arg1 = status out */
.global _sys_wait_status
_sys_wait_status:
    mov x8, #SYS_WAIT
    svc #0
    ret

/* void _sys_yield(void) */
_sys_yield:
    mov x8, #SYS_YIELD
    svc #0
    ret

/* void _sys_nanosleep(uint64_t ns)  — ns in x0 */
_sys_nanosleep:
    mov x8, #SYS_NANOSLEEP
    svc #0
    ret

/* long _sys_clock_gettime(int clk)  — clk in x0, returns ns in x0 */
_sys_clock_gettime:
    mov x8, #SYS_CLOCK_GETTIME
    svc #0
    ret

/* void _sys_draw(int x, int y, int w, int h, int color) */
/* Args: x0, x1, x2, x3, x4 */
_sys_draw:
    mov x8, #SYS_DRAW
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

/* long _sys_window_write(int win_id, const char *buf, unsigned long count) */
_sys_window_write:
    mov x8, #SYS_WINDOW_WRITE
    svc #0
    ret

/* int _sys_window_of_pid(int pid) */
_sys_window_of_pid:
    mov x8, #SYS_WINDOW_OF_PID
    svc #0
    ret

/* long _sys_window_grid(int win_id) */
.global _sys_window_grid
_sys_window_grid:
    mov x8, #SYS_WINDOW_GRID
    svc #0
    ret

/* long _sys_display_info(void) — packed (w<<16)|h (GFX-DYN-01) */
.global _sys_display_info
_sys_display_info:
    mov x8, #SYS_DISPLAY_INFO
    svc #0
    ret

/* int _sys_set_display_mode(int w, int h) */
.global _sys_set_display_mode
_sys_set_display_mode:
    mov x8, #SYS_SET_DISPLAY_MODE
    svc #0
    ret

/* int _sys_window_resize(int win_id, int w, int h) */
.global _sys_window_resize
_sys_window_resize:
    mov x8, #SYS_WINDOW_RESIZE
    svc #0
    ret

/* int _sys_display_poll(void) — returns 1 if a host display-change was applied */
.global _sys_display_poll
_sys_display_poll:
    mov x8, #SYS_DISPLAY_POLL
    svc #0
    ret



/* int _sys_set_style(int style_id, int theme_id, int bg_id) — -1 = keep */
.global _sys_set_style
_sys_set_style:
    mov x8, #SYS_SET_STYLE
    svc #0
    ret

/* int _sys_set_zoom(int percent) — desktop HiDPI/zoom */
.global _sys_set_zoom
_sys_set_zoom:
    mov x8, #SYS_SET_ZOOM
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

/* long _sys_get_identity(void) -> (level<<16)|caps */
.global _sys_get_identity
_sys_get_identity:
    mov x8, #SYS_GET_IDENTITY
    svc #0
    ret

/* long _sys_sysstats(void *buf, size_t buf_size) -> bytes written, or <0 */
.global _sys_sysstats
_sys_sysstats:
    mov x8, #SYS_SYSSTATS
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

.global _sys_sbrk
_sys_sbrk:
    mov x8, #SYS_SBRK
    svc #0
    ret

/* int _sys_set_font(void *data, size_t size) */
_sys_set_font:
    mov x8, #SYS_SET_FONT
    svc #0
    ret

.global _sys_chdir
_sys_chdir:
    mov x8, #SYS_CHDIR
    svc #0
    ret

.global _sys_unlink
_sys_unlink:
    mov x8, #SYS_UNLINK
    svc #0
    ret

.global _sys_mkdir
_sys_mkdir:
    mov x8, #SYS_MKDIR
    svc #0
    ret

/* int _sys_pipe(int fds[2]) — fds in x0 */
.global _sys_pipe
_sys_pipe:
    mov x8, #SYS_PIPE
    svc #0
    ret

.global _sys_port_send_caps
_sys_port_send_caps:
    mov x8, #SYS_PORT_SEND_CAPS
    svc #0
    ret

.global _sys_getcwd
_sys_getcwd:
    mov x8, #SYS_GETCWD
    svc #0
    ret

/* --- Object / capability ABI (ASTRA §6.1/6.2/6.5) --- */
.global _sys_handle_create
_sys_handle_create:
    mov x8, #SYS_HANDLE_CREATE
    svc #0
    ret

.global _sys_handle_dup
_sys_handle_dup:
    mov x8, #SYS_HANDLE_DUP
    svc #0
    ret

.global _sys_handle_close
_sys_handle_close:
    mov x8, #SYS_HANDLE_CLOSE
    svc #0
    ret

.global _sys_cap_query
_sys_cap_query:
    mov x8, #SYS_CAP_QUERY
    svc #0
    ret

.global _sys_cap_grant
_sys_cap_grant:
    mov x8, #SYS_CAP_GRANT
    svc #0
    ret

.global _sys_object_read
_sys_object_read:
    mov x8, #SYS_OBJECT_READ
    svc #0
    ret

.global _sys_object_write
_sys_object_write:
    mov x8, #SYS_OBJECT_WRITE
    svc #0
    ret

.global _sys_object_wait
_sys_object_wait:
    mov x8, #SYS_OBJECT_WAIT
    svc #0
    ret

.global _sys_object_ctl
_sys_object_ctl:
    mov x8, #SYS_OBJECT_CTL
    svc #0
    ret

/* long _sys_window_enum(struct window_info *buf, unsigned long max) */
.global _sys_window_enum
_sys_window_enum:
    mov x8, #SYS_WINDOW_ENUM
    svc #0
    ret

.global setjmp
setjmp:
    /* x0 contains the address of jmp_buf */
    /* Save callee-saved registers x19-x29 */
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    str x29,      [x0, #80]
    /* Save SP (x31) and LR (x30) */
    mov x9, sp
    str x9,      [x0, #88]
    str x30,     [x0, #96]
    /* Return 0 */
    mov x0, #0
    ret

.global longjmp
longjmp:
    /* x0 contains the address of jmp_buf, x1 contains the val */
    /* Restore callee-saved registers x19-x29 */
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldr x29,      [x0, #80]
    /* Restore SP and LR */
    ldr x9,      [x0, #88]
    mov sp, x9
    ldr x30,     [x0, #96]
    /* If val is 0, return 1; else return val */
    mov w0, #1
    cmp w1, #0
    csel w0, w1, w0, ne
    ret

/* int _sys_stat(const char *path, struct abi_stat *out) */
_sys_stat:
    mov x8, #SYS_STAT
    svc #0
    ret
