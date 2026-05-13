/*
 * kernel/arch/amd64/cpu/syscall.S
 * Fast Path Syscall Entry (MSR LSTAR)
 */

.section .text
.global syscall_entry
.extern kernel_syscall_dispatcher

/*
 * On entry:
 *   RCX = Return RIP
 *   R11 = Return RFLAGS
 *   RSP = User Stack Pointer (NOT kernel stack!)
 *
 * We must:
 *   1. SwapGS to get kernel per-CPU data (which contains kernel RSP)
 *   2. Switch RSP to kernel stack
 *   3. Push `struct pt_regs`
 *   4. Call C handler
 *   5. Restore registers
 *   6. sysretq
 */

/* Minimal GS offsets for now.
 * GS:0x00 will be Kernel RSP.
 * GS:0x08 will be User RSP temp storage.
 * Actually, to avoid SwapGS complexity initially, we can use the TSS rsp0
 * directly if we are single-core, or just a global kernel_rsp.
 * Wait, SYSCALL doesn't switch stacks automatically like interrupts do.
 * If we don't have per-CPU GS set up yet, we can use a global variable
 * to store the user RSP and load the kernel RSP, but that's not thread-safe.
 * Let's assume SwapGS works and we set GS base to a `struct cpu_info` later.
 */

syscall_entry:
    /* Swap GS to kernel per-CPU data */
    swapgs

    /* Store user RSP in cpu_info->user_stack_tmp (offset 24) */
    movq %rsp, %gs:24
    /* Load kernel RSP from cpu_info->stack_top (offset 16) */
    movq %gs:16, %rsp

    /* Now we are on kernel stack. Construct pt_regs.
     * Match layout in pt_regs.h:
     * CPU pushes (conceptually): SS, RSP, RFLAGS, CS, RIP
     * We simulate the iret frame for compatibility with ISR dispatch
     */
    /* Build pt_regs frame manually (to match isr_stubs.S) 
     * Correct Order (High to Low): SS, RSP, RFLAGS, CS, RIP */
    pushq $0x1B         /* SS */
    pushq %gs:24        /* User RSP */
    pushq %r11          /* RFLAGS (saved in r11 by syscall) */
    pushq $0x23         /* CS */
    pushq %rcx          /* RIP (saved in rcx by syscall) */

    /* "Error Code" and "Vector" */
    pushq $0            /* Error code */
    pushq $0x80         /* Vector (dummy for syscall) */

    /* GP Registers */
    pushq %rax          /* System call number */
    pushq %rbx
    pushq %rcx          /* User RIP (saved by syscall) */
    pushq %rdx          /* Arg 2 */
    pushq %rsi          /* Arg 1 */
    pushq %rdi          /* Arg 0 */
    pushq %rbp
    pushq %r8           /* Arg 4 */
    pushq %r9           /* Arg 5 */
    pushq %r10          /* Arg 3 */
    pushq %r11          /* User RFLAGS (saved by syscall) */
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Call C handler */
    movq %rsp, %rdi
    call kernel_syscall_dispatcher
    movq %rax, %rsp

    /* Restore registers */
    /* Restore GP registers (Reverse order of pushes!) */
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

    /* Skip error code and vector */
    addq $16, %rsp

    /* Return via iretq (more robust than sysretq for now) */
    swapgs
    iretq
