/* ============================================================
 * hal/amd64/isr_stubs.S — 256 ISR entry stubs for x86-64
 * ============================================================
 * Each stub:
 *   1. Pushes a dummy error code (0) if the CPU doesn't push one
 *   2. Pushes the vector number
 *   3. Jumps to common_isr_entry
 *
 * common_isr_entry saves all GP registers and calls nexs_isr_dispatch(IsrFrame*).
 * ============================================================ */

.extern amd64_isr_dispatch

/* Vectors that push an error code automatically (Intel spec) */
#define HAS_ERR(v) ((v)==8||(v)==10||(v)==11||(v)==12||(v)==13||(v)==14||(v)==17||(v)==21||(v)==29||(v)==30)

/* Generate a stub for vector V */
.altmacro

.macro ISR_NOERR v
.global isr_stub_\v
isr_stub_\v:
    pushq $0
    pushq $\v
    jmp   common_isr_entry
.endm

.macro MAKE_ISR_NOERR v
    ISR_NOERR %v
.endm

.macro ISR_ERR v
.global isr_stub_\v
isr_stub_\v:
    pushq $\v
    jmp   common_isr_entry
.endm

.macro MAKE_ISR_ERR v
    ISR_ERR %v
.endm

.section .text

/* Exceptions 0-31 */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* IRQs / Software interrupts 32-255 */
.set V, 32
.rept 224
MAKE_ISR_NOERR V
.set V, V+1
.endr

/* ── Common handler ───────────────────────────────────────── */
common_isr_entry:
    /* Check if we came from user mode (CS & 3 != 0)
     * Stack layout: vec (0), err (8), rip (16), cs (24) */
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
    /* Save GP registers */
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    /* Pass pointer to pt_regs as first argument */
    movq  %rsp, %rdi
    call  amd64_isr_dispatch

    /* Restore GP registers */
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

    /* Swap GS back if we are returning to user mode */
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
    /* Skip vec + err pushed by stub */
    addq  $16, %rsp

    iretq

.macro QUAD_GEN v
    .quad isr_stub_\v
.endm

.macro DO_QUAD v
    QUAD_GEN %v
.endm

/* ── Table of stub addresses (used by idt_init) ─────────── */
.section .rodata
.global isr_stub_table
isr_stub_table:
.set V, 0
.rept 256
    DO_QUAD V
    .set V, V+1
.endr
