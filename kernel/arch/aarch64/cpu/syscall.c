/*
 * kernel/arch/aarch64/cpu/syscall.c
 * System Call Handler
 */
#include <drivers/uart.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/ext4.h>
#include <kernel/fault.h>
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

  /* Perform copy while user space is mapped at TTBR0.  uaccess_active flags
   * the dereference window for the fault classifier (kernel/core/fault.c):
   * an abort here is recoverable (terminate the process via
   * arch_uaccess_fault_fixup), anywhere else it is a kernel bug. */
  get_cpu_info()->uaccess_active = 1;
  memcpy(dest, src, n);
  get_cpu_info()->uaccess_active = 0;

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

  /* uaccess window — see arch_copy_from_user */
  get_cpu_info()->uaccess_active = 1;
  memcpy(dest, src, n);
  get_cpu_info()->uaccess_active = 0;

  arch_vmm_set_pgd(old_pgd);
  arch_tlb_flush_all();
  arch_isb();

  spin_unlock(&current_process->mm_lock);
  local_irq_restore(flagsptr);

  return 0;
}

/*
 * arch_uaccess_fault_fixup - release the uaccess critical section after an
 * abort inside one of the copy windows above (kernel/core/fault.c).
 *
 * The copies hold: IRQs masked (local_irq_save), current_process->mm_lock,
 * TTBR0 = the user PGD, and uaccess_active = 1.  The faulting context is
 * being discarded (process terminated), so: clear the flag, drop mm_lock,
 * re-enable IRQs (SYS-AARCH64-02 made explicit in ONE place).  TTBR0 is NOT
 * restored here — the scheduler loads the next task's PGD unconditionally on
 * the switch (SCHED-UAF-01 fix).
 */
void arch_uaccess_fault_fixup(void) {
  struct cpu_info *ci = arch_cpu_info_fault_safe();
  if (ci) {
    ci->uaccess_active = 0;
    if (ci->current_task)
      spin_unlock(&ci->current_task->mm_lock);
  }
  local_irq_enable();
}

/* Copy null-terminated string from user space safely with Page Table Switching
 */
/*
 * arch_copy_string_from_user_n - HAL uaccess provider (kernel/hal_uaccess.h).
 *
 * Reports what the copy loop already knows and used to discard: how much was
 * copied, and whether the source was LONGER than the destination.  Without
 * that, a truncated string is indistinguishable from a complete one and every
 * caller proceeds with a value the process never passed.
 */
int arch_copy_string_from_user_n(char *dest, const char *src, size_t max_len,
                                 size_t *out_len, int *out_truncated) {
  if (out_len)
    *out_len = 0;
  if (out_truncated)
    *out_truncated = 0;
  if (!vmm_is_user_addr((uint64_t)src))
    return -1;

  if (!current_process || !current_process->page_table)
    return -1;
  /* max_len 0 would underflow `max_len - 1` below and run away; a zero-sized
   * destination cannot hold even the terminator, so refuse it outright. */
  if (max_len == 0)
    return -1;

  uint64_t flagsptr = local_irq_save();
  spin_lock(&current_process->mm_lock);

  uint64_t old_pgd = arch_vmm_get_pgd();
  arch_vmm_set_pgd(virt_to_phys(current_process->page_table));
  arch_tlb_flush_all();
  arch_isb();

  /* uaccess window — see arch_copy_from_user */
  get_cpu_info()->uaccess_active = 1;

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
  /* Ran out of destination before the terminator: the source is longer. */
  dest[max_len - 1] = '\0';
  if (out_truncated)
    *out_truncated = 1;

out:
  if (out_len)
    *out_len = i;
  get_cpu_info()->uaccess_active = 0;
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

    /* Recursion guard (Phase A step 7): a fault while handling this EL0
     * fault arrives as an EL1 sync abort, but guard this path too so e.g. a
     * fault inside process_terminate stops cleanly. */
    if (fault_enter() > 1) {
      fault_printf("\n[FATAL] NESTED EXCEPTION in EL0-fault path EC=0x%lx ELR=%016lx — halting\n",
                   ec, frame->elr);
      arch_cpu_halt();
    }

    /* Decode the abort class → the human-readable desc passed to the common
     * fault handler (which logs the ONE unified "[FAULT] <class>: PID … pc …
     * addr … cause=0x<ESR>" line — the user-fault debug, now IDENTICAL across
     * arches; the ESR/EC/ISS detail rides in cause=0x<ESR>, FAR in addr, ELR in
     * pc).  The raw register dump is reserved for the kernel/corruption path
     * below, mirroring amd64 (DIR-06 HAL conformance: one reporting path). */
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

    /* This vector (el0_64_sync) only fires for EL0-origin exceptions: always
     * user-attributable.  A NULL return = no current task on this vector =
     * scheduler-state corruption: dump the raw syndrome and panic. */
    {
      struct pt_regs *next =
          fault_handle_user_or_panic(frame, 1, far, frame->elr, ec_name, esr);
      if (next)
        return next;
      pr_err(
          "PID %d EXCEPTION: EC=0x%lx ESR=0x%lx FAR=0x%lx ELR=0x%lx ISS=0x%lx\n",
          current_process ? (int)current_process->pid : -1, ec, esr, far,
          frame->elr, iss);
      panic("Fatal EL0 exception with no current task (EC=0x%lx)", ec);
    }
  }

  /* Dispatch via agnostic core */
  extern struct pt_regs *kernel_syscall_dispatcher(struct pt_regs *regs);
  return kernel_syscall_dispatcher(frame);
}

/* Legacy tolerant spelling — truncation is success.  See kernel/hal_uaccess.h
 * for why both exist and which call sites must use the strict form instead. */
int arch_copy_string_from_user(char *dest, const char *src, size_t max_len) {
  return arch_copy_string_from_user_n(dest, src, max_len, (size_t *)0,
                                      (int *)0);
}
