/*
 * user/arch/amd64/syscall.S
 * x86-64 System Call Wrappers
 *
 * ABI:
 *   RAX: syscall number
 *   RDI, RSI, RDX, R10, R8, R9: arguments (R10 replaces RCX)
 *   syscall instruction traps to kernel
 */
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

.global _sys_read
_sys_read:
    movq $63, %rax
    syscall
    ret

.global _sys_write
_sys_write:
    movq $64, %rax
    syscall
    ret

.global _sys_get_time
_sys_get_time:
    movq $169, %rax
    syscall
    ret

.global _sys_get_pid
_sys_get_pid:
    movq $172, %rax
    syscall
    ret

.global _sys_exit
_sys_exit:
    movq $93, %rax
    syscall
    ret

.global _sys_spawn
_sys_spawn:
    movq $220, %rax
    syscall
    ret

.global _sys_kill
_sys_kill:
    movq $221, %rax
    syscall
    ret

.global _sys_wait
_sys_wait:
    movq $247, %rax
    syscall
    ret

.global _sys_yield
_sys_yield:
    movq $223, %rax
    syscall
    ret

.global _sys_draw
_sys_draw:
    movq $200, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_flush
_sys_flush:
    movq $201, %rax
    syscall
    ret

.global _sys_create_window
_sys_create_window:
    movq $210, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_window_draw
_sys_window_draw:
    movq $211, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_window_blit
_sys_window_blit:
    movq $213, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_compositor_render
_sys_compositor_render:
    movq $212, %rax
    syscall
    ret

.global _sys_send
_sys_send:
    movq $230, %rax
    syscall
    ret

.global _sys_recv
_sys_recv:
    movq $231, %rax
    syscall
    ret

.global _sys_window_set_flags
_sys_window_set_flags:
    movq $214, %rax
    syscall
    ret

.global _sys_destroy_window
_sys_destroy_window:
    movq $215, %rax
    syscall
    ret

.global _sys_registry
_sys_registry:
    movq $250, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_get_procs
_sys_get_procs:
    movq $222, %rax
    syscall
    ret

.global _sys_file_write
_sys_file_write:
    movq $251, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_file_read
_sys_file_read:
    movq $252, %rax
    movq %rcx, %r10   /* arg3: rcx → r10 */
    syscall
    ret

.global _sys_set_focus
_sys_set_focus:
    movq $232, %rax
    syscall
    ret

.global _sys_try_recv
_sys_try_recv:
    movq $32, %rax
    syscall
    ret

.global _sys_sbrk
_sys_sbrk:
    movq $216, %rax
    syscall
    ret

.global _sys_set_font
_sys_set_font:
    movq $253, %rax
    syscall
    ret

.global _sys_list_dir
_sys_list_dir:
    movq $254, %rax
    syscall
    ret

.global _sys_chdir
_sys_chdir:
    movq $255, %rax
    syscall
    ret

.global _sys_getcwd
_sys_getcwd:
    movq $256, %rax
    syscall
    ret
