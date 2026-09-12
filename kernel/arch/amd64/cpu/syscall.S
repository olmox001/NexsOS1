/*
 * kernel/arch/amd64/cpu/syscall.S
 * Fast-path syscall entry (SYSCALL instruction, MSR IA32_LSTAR)
 *
 * On entry (architectural — Intel SDM Vol.2A SYSCALL; AMD64 APM Vol.3):
 *   RCX = return RIP (the instruction after SYSCALL in user code)
 *   R11 = return RFLAGS (as it was before SYSCALL cleared IF/DF per FMASK)
 *   RSP = still the USER stack pointer; SYSCALL does not switch stacks
 *   CS/SS were loaded from IA32_STAR by hardware; GS is still the user GS
 *     base until this code executes 'swapgs'
 *
 * Sequence:
 *   1. swapgs to reach this CPU's struct cpu_info via %gs.
 *   2. Move to the kernel stack (cpu_info->stack_top), stashing the user
 *      RSP in cpu_info->user_stack_tmp first — there is nowhere else to put
 *      it, since no register survives SYSCALL untouched except the ones
 *      software chooses not to clobber before this point.
 *   3. Hand-build a struct pt_regs matching <arch/pt_regs.h> exactly, so the
 *      C dispatcher and the ISR path share one frame layout (DIR-06).
 *   4. Save the caller's FPU/SSE state, call the arch-neutral dispatcher.
 *   5. Restore the (possibly different, if the dispatcher scheduled another
 *      task) selected task's FPU/SSE state and GP registers.
 *   6. Return via iretq (see msr.c NOTE(SYS-AMD64-01): sysretq is cheaper and
 *      the RCX/R11 prerequisites are already met — not yet switched over).
 *
 * pt_regs.rcx / pt_regs.r11: by the time this code runs, RCX and R11 no
 * longer hold the user's original values — the CPU overwrote them with the
 * return RIP/RFLAGS as part of executing SYSCALL. This is the identical,
 * architecturally-unavoidable trade Linux's own entry_SYSCALL_64 documents:
 * pt_regs->cx and pt_regs->r11 are populated from those same clobbered
 * values (matching pt_regs->ip and pt_regs->flags) rather than the vanished
 * originals. This is why pt_regs_arg() (<arch/pt_regs.h>) reads syscall
 * argument 3 from R10, never RCX — R10 is the register the calling
 * convention keeps free specifically because RCX is not available.
 */
#include <kernel/cpu_asm_offsets.h>
#include "gdt_defs.h"

/* SYSCALL_ENTRY_VECTOR — not a real IDT vector (0x80 is a kernel-only gate;
 * SYS-AMD64-03 removed the legacy int 0x80 surface). This is a sentinel
 * written into pt_regs.vec purely so a frame built by this path is
 * self-identifying to anything that later inspects regs->vec (backtrace /
 * panic dumps, ptrace-style introspection) as "syscall entry", distinct from
 * a real exception or hardware IRQ frame. */
#define SYSCALL_ENTRY_VECTOR 0x80

.section .text
.extern amd64_fpu_save_task
.extern amd64_fpu_restore_task
.extern kernel_syscall_dispatcher
.global syscall_entry

syscall_entry:
    /* Swap to kernel GS so %gs:OFF reaches this CPU's struct cpu_info. */
    swapgs

    /* Stash the user RSP (nowhere else to keep it), then move to this CPU's
     * kernel stack. */
    movq %rsp, %gs:CPU_INFO_USER_STACK_TMP_OFF
    movq %gs:CPU_INFO_STACK_TOP_OFF, %rsp

    /* Build pt_regs on the kernel stack, high-to-low address matching the
     * struct pt_regs field order in <arch/pt_regs.h> exactly (ss..r15):
     * this constructs the same frame shape common_isr_entry builds in
     * isr_stubs.S, so amd64_isr_dispatch()/schedule()/restore_context can
     * treat a syscall frame and an interrupt frame identically. */
    pushq $USER_DS_SEL              /* ss */
    pushq %gs:CPU_INFO_USER_STACK_TMP_OFF /* rsp (the user RSP stashed above) */
    pushq %r11                      /* rflags (return RFLAGS, per SYSCALL) */
    pushq $USER_CS_SEL              /* cs */
    pushq %rcx                      /* rip (return RIP, per SYSCALL) */

    pushq $0                        /* err  (syscalls carry no error code) */
    pushq $SYSCALL_ENTRY_VECTOR     /* vec  (sentinel, see header comment) */

    /* GP registers, in the exact order struct pt_regs declares them
     * (rax first ... r15 last) — see the file header for why rcx/r11 end up
     * holding the clobbered return RIP/RFLAGS rather than their original
     * user values. */
    pushq %rax                      /* syscall number */
    pushq %rbx
    pushq %rcx                      /* == pt_regs.rip; see header comment */
    pushq %rdx                      /* arg 2 */
    pushq %rsi                      /* arg 1 */
    pushq %rdi                      /* arg 0 */
    pushq %rbp
    pushq %r8                       /* arg 4 */
    pushq %r9                       /* arg 5 */
    pushq %r10                      /* arg 3 (replaces rcx — see header) */
    pushq %r11                      /* == pt_regs.rflags; see header comment */
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* The dispatcher may call schedule(), which can hand execution to a
     * different task before returning here. Save the caller's x87/MMX/SSE
     * state before any C code is allowed to touch the XMM registers.
     * current_task is NULL only during early boot, before any task exists. */
    movq %gs:CPU_INFO_CURRENT_TASK_OFF, %rdi
    testq %rdi, %rdi
    jz 1f
    call amd64_fpu_save_task
1:
    movq %rsp, %rdi
    call kernel_syscall_dispatcher

    /* kernel_syscall_dispatcher() has published the selected task in
     * cpu_info (possibly not the caller, if it scheduled). rbx is
     * callee-saved by the C ABI and the caller's own rbx value is already
     * safe inside the pt_regs frame, so it is free to hold the returned
     * frame pointer across the FXRSTOR call below. */
    movq %rax, %rbx
    movq %gs:CPU_INFO_CURRENT_TASK_OFF, %rdi
    testq %rdi, %rdi
    jz 2f
    call amd64_fpu_restore_task
2:
    movq %rbx, %rsp

    /* Restore GP registers (exact reverse of the push sequence above). */
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %r11
    popq  %r10
    popq  %r9
    popq  %r8
    popq  %rbp
    popq  %rdi
    popq  %rsi
    popq  %rdx
    popq  %rcx
    popq  %rbx
    popq  %rax

    /* Skip the err/vec slots (not part of the CPU-defined iretq frame). */
    addq $16, %rsp

    /* NOTE(SYS-AMD64-01, msr.c): iretq is used instead of sysretq. Both the
     * RCX (return RIP) and R11 (return RFLAGS) prerequisites for sysretq
     * are met at this point — the switch is a drop-in replacement of the
     * next two instructions when the 20-50 cycle cost of iretq is no longer
     * acceptable; iretq was kept for now because it tolerates a return CS/SS
     * that sysretq's fixed +16/+8 STAR arithmetic does not. */
    swapgs
    iretq