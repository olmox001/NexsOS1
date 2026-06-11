/*
 * kernel/include/kernel/fault.h
 * Fault-safe reporting primitives (Phase A, steps 5-7).
 *
 * Everything declared here must be callable from an exception handler whose
 * address space, stack, or lock state may be compromised:
 *   - no spinlocks (a stuck CPU holding uart_lock must not deadlock us),
 *   - no per-CPU printk buffer,
 *   - no LAPIC-MMIO reads on amd64 (the mapping may be gone — the cpu id is
 *     recovered from the IA32_GS_BASE MSR instead),
 *   - bounded stack usage (callable from a 16KB IST/abort stack).
 */
#ifndef _KERNEL_FAULT_H
#define _KERNEL_FAULT_H

#include <stdarg.h>
#include <kernel/types.h>

struct cpu_info;
struct pt_regs;

/*
 * fault_printf / fault_vprintf - lock-free emergency console output.
 *
 * Formats into a stack buffer via the stateless vsnprintf and emits through
 * uart_putc_emergency (raw MMIO/port write, no locks).  Output may interleave
 * with a concurrent printk on another CPU; that is the accepted trade for
 * never deadlocking inside a fault handler.
 */
void fault_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fault_vprintf(const char *fmt, va_list args);

/*
 * Per-CPU fault recursion guard.
 *
 * fault_enter() increments the calling CPU's in_fault depth and returns the
 * new depth (1 = outermost fault).  A dispatcher seeing depth > 1 must emit
 * one raw fault_printf line and halt — converting any handler-in-handler
 * recursion into a clean stop instead of a triple fault / silent hang.
 * fault_exit() decrements on every path that resumes normal execution.
 *
 * If the per-CPU structure cannot be located safely (see below) a global
 * fallback counter is used so the guard still functions during early boot.
 */
unsigned fault_enter(void);
void fault_exit(void);
unsigned fault_depth(void);

/*
 * arch_cpu_info_fault_safe - per-HAL: locate this CPU's cpu_info via a path
 * with no faultable dependencies.
 *
 *   amd64:   reads the IA32_GS_BASE MSR (rdmsr — no memory access) and
 *            validates the value against the cpu_data[] bounds BEFORE any
 *            dereference, so a user GS base (syscall entry window) or an
 *            uninitialised MSR yields NULL instead of a nested fault.
 *   aarch64: derives the index from MPIDR_EL1 (register read, always safe).
 *
 * Returns NULL when the structure cannot be determined safely.
 */
struct cpu_info *arch_cpu_info_fault_safe(void);

/*
 * arch_frame_on_fault_stack - aarch64 HAL: whether an exception frame lives
 * on one of the per-CPU EL1 fault stacks (i.e. the vector switched stacks and
 * parked the original SP at frame+816).  amd64 does not define it — the IST
 * mechanism is transparent there.
 */
int arch_frame_on_fault_stack(const void *frame);

#endif /* _KERNEL_FAULT_H */
