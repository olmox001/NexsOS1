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

/*
 * fault_handle_user_or_panic - generic user-vs-kernel fault decision
 * (kernel/core/fault.c, Phase A step 8).
 *
 * Returns the next frame to restore (the fault was user-attributable: the
 * process was terminated and schedule() picked a successor), or NULL (kernel
 * bug: the caller must dump its arch state and panic).  user_mode is the
 * arch's privilege test at fault time (CS RPL / SPSR.M); fault_addr is
 * CR2/FAR (0 when the vector has no fault address); syndrome is the arch fault
 * cause register (aarch64 ESR / amd64 error code, 0 when none) — logged
 * uniformly so the user-fault debug is identical across arches (DIR-06 HAL
 * conformance: the reporting lives HERE, not duplicated per arch).
 */
struct pt_regs *fault_handle_user_or_panic(struct pt_regs *regs, int user_mode,
                                           uint64_t fault_addr, uint64_t fault_pc,
                                           const char *desc, uint64_t syndrome);

/*
 * arch_uaccess_fault_fixup - per-HAL: release the arch_copy_{from,to}_user
 * critical section after a fault inside the copy window (clear the per-CPU
 * uaccess_active flag, drop mm_lock / restore IRQ state where the arch holds
 * them).  Called only by fault_handle_user_or_panic on the uaccess path.
 */
void arch_uaccess_fault_fixup(void);

/*
 * Symbolized frame-pointer backtrace (kernel/lib/backtrace.c, steps 11-12).
 * backtrace_regs seeds from an exception frame (pc = RIP/ELR, fp = RBP/X29);
 * backtrace_here walks from the current call site (used by panic()).
 * ksym_lookup resolves a text address against the .ksyms blob; returns NULL
 * (and prints raw) when no table is linked.  All output via fault_printf.
 */
void backtrace_regs(uint64_t pc, uint64_t fp);
void backtrace_here(void);
const char *ksym_lookup(uint64_t addr, uint64_t *off);

#endif /* _KERNEL_FAULT_H */
