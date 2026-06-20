/*
 * user/arch/amd64/syscall.S
 * x86-64 System Call Wrappers
 *
 * ABI:
 *   RAX: syscall number
 *   RDI, RSI, RDX, R10, R8, R9: arguments (R10 replaces RCX)
 *   syscall instruction traps to kernel
 */
/* Syscall numbers come from the shared ABI header (ABI-01/ABI-SYS-01):
 * the same macros the kernel dispatch switch compiles against. */
#include <syscall_nums.h>

.text
.global _start
_start:
    /* SP is already set by kernel ELF loader */
    call main
    /* Exit with main's return value */
    movq %rax, %rdi
    call _sys_exit
1:  hlt
    jmp 1b

.global _sys_open
_sys_open:
    movq $SYS_OPEN, %rax
    syscall
    ret

.global _sys_close
_sys_close:
    movq $SYS_CLOSE, %rax
    syscall
    ret

.global _sys_lseek
_sys_lseek:
    movq $SYS_LSEEK, %rax
    syscall
    ret

.global _sys_read
_sys_read:
    movq $SYS_READ, %rax
    syscall
    ret

.global _sys_write
_sys_write:
    movq $SYS_WRITE, %rax
    syscall
    ret

.global _sys_get_time
_sys_get_time:
    movq $SYS_GET_TIME, %rax
    syscall
    ret

.global _sys_get_pid
_sys_get_pid:
    movq $SYS_GETPID, %rax
    syscall
    ret

.global _sys_exit
_sys_exit:
    movq $SYS_EXIT, %rax
    syscall
    ret

.global _sys_spawn
_sys_spawn:
    movq $SYS_SPAWN, %rax
    syscall
    ret

.global _sys_spawn_caps
_sys_spawn_caps:
    movq $SYS_SPAWN_CAPS, %rax
    syscall
    ret

.global _sys_kill
_sys_kill:
    movq $SYS_KILL, %rax
    syscall
    ret

.global _sys_wait
_sys_wait:
    movq $SYS_WAIT, %rax
    syscall
    ret

.global _sys_yield
_sys_yield:
    movq $SYS_YIELD, %rax
    syscall
    ret

.global _sys_nanosleep
_sys_nanosleep:
    movq $SYS_NANOSLEEP, %rax
    syscall
    ret

/* long _sys_clock_gettime(int clk)  — clk in edi, returns ns in rax */
.global _sys_clock_gettime
_sys_clock_gettime:
    movq $SYS_CLOCK_GETTIME, %rax
    syscall
    ret

.global _sys_draw
_sys_draw:
    movq $SYS_DRAW, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_flush
_sys_flush:
    movq $SYS_FLUSH, %rax
    syscall
    ret

.global _sys_create_window
_sys_create_window:
    movq $SYS_CREATE_WINDOW, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_window_draw
_sys_window_draw:
    movq $SYS_WINDOW_DRAW, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

/* long _sys_window_write(int win_id, const char *buf, unsigned long count) */
.global _sys_window_write
_sys_window_write:
    movq $SYS_WINDOW_WRITE, %rax
    syscall
    ret

/* int _sys_window_of_pid(int pid) */
.global _sys_window_of_pid
_sys_window_of_pid:
    movq $SYS_WINDOW_OF_PID, %rax
    syscall
    ret

/* long _sys_window_grid(int win_id) */
.global _sys_window_grid
_sys_window_grid:
    movq $SYS_WINDOW_GRID, %rax
    syscall
    ret

/* long _sys_display_info(void) — packed (w<<16)|h (GFX-DYN-01) */
.global _sys_display_info
_sys_display_info:
    movq $SYS_DISPLAY_INFO, %rax
    syscall
    ret

/* int _sys_set_display_mode(int w, int h) */
.global _sys_set_display_mode
_sys_set_display_mode:
    movq $SYS_SET_DISPLAY_MODE, %rax
    syscall
    ret

/* int _sys_window_resize(int win_id, int w, int h) */
.global _sys_window_resize
_sys_window_resize:
    movq $SYS_WINDOW_RESIZE, %rax
    syscall
    ret

/* int _sys_display_poll(void) — returns 1 if a host display-change was applied */
.global _sys_display_poll
_sys_display_poll:
    movq $SYS_DISPLAY_POLL, %rax
    syscall
    ret

/* int _sys_set_style(int style_id, int theme_id) — -1 = keep */
.global _sys_set_style
_sys_set_style:
    movq $SYS_SET_STYLE, %rax
    syscall
    ret

/* int _sys_set_zoom(int percent) — desktop HiDPI/zoom */
.global _sys_set_zoom
_sys_set_zoom:
    movq $SYS_SET_ZOOM, %rax
    syscall
    ret

.global _sys_window_blit
_sys_window_blit:
    movq $SYS_WINDOW_BLIT, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_compositor_render
_sys_compositor_render:
    movq $SYS_COMPOSITOR_RENDER, %rax
    syscall
    ret

.global _sys_send
_sys_send:
    movq $SYS_SEND, %rax
    syscall
    ret

.global _sys_recv
_sys_recv:
    movq $SYS_RECV, %rax
    syscall
    ret

.global _sys_window_set_flags
_sys_window_set_flags:
    movq $SYS_WINDOW_SET_FLAGS, %rax
    syscall
    ret

.global _sys_destroy_window
_sys_destroy_window:
    movq $SYS_DESTROY_WINDOW, %rax
    syscall
    ret

.global _sys_registry
_sys_registry:
    movq $SYS_REGISTRY, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_get_procs
_sys_get_procs:
    movq $SYS_GETPROCS, %rax
    syscall
    ret

.global _sys_file_write
_sys_file_write:
    movq $SYS_FILE_WRITE, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_file_read
_sys_file_read:
    movq $SYS_FILE_READ, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_set_focus
_sys_set_focus:
    movq $SYS_SET_FOCUS, %rax
    syscall
    ret

.global _sys_try_recv
_sys_try_recv:
    movq $SYS_TRY_RECV, %rax
    syscall
    ret

.global _sys_sbrk
_sys_sbrk:
    movq $SYS_SBRK, %rax
    syscall
    ret

.global _sys_set_font
_sys_set_font:
    movq $SYS_SET_FONT, %rax
    syscall
    ret

.global _sys_list_dir
_sys_list_dir:
    movq $SYS_LIST_DIR, %rax
    syscall
    ret

.global _sys_chdir
_sys_chdir:
    movq $SYS_CHDIR, %rax
    syscall
    ret

.global _sys_getcwd
_sys_getcwd:
    movq $SYS_GETCWD, %rax
    syscall
    ret

/* --- Object / capability ABI (ASTRA §6.1/6.2/6.5) --- */
/* long _sys_handle_create(int ns, const char *path, unsigned rights, int type)
 * 4 args: the 4th (type) arrives in %rcx but the syscall ABI wants it in %r10. */
.global _sys_handle_create
_sys_handle_create:
    movq $SYS_HANDLE_CREATE, %rax
    movq %rcx, %r10      /* arg3 (type): rcx -> r10 */
    syscall
    ret

.global _sys_handle_dup
_sys_handle_dup:
    movq $SYS_HANDLE_DUP, %rax
    syscall
    ret

.global _sys_handle_close
_sys_handle_close:
    movq $SYS_HANDLE_CLOSE, %rax
    syscall
    ret

.global _sys_cap_query
_sys_cap_query:
    movq $SYS_CAP_QUERY, %rax
    syscall
    ret

.global _sys_cap_grant
_sys_cap_grant:
    movq $SYS_CAP_GRANT, %rax
    syscall
    ret

.global _sys_object_read
_sys_object_read:
    movq $SYS_OBJECT_READ, %rax
    syscall
    ret

.global _sys_object_write
_sys_object_write:
    movq $SYS_OBJECT_WRITE, %rax
    syscall
    ret

.global _sys_object_wait
_sys_object_wait:
    movq $SYS_OBJECT_WAIT, %rax
    syscall
    ret

.global _sys_object_ctl
_sys_object_ctl:
    movq $SYS_OBJECT_CTL, %rax
    syscall
    ret

/* long _sys_window_enum(struct window_info *buf, unsigned long max) */
.global _sys_window_enum
_sys_window_enum:
    movq $SYS_WINDOW_ENUM, %rax
    syscall
    ret
