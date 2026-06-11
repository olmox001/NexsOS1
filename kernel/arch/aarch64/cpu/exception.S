/*
 * kernel/arch/aarch64/cpu/exception.S
 * Exception vector table for AArch64
 *
 * Exception frame layout (816 bytes):
 *   0-240:  x0-x30
 *   256:    elr_el1
 *   264:    spsr_el1
 *   272:    sp_el0
 *   288-800: q0-q31 (FPU/SIMD)
 *   800:    fpsr
 *   804:    fpcr
 */

.section .text

/*
 * Each vector must be 128 bytes (32 instructions)
 * Table must be 2KB aligned (0x800)
 */
.macro vector_entry label
.balign 128
\label:
.endm

.macro vector_stub name
    /* Allocate 816 byte exception frame (16-byte aligned) */
    sub sp, sp, #816
    
    /* Save general purpose registers x0-x30 */
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    
    /* Save exception state */
    mrs x0, elr_el1
    mrs x1, spsr_el1
    mrs x2, sp_el0
    str x0, [sp, #256] /* elr */
    str x1, [sp, #264] /* spsr */
    str x2, [sp, #272] /* sp_el0 */

    /* Save FPU/SIMD registers q0-q31 */
    stp q0, q1, [sp, #288]
    stp q2, q3, [sp, #320]
    stp q4, q5, [sp, #352]
    stp q6, q7, [sp, #384]
    stp q8, q9, [sp, #416]
    stp q10, q11, [sp, #448]
    stp q12, q13, [sp, #480]
    stp q14, q15, [sp, #512]
    stp q16, q17, [sp, #544]
    stp q18, q19, [sp, #576]
    stp q20, q21, [sp, #608]
    stp q22, q23, [sp, #640]
    stp q24, q25, [sp, #672]
    stp q26, q27, [sp, #704]
    stp q28, q29, [sp, #736]
    stp q30, q31, [sp, #768]

    /* Save FPU control/status */
    mrs x0, fpsr
    mrs x1, fpcr
    str w0, [sp, #800]
    str w1, [sp, #804]
    
    /* Call C handler with frame pointer */
    mov x0, sp
    bl \name\()_handler
    
    /* Handler returns pointer to new context (x0) */
    /* Switch stack pointer to the new process's kernel stack */
    mov sp, x0
    
    /* CRITICAL: Mask interrupts before restoring exception state to prevent races */
    msr daifset, #3

    /* Restore exception state */
    ldr x0, [sp, #256]
    ldr x1, [sp, #264]
    ldr x2, [sp, #272]
    msr elr_el1, x0
    msr spsr_el1, x1
    msr sp_el0, x2

    /* Restore FPU/SIMD registers q0-q31 */
    ldp q0, q1, [sp, #288]
    ldp q2, q3, [sp, #320]
    ldp q4, q5, [sp, #352]
    ldp q6, q7, [sp, #384]
    ldp q8, q9, [sp, #416]
    ldp q10, q11, [sp, #448]
    ldp q12, q13, [sp, #480]
    ldp q14, q15, [sp, #512]
    ldp q16, q17, [sp, #544]
    ldp q18, q19, [sp, #576]
    ldp q20, q21, [sp, #608]
    ldp q22, q23, [sp, #640]
    ldp q24, q25, [sp, #672]
    ldp q26, q27, [sp, #704]
    ldp q28, q29, [sp, #736]
    ldp q30, q31, [sp, #768]

    /* Restore FPU control/status */
    ldr w0, [sp, #800]
    ldr w1, [sp, #804]
    msr fpsr, x0
    msr fpcr, x1
    
    /* Restore general purpose registers */
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    
    /* Deallocate frame and return */
    add sp, sp, #816
    eret
.endm

/*
 * IRQ stub (same frame layout)
 */
.macro irq_stub
    sub sp, sp, #816
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    
    mrs x0, elr_el1
    mrs x1, spsr_el1
    mrs x2, sp_el0
    str x0, [sp, #256]
    str x1, [sp, #264]
    str x2, [sp, #272]

    /* Save FPU/SIMD registers q0-q31 */
    stp q0, q1, [sp, #288]
    stp q2, q3, [sp, #320]
    stp q4, q5, [sp, #352]
    stp q6, q7, [sp, #384]
    stp q8, q9, [sp, #416]
    stp q10, q11, [sp, #448]
    stp q12, q13, [sp, #480]
    stp q14, q15, [sp, #512]
    stp q16, q17, [sp, #544]
    stp q18, q19, [sp, #576]
    stp q20, q21, [sp, #608]
    stp q22, q23, [sp, #640]
    stp q24, q25, [sp, #672]
    stp q26, q27, [sp, #704]
    stp q28, q29, [sp, #736]
    stp q30, q31, [sp, #768]

    /* Save FPU control/status */
    mrs x0, fpsr
    mrs x1, fpcr
    str w0, [sp, #800]
    str w1, [sp, #804]
    
    /* Call IRQ handler */
    mov x0, sp
    bl irq_handler
    
    /* Switch stack pointer */
    mov sp, x0
    
    /* CRITICAL: Mask interrupts before restoring exception state */
    msr daifset, #3

    /* Restore exception state */
    ldr x0, [sp, #256]
    ldr x1, [sp, #264]
    ldr x2, [sp, #272]
    msr elr_el1, x0
    msr spsr_el1, x1
    msr sp_el0, x2

    /* Restore FPU/SIMD registers q0-q31 */
    ldp q0, q1, [sp, #288]
    ldp q2, q3, [sp, #320]
    ldp q4, q5, [sp, #352]
    ldp q6, q7, [sp, #384]
    ldp q8, q9, [sp, #416]
    ldp q10, q11, [sp, #448]
    ldp q12, q13, [sp, #480]
    ldp q14, q15, [sp, #512]
    ldp q16, q17, [sp, #544]
    ldp q18, q19, [sp, #576]
    ldp q20, q21, [sp, #608]
    ldp q22, q23, [sp, #640]
    ldp q24, q25, [sp, #672]
    ldp q26, q27, [sp, #704]
    ldp q28, q29, [sp, #736]
    ldp q30, q31, [sp, #768]

    /* Restore FPU control/status */
    ldr w0, [sp, #800]
    ldr w1, [sp, #804]
    msr fpsr, x0
    msr fpcr, x1
    
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #816
    
    eret
.endm

/*
 * Exception Vector Table
 * Must be 2KB aligned
 */
/*
 * Exception Vector Table
 * Must be 2KB aligned
 */
.balign 0x800
.global exception_vectors
exception_vectors:

/* Current EL with SP_EL0 */
.balign 128
el1_sp0_sync:
    b handle_el1_sp0_sync
.balign 128
el1_sp0_irq:
    b handle_el1_sp0_irq
.balign 128
el1_sp0_fiq:
    b handle_unexpected_fiq
.balign 128
el1_sp0_serror:
    b handle_el1_sp0_serror

/* Current EL with SP_ELx */
.balign 128
el1_spx_sync:
    b handle_el1_spx_sync
.balign 128
el1_spx_irq:
    b handle_el1_spx_irq
.balign 128
el1_spx_fiq:
    b handle_unexpected_fiq
.balign 128
el1_spx_serror:
    b handle_el1_spx_serror

/* Lower EL using AArch64 */
.balign 128
el0_64_sync:
    b handle_el0_64_sync
.balign 128
el0_64_irq:
    b handle_el0_64_irq
.balign 128
el0_64_fiq:
    b handle_unexpected_fiq
.balign 128
el0_64_serror:
    b handle_el0_64_serror

/* Lower EL using AArch32 (unsupported: kernel runs AArch64-only user code).
 * Phase A step 13: these were silent `b .` hangs — now reported + handled. */
.balign 128
el0_32_sync:
    b handle_el0_aarch32
.balign 128
el0_32_irq:
    b handle_el0_aarch32
.balign 128
el0_32_fiq:
    b handle_el0_aarch32
.balign 128
el0_32_serror:
    b handle_el0_aarch32

/* 
 * Actual Handlers (Outside the table)
 */
handle_el1_sp0_sync:
    stp x0, x1, [sp, #-16]!
    mov x0, #0x09000000
    mov w1, #75
    /* str w1, [x0] */
    ldp x0, x1, [sp], #16
    vector_stub sync

handle_el1_sp0_irq:
    irq_stub

handle_el1_sp0_serror:
    vector_stub serror

/*
 * EL1-from-EL1 synchronous abort (Phase A step 4): run the handler on the
 * per-CPU fault stack so a kernel-stack overflow or wild SP cannot recurse.
 *
 * The original SP is parked in the 16-byte slot directly above the 816-byte
 * frame (i.e. at frame+816): sync_handler reads it for diagnostics and, on
 * the only resuming path (probe fixup), copies the frame back below the
 * original SP so the eret epilogue (mov sp, x0 ... add sp, #816) leaves SP
 * exactly where the faulting code expects it.
 *
 * The 16-byte scratch push on the old SP is the unavoidable minimum — if even
 * that aborts, the CPU was unrecoverable before this code ran.  A nested
 * abort while already on the fault stack re-parks at the top and clobbers the
 * outer frame; that is caught immediately by the fault_enter() depth guard.
 */
handle_el1_spx_sync:
    stp x0, x1, [sp, #-16]!
    mrs x0, mpidr_el1
    and x0, x0, #0xFF
    adrp x1, arch_fault_stack_top
    add  x1, x1, :lo12:arch_fault_stack_top
    ldr  x1, [x1, x0, lsl #3]
    cbz  x1, 1f                  /* fault stack not published yet: stay */
    mov  x0, sp                  /* x0 = old SP (after the 16-byte push)  */
    str  x0, [x1, #-16]!         /* park it; x1 -> top-16                 */
    mov  sp, x1                  /* switch to the fault stack             */
    ldp  x0, x1, [x0]            /* restore scratch from the old stack    */
    b    2f
1:
    ldp x0, x1, [sp], #16
2:
    vector_stub sync

handle_el1_spx_irq:
    stp x0, x1, [sp, #-16]!
    mov x0, #0x09000000
    mov w1, #105
    /* str w1, [x0] */
    ldp x0, x1, [sp], #16
    irq_stub

/* EL1-from-EL1 SError: same fault-stack switch as el1_spx_sync.  The SError
 * handler never resumes (panic), so no copy-back path is needed here. */
handle_el1_spx_serror:
    stp x0, x1, [sp, #-16]!
    mrs x0, mpidr_el1
    and x0, x0, #0xFF
    adrp x1, arch_fault_stack_top
    add  x1, x1, :lo12:arch_fault_stack_top
    ldr  x1, [x1, x0, lsl #3]
    cbz  x1, 1f
    mov  x0, sp
    str  x0, [x1, #-16]!
    mov  sp, x1
    ldp  x0, x1, [x0]
    b    2f
1:
    ldp x0, x1, [sp], #16
2:
    vector_stub serror

handle_el0_64_sync:
    stp x0, x1, [sp, #-16]!
    mov x0, #0x09000000
    mov w1, #83  /* 'S' */
    /* str w1, [x0] */
    ldp x0, x1, [sp], #16
    vector_stub syscall

handle_el0_64_irq:
    stp x0, x1, [sp, #-16]!
    mov x0, #0x09000000
    mov w1, #73
    /* str w1, [x0] */
    ldp x0, x1, [sp], #16
    irq_stub

handle_el0_64_serror:
    vector_stub serror

/* Phase A step 13: no silent halts.  FIQ is never configured by this kernel
 * and AArch32 EL0 is unsupported; both were `b .` infinite loops (a wedged
 * core with no diagnostic).  Build the standard frame and report via the
 * fault-path C handlers (terminate the offending EL0 task / panic for EL1). */
handle_unexpected_fiq:
    vector_stub fiq

handle_el0_aarch32:
    vector_stub aarch32

/*
 * Install exception vector table
 */
.global exception_vectors_install
exception_vectors_install:
    adrp x0, exception_vectors
    add x0, x0, :lo12:exception_vectors
    msr vbar_el1, x0
    isb
    isb
    ret

/*
 * Enter User Mode
 * void enter_user_mode(uint64_t entry, uint64_t sp);
 */
.global arch_enter_user_mode
arch_enter_user_mode:
    /* x0 = entry point (ELR_EL1) */
    /* x1 = stack pointer (SP_EL0) */
    /* x2 = kernel stack pointer (SP_EL1) */
    
    msr elr_el1, x0
    msr sp_el0, x1
    mov sp, x2
    
    /* SPSR_EL1
       EL0t (0) + Unmask Interrupts (0)
    */
    mov x2, #0
    msr spsr_el1, x2
    
    /* Clear general purpose registers to not leak kernel data */
    mov x0, #0
    mov x1, #0
    mov x2, #0
    mov x3, #0
    mov x4, #0
    mov x5, #0
    mov x6, #0
    mov x7, #0
    mov x8, #0
    mov x9, #0
    mov x10, #0
    mov x11, #0
    mov x12, #0
    mov x13, #0
    mov x14, #0
    mov x15, #0
    mov x16, #0
    mov x17, #0
    mov x18, #0
    mov x19, #0
    mov x20, #0
    mov x21, #0
    mov x22, #0
    mov x23, #0
    mov x24, #0
    mov x25, #0
    mov x26, #0
    mov x27, #0
    mov x28, #0
    mov x29, #0
    mov x30, #0

    eret
