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

/* Secure memory access helpers */
int arch_copy_from_user(void *dest, const void *src, size_t n) {
  uint64_t src_addr = (uint64_t)src;
  if (src_addr + n < src_addr) return -1;
  if (!vmm_is_user_addr(src_addr) || !vmm_is_user_addr(src_addr + n)) return -1;
  if (!current_process || !current_process->page_table) return -1;

  /* Check if range is valid in user page table */
  if (vmm_check_range(current_process->page_table, src_addr, n, PTE_PRESENT) != 0)
    return -1;

  /* On x86-64, we don't need to switch page tables if we are in the process context,
   * but we should eventually handle SMAP (stac/clac) if enabled.
   */
  memcpy(dest, src, n);
  return 0;
}

int arch_copy_to_user(void *dest, const void *src, size_t n) {
  uint64_t dest_addr = (uint64_t)dest;
  if (dest_addr + n < dest_addr) return -1;
  if (!vmm_is_user_addr(dest_addr) || !vmm_is_user_addr(dest_addr + n)) return -1;
  if (!current_process || !current_process->page_table) return -1;

  if (vmm_check_range(current_process->page_table, dest_addr, n, PTE_PRESENT) != 0)
    return -1;

  memcpy(dest, src, n);
  return 0;
}

int arch_copy_string_from_user(char *dest, const char *src, size_t max_len) {
  if (!vmm_is_user_addr((uint64_t)src)) return -1;
  if (!current_process || !current_process->page_table) return -1;

  size_t i;
  int ret = 0;
  for (i = 0; i < max_len - 1; i++) {
    if (((uint64_t)&src[i] & 0xFFF) == 0) {
       if (vmm_check_range(current_process->page_table, (uint64_t)&src[i], 1, PTE_PRESENT) != 0) {
         ret = -1;
         break;
       }
    }
    dest[i] = src[i];
    if (src[i] == '\0') break;
  }
  dest[max_len - 1] = '\0';
  return ret;
}

struct pt_regs *amd64_syscall_handler(struct pt_regs *frame) {
  return kernel_syscall_dispatcher(frame);
}
