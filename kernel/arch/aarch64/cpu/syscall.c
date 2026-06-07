/*
 * kernel/arch/aarch64/cpu/syscall.c
 * System Call Handler
 */
#include <drivers/uart.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/ext4.h>
#include <kernel/graphics.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>
#include <kernel/registry.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <stdint.h>

extern volatile uint64_t jiffies;
extern struct pt_regs *schedule(struct pt_regs *regs);
extern int process_terminate(int pid);

extern int compositor_get_window_by_pid(int pid);
extern void compositor_window_write(int win_id, const char *buf, size_t count);
extern void compositor_blit(int win_id, int x, int y, int w, int h,
                            const uint32_t *buf, int pid);
extern int compositor_create_window(int x, int y, int w, int h,
                                    const char *title, int pid);
extern void compositor_draw_rect(int window_id, int x, int y, int w, int h,
                                 uint32_t color, int caller_pid);
extern void compositor_render(void);
extern void compositor_set_window_flags(int window_id, int flags);

/* Secure memory access helpers with Page Table Switching */
int arch_copy_from_user(void *dest, const void *src, size_t n) {
  uint64_t src_addr = (uint64_t)src;
  if (src_addr + n < src_addr)
    return -1; /* Wrap around */

  if (!vmm_is_user_addr(src_addr) || !vmm_is_user_addr(src_addr + n))
    return -1;

  if (!current_process || !current_process->page_table)
    return -1;

  /* Check if range is valid and mapped in user page table */
  if (vmm_check_range(current_process->page_table, src_addr, n, PTE_VALID) != 0)
    return -1;

  /* Save and disable interrupts to prevent scheduler preemption */
  uint64_t flagsptr = local_irq_save();

  /* Lock the address space for this process */
  spin_lock(&current_process->mm_lock);

  /* Save kernel TTBR0 (usually 0 or points to identity map initially) */
  uint64_t old_pgd = arch_vmm_get_pgd();

  /* Switch to user's page table (must use physical address) */
  arch_vmm_set_pgd(virt_to_phys(current_process->page_table));
  arch_tlb_flush_all();
  arch_isb();

  /* Perform copy while user space is mapped at TTBR0 */
  memcpy(dest, src, n);

  /* Restore kernel/previous TTBR0 */
  arch_vmm_set_pgd(old_pgd);
  arch_tlb_flush_all();
  arch_isb();

  spin_unlock(&current_process->mm_lock);
  local_irq_restore(flagsptr);

  return 0;
}

int arch_copy_to_user(void *dest, const void *src, size_t n) {
  uint64_t dest_addr = (uint64_t)dest;
  if (dest_addr + n < dest_addr)
    return -1; /* Wrap around */
    
  if (!vmm_is_user_addr(dest_addr) ||
      !vmm_is_user_addr(dest_addr + n))
    return -1;

  /* UACC-AARCH64-01: guard current_process before dereferencing its
   * page_table, matching arch_copy_from_user above. */
  if (!current_process || !current_process->page_table)
    return -1;

  if (vmm_check_range(current_process->page_table, dest_addr, n, PTE_VALID) != 0)
    return -1;

  uint64_t flagsptr = local_irq_save();
  spin_lock(&current_process->mm_lock);

  uint64_t old_pgd = arch_vmm_get_pgd();
  arch_vmm_set_pgd(virt_to_phys(current_process->page_table));
  arch_tlb_flush_all();
  arch_isb();

  memcpy(dest, src, n);

  arch_vmm_set_pgd(old_pgd);
  arch_tlb_flush_all();
  arch_isb();

  spin_unlock(&current_process->mm_lock);
  local_irq_restore(flagsptr);

  return 0;
}

/* Copy null-terminated string from user space safely with Page Table Switching
 */
int arch_copy_string_from_user(char *dest, const char *src, size_t max_len) {
  if (!vmm_is_user_addr((uint64_t)src))
    return -1;

  if (!current_process || !current_process->page_table)
    return -1;

  uint64_t flagsptr = local_irq_save();
  spin_lock(&current_process->mm_lock);

  uint64_t old_pgd = arch_vmm_get_pgd();
  arch_vmm_set_pgd(virt_to_phys(current_process->page_table));
  arch_tlb_flush_all();
  arch_isb();

  int ret = 0;
  size_t i;
  for (i = 0; i < max_len - 1; i++) {
    /* Check each page boundary for mapping if we cross it */
    if (((uint64_t)&src[i] & 0xFFF) == 0) {
       if (vmm_check_range(current_process->page_table, (uint64_t)&src[i], 1, PTE_VALID) != 0)
         goto out;
    }
    
    dest[i] = src[i];
    if (src[i] == '\0')
      goto out;
  }
  dest[max_len - 1] = '\0';

out:
  arch_vmm_set_pgd(old_pgd);
  arch_tlb_flush_all();
  arch_isb();
  spin_unlock(&current_process->mm_lock);
  local_irq_restore(flagsptr);
  return ret;
}

struct pt_regs *syscall_handler(struct pt_regs *frame) {
  /* Check Exception Syndrome to distinguish SVC from Aborts */
  uint64_t esr = arch_get_fault_status();
  uint64_t ec = (esr >> 26) & 0x3F;

  /* EC 0x15 = SVC from AArch64 */
  if (ec != 0x15) {
    uint64_t far = arch_get_fault_address();
    uint64_t iss = esr & 0x1FFFFFF;

    pr_err(
        "PID %d EXCEPTION: EC=0x%lx ESR=0x%lx FAR=0x%lx ELR=0x%lx ISS=0x%lx\n",
        current_process ? (int)current_process->pid : -1, ec, esr, far,
        frame->elr, iss);

    /* Decode exception class */
    const char *ec_name = "Unknown";
    switch (ec) {
    case 0x00:
      ec_name = "Unknown/Uncategorized";
      break;
    case 0x01:
      ec_name = "WFI/WFE";
      break;
    case 0x20:
      ec_name = "Instruction Abort (Lower EL)";
      break;
    case 0x21:
      ec_name = "Instruction Abort (Same EL)";
      break;
    case 0x24:
      ec_name = "Data Abort (Lower EL)";
      break;
    case 0x25:
      ec_name = "Data Abort (Same EL)";
      break;
    default:
      break;
    }
    pr_err("Exception Class: %s (0x%lx)\n", ec_name, ec);

    if (current_process) {
      pr_err("Terminating PID %d due to fatal exception\n",
             current_process->pid);
      process_terminate(current_process->pid);
      return schedule(frame);
    } else {
      /* Kernel Fault? This function is only for Lower EL Sync though... */
      /* But if VBAR points here for EL1 Sync? No, distinct vectors. */
      /* If we are here, it SHOULD be User Fault. */
      panic("Fatal Exception in Kernel Thread Context");
    }
  }

  /* Dispatch via agnostic core */
  extern struct pt_regs *kernel_syscall_dispatcher(struct pt_regs *regs);
  return kernel_syscall_dispatcher(frame);
}
