/* Same bounded context transaction as AMD64; Q0/Q31 and FPCR.
 * Baseline FP/ASIMD only, no optional SVE state assumed. */
#include <syscall_nums.h>
.text
.global os1_simd_context_probe
os1_simd_context_probe:
    stp x19, x20, [sp, #-48]!
    stp x21, x22, [sp, #16]
    mrs x22, fpcr
    mov w19, w0
    orr x19, x19, x19, lsl #32
    and x20, x19, #3
    lsl x20, x20, #22
    msr fpcr, x20
    mov w21, #128
.Lepoch:
    dup v0.2d, x19
    dup v31.2d, x19
    mov w9, #131072
.Lbusy:
    subs w9, w9, #1
    b.ne .Lbusy
    mov x8, #SYS_GETPID
    svc #0
    cmp x0, #0
    b.le .Lsysfail
    mov x8, #SYS_YIELD
    svc #0
    mov x8, #SYS_NANOSLEEP
    movz x0, #0x4240
    movk x0, #0xf, lsl #16
    svc #0
    cmp x0, #0
    b.lt .Lsysfail
    umov x9, v0.d[0]
    cmp x9, x19
    b.ne .Ldatafail
    umov x9, v0.d[1]
    cmp x9, x19
    b.ne .Ldatafail
    umov x9, v31.d[0]
    cmp x9, x19
    b.ne .Ldatafail
    umov x9, v31.d[1]
    cmp x9, x19
    b.ne .Ldatafail
    mrs x9, fpcr
    cmp x9, x20
    b.ne .Lcontrolfail
    subs w21, w21, #1
    b.ne .Lepoch
    mov x0, #0
    b .Ldone
.Ldatafail:
    mov x0, #1
    b .Ldone
.Lcontrolfail:
    mov x0, #2
    b .Ldone
.Lsysfail:
    mov x0, #3
.Ldone:
    msr fpcr, x22
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #48
    ret
.section .note.GNU-stack,"",%progbits
