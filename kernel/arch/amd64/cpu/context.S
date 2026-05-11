/* ============================================================
 * kernel/arch/amd64/cpu/context.S — x86-64 Context Switch & User Mode
 * ============================================================ */

.section .text

/*
 * void ctx_switch(void *from_ctx, void *to_ctx)
 * Ctx_amd64 layout: rbx, rbp, r12-r15, rsp, rip
 */
.global ctx_switch
ctx_switch:
    /* Save callee-saved registers of current process */
    movq  %rbx,  0(%rdi)
    movq  %rbp,  8(%rdi)
    movq  %r12, 16(%rdi)
    movq  %r13, 24(%rdi)
    movq  %r14, 32(%rdi)
    movq  %r15, 40(%rdi)
    /* Save RSP */
    movq  %rsp, 48(%rdi)
    /* Save RIP */
    movq  (%rsp), %rax
    movq  %rax, 56(%rdi)

    /* Restore callee-saved registers of next process */
    movq  0(%rsi),  %rbx
    movq  8(%rsi),  %rbp
    movq  16(%rsi), %r12
    movq  24(%rsi), %r13
    movq  32(%rsi), %r14
    movq  40(%rsi), %r15
    movq  48(%rsi), %rsp
    /* Jump to saved RIP */
    movq  56(%rsi), %rax
    jmpq  *%rax

/*
 * void arch_enter_user_mode(uint64_t entry_point, uint64_t user_sp, uint64_t kernel_sp)
 * rdi = entry_point
 * rsi = user_sp
 * rdx = kernel_sp
 */
.global arch_enter_user_mode
arch_enter_user_mode:
    /* We don't strictly need to switch to kernel_sp here if we're going to iretq
     * and the scheduler handles the main stack. But for safety, we set it.
     * Wait, if we change RSP now, we lose the return address. But we don't return.
     */
    movq %rdx, %rsp

    /* Construct iretq frame for Ring 3 */
    /* Push SS (GDT_USER_DATA | 3) */
    pushq $0x1B
    
    /* Push RSP */
    pushq %rsi

    /* Push RFLAGS (Enable Interrupts IF=1) */
    pushfq
    movq (%rsp), %rax
    orq $0x200, %rax
    movq %rax, (%rsp)

    /* Push CS (GDT_USER_CODE | 3) */
    pushq $0x23

    /* Push RIP */
    pushq %rdi

    /* Clear registers to prevent leaking kernel data to user space */
    xorq %rax, %rax
    xorq %rbx, %rbx
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %r8, %r8
    xorq %r9, %r9
    xorq %r10, %r10
    xorq %r11, %r11
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    xorq %rbp, %rbp
    xorq %rdi, %rdi
    xorq %rsi, %rsi

    /* swapgs not needed here if we haven't loaded kernel GS base yet,
     * but standard x86-64 uses it. For our single-core, we'll skip it or 
     * configure it later. */
    
    iretq
