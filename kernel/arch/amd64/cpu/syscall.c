/*
 * kernel/arch/amd64/cpu/syscall.c
 * Thin HAL for AMD64 syscalls.
 * We must keep `kernel_syscall_dispatcher` generic.
 */
#include <kernel/types.h>
#include <arch/pt_regs.h>
#include <kernel/sched.h>
#include <kernel/printk.h>

/* The generic syscall dispatcher is in kernel/core/syscall.c (if we created it) 
 * or currently in aarch64/cpu/syscall.c for now. Wait, I should make sure 
 * I extract the dispatcher.
 */

/* For now, just forward it. */
extern struct pt_regs *kernel_syscall_dispatcher(struct pt_regs *regs);

/* Secure memory access helpers are implemented in kernel/arch/amd64/mm/uaccess.c */

struct pt_regs *amd64_syscall_handler(struct pt_regs *frame) {
  return kernel_syscall_dispatcher(frame);
}
