/*
 * kernel/arch/amd64/include/arch/irq_vectors.h
 * Canonical x86-64 interrupt-vector numbering for this kernel.
 *
 * Single source of truth for every IDT vector number that has meaning
 * outside the file that owns the hardware it names.  Before this header
 * existed, the values 32, 39, 47, 48 and 0xFF were duplicated as bare
 * integer literals in kernel/arch/amd64/cpu/idt.c, kernel/arch/amd64/cpu/apic.c
 * and kernel/drivers/timer/pic_pit.c; a change to the legacy PIC remap base
 * in one file had no compiler-visible link to the matching literal in the
 * other two.  Every one of those sites now includes this header instead.
 *
 * Numbering rationale (Intel SDM Vol.3 chapter 6, and Linux
 * arch/x86/include/asm/irq_vectors.h, whose naming this mirrors):
 *   0-31    reserved for CPU exceptions/traps (see idt.c: enum x86_trap).
 *   32-47   legacy 8259 PIC IRQ0-IRQ15, remapped by pic_init() (ICW2).
 *   32      is also the LAPIC periodic timer vector (LOCAL_TIMER_VECTOR):
 *           PIC IRQ0 is masked before the LAPIC timer is armed, so vector 32
 *           has exactly one source once boot completes (EXC-AMD64-03).
 *   0xFE    fixed-vector LAPIC IPI used for TLB shootdown broadcast.
 *   0xFF    LAPIC spurious-interrupt vector (programmed into the SVR).
 */
#ifndef _ARCH_AMD64_IRQ_VECTORS_H
#define _ARCH_AMD64_IRQ_VECTORS_H

/* First IDT vector available for hardware IRQs (Intel SDM: vectors 0-31 are
 * reserved for architectural exceptions). */
#define FIRST_EXTERNAL_VECTOR   32U

/* Legacy 8259 PIC remap window: pic_init() programs ICW2 on PIC1 with
 * IRQ0_VECTOR and PIC2 with IRQ0_VECTOR + 8, so PIC hardware line n always
 * arrives as vector IRQ0_VECTOR + n. */
#define IRQ0_VECTOR             FIRST_EXTERNAL_VECTOR   /* 32 */
#define NR_LEGACY_IRQS          16U                      /* 8259 pair: 2x8 lines */
#define LEGACY_VECTOR_END       (IRQ0_VECTOR + NR_LEGACY_IRQS - 1U) /* 47 */

/* Master/slave 8259 spurious-IRQ vectors (lowest-priority line on each
 * chip): see pic_handle_spurious() for the ISR-register check that
 * distinguishes a real IRQ7/IRQ15 from a spurious one on these vectors. */
#define IRQ7_VECTOR              (IRQ0_VECTOR + 7U)   /* 39: master spurious */
#define IRQ15_VECTOR             (IRQ0_VECTOR + 15U)  /* 47: slave spurious  */

/* LAPIC periodic timer vector (lapic_timer_setup() programs LVT_TIMER with
 * this value).  Deliberately aliased to IRQ0_VECTOR: see rationale above. */
#define LOCAL_TIMER_VECTOR       IRQ0_VECTOR            /* 32 */

/* Fixed-vector IPIs (never routed through the 8259; chosen from the top of
 * the vector space so they cannot collide with a PCI/MSI allocation). */
#define TLB_SHOOTDOWN_IPI_VECTOR 0xFDU
#define HALT_IPI_VECTOR          0xFEU

/* LAPIC spurious-interrupt vector, programmed into the SVR (LAPIC_SVR bits
 * [7:0]) by lapic_init(); delivered when the LAPIC arbitrates an interrupt
 * that is withdrawn before it can be serviced. Requires no EOI. */
#define SPURIOUS_APIC_VECTOR     0xFFU

#endif /* _ARCH_AMD64_IRQ_VECTORS_H */
