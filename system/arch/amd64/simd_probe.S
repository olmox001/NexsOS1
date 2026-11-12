/* Two distinct processes keep XMM0/XMM15 and MXCSR across 128 epochs.
 * Each epoch includes 131072 ALU iterations (IRQ opportunity), a nonblocking
 * syscall, a scheduler yield and a 1ms blocking sleep. No libc may spill XMM.
 * Only the architecture baseline SSE2 is used; no AVX/XSAVE assumption. */
#include <syscall_nums.h>
.text
.global os1_simd_context_probe
os1_simd_context_probe:
    pushq %r12
    subq $48, %rsp
    stmxcsr 32(%rsp)
    movl %edi, (%rsp)
    movl %edi, 4(%rsp)
    movl %edi, 8(%rsp)
    movl %edi, 12(%rsp)
    andl $3, %edi
    shll $13, %edi
    orl $0x1f80, %edi
    movl %edi, 36(%rsp)
    ldmxcsr 36(%rsp)
    movl $128, %r12d
.Lepoch:
    movdqu (%rsp), %xmm0
    movdqu (%rsp), %xmm15
    movl $131072, %ecx
.Lbusy:
    decl %ecx
    jnz .Lbusy
    movl $SYS_GETPID, %eax
    syscall
    testq %rax, %rax
    jle .Lsysfail
    movl $SYS_YIELD, %eax
    syscall
    movl $SYS_NANOSLEEP, %eax
    movl $1000000, %edi
    syscall
    testq %rax, %rax
    js .Lsysfail
    pcmpeqd (%rsp), %xmm0
    pmovmskb %xmm0, %eax
    cmpl $65535, %eax
    jne .Ldatafail
    pcmpeqd (%rsp), %xmm15
    pmovmskb %xmm15, %eax
    cmpl $65535, %eax
    jne .Ldatafail
    stmxcsr 40(%rsp)
    movl 40(%rsp), %eax
    cmpl 36(%rsp), %eax
    jne .Lcontrolfail
    decl %r12d
    jnz .Lepoch
    xorl %eax, %eax
    jmp .Ldone
.Ldatafail:
    movl $1, %eax
    jmp .Ldone
.Lcontrolfail:
    movl $2, %eax
    jmp .Ldone
.Lsysfail:
    movl $3, %eax
.Ldone:
    ldmxcsr 32(%rsp)
    addq $48, %rsp
    popq %r12
    ret
.section .note.GNU-stack,"",@progbits
