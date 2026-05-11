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

    /* Store user RSP in cpu_info->user_stack_tmp (offset 16) */
    movq %rsp, %gs:16
    /* Load kernel RSP from cpu_info->stack_top (offset 8) */
    movq %gs:8, %rsp

    /* Now we are on kernel stack. Construct pt_regs.
     * Match layout in pt_regs.h:
     * CPU pushes (conceptually): SS, RSP, RFLAGS, CS, RIP
     * We simulate the iret frame for compatibility with ISR dispatch
     */
    pushq $0x18         /* User SS */
    pushq %gs:16        /* User RSP */
    pushq %r11          /* RFLAGS (saved in r11 by syscall) */
    pushq $0x20         /* User CS */
    pushq %rcx          /* RIP (saved in rcx by syscall) */

    /* "Error Code" and "Vector" */
    pushq $0            /* Error code */
    pushq $0x80         /* Vector (dummy for syscall) */

    /* GP Registers */
    pushq %rax          /* System call number */
    pushq %rbx
    pushq %rcx          /* RCX holds RIP, but we push it anyway */
    pushq %rdx          /* Arg 2 */
    pushq %rsi          /* Arg 1 */
    pushq %rdi          /* Arg 0 */
    pushq %rbp
    pushq %r8           /* Arg 4 */
    pushq %r9           /* Arg 5 */
    pushq %r10          /* Arg 3 (R10 replaces RCX) */
    pushq %r11          /* R11 holds RFLAGS */
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Call C handler */
    movq %rsp, %rdi
    call kernel_syscall_dispatcher

    /* Restore registers */
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Skip error code and vector */
    addq $16, %rsp

    /* Pop iret frame manually for SYSRET */
    popq %rcx           /* RIP */
    addq $8, %rsp       /* Skip CS */
    popq %r11           /* RFLAGS */
    popq %rsp           /* User RSP */
    /* SS is implicit in SYSRET */

    /* Swap GS back to user data */
    swapgs

    sysretq
