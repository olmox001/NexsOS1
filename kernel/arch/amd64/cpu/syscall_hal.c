/*
 * kernel/arch/amd64/cpu/syscall_hal.c
 * Thin AMD64 Syscall HAL — forwards to the arch-neutral dispatcher.
 *
 * The actual syscall dispatch logic lives in kernel/core/syscall_dispatch.c
 * (kernel_syscall_dispatcher). amd64_syscall_handler is a named hook for any
 * future amd64-specific pre/post processing (e.g. saving/restoring
 * additional state); it currently does nothing beyond forwarding the frame
 * pointer.
 *
 * Calling convention:
 *   amd64_syscall_handler is not called from any assembly path today — both
 *   the SYSCALL fast path (syscall.S) and idt.c:amd64_isr_dispatch invoke
 *   kernel_syscall_dispatcher directly. This function is retained as a
 *   named hook for future use.
 *
 * No known defects specific to this file.
 */
#include <arch/pt_regs.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/types.h>

extern struct pt_regs *kernel_syscall_dispatcher(struct pt_regs *regs);

struct pt_regs *amd64_syscall_handler(struct pt_regs *frame);

/*
 * amd64_syscall_handler - AMD64-specific syscall entry hook.
 *
 * Params:
 *   frame - pointer to the pt_regs saved frame built by the entry stub.
 *           On the SYSCALL fast path (syscall.S), this is the kernel stack
 *           frame with rax = syscall number and rdi/rsi/rdx/r10/r8/r9 = args.
 *
 * Returns the (possibly modified) pt_regs* that the entry stub will write to
 * RSP before restoring GP registers. A different return value performs an
 * in-place context switch.
 *
 * Currently a direct forward; no amd64-specific pre/post logic is applied.
 */
struct pt_regs *amd64_syscall_handler(struct pt_regs *frame) {
  return kernel_syscall_dispatcher(frame);
}