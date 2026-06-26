/*
 * kernel/core/fault.c
 * Generic user-vs-kernel fault decision (Phase A step 8).
 *
 * Both HAL exception dispatchers route recoverable-fault classification
 * through fault_handle_user_or_panic():
 *
 *   - fault from user mode (CS RPL==3 on amd64, SPSR.M==EL0t on aarch64), or
 *   - fault from kernel mode while arch_copy_{from,to}_user was dereferencing
 *     user memory (per-CPU uaccess_active set, fault address in user VA)
 *
 *   -> log, terminate the process, return schedule()'s next frame.
 *
 * Everything else is a kernel bug: the function returns NULL and the caller
 * performs its arch-specific register dump and panics (panic() is fault-safe
 * here because the dispatcher's fault_enter() depth is still held).
 *
 * The uaccess_active gate is the CPU-AARCH64-01 fix: a wild kernel pointer
 * that merely happens to land in user VA range is NOT recoverable and must
 * panic; only a fault raised by the explicit user-copy window may terminate
 * the process and continue.
 */
#include <kernel/cpu.h>
#include <kernel/fault.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/types.h>
#include <kernel/vmm.h>

extern struct pt_regs *schedule(struct pt_regs *regs);
extern int process_terminate(int pid);

struct pt_regs *fault_handle_user_or_panic(struct pt_regs *regs, int user_mode,
                                           uint64_t fault_addr, uint64_t fault_pc,
                                           const char *desc, uint64_t syndrome) {
  /* Fault context: locate per-CPU state via the MSR/MPIDR path, never via
   * the LAPIC-MMIO read (kernel/fault.h). */
  struct cpu_info *ci = arch_cpu_info_fault_safe();
  struct process *task = ci ? ci->current_task : NULL;

  int uaccess = (!user_mode && ci && ci->uaccess_active &&
                 vmm_is_user_addr(fault_addr));

  if ((user_mode || uaccess) && task) {
    /* User-attributable fault: the kernel itself is healthy.  fault_printf
     * is still used (not printk) because on the uaccess path we may hold
     * arch-side locks until the fixup below. */
    fault_printf("\n[FAULT] %s%s: PID %d (%s) pc=0x%016lx addr=0x%016lx cause=0x%016lx — terminating\n",
                 desc, uaccess ? " (uaccess)" : "", task->pid, task->name,
                 fault_pc, fault_addr, syndrome);

    if (uaccess) {
      /* Release the arch uaccess critical section (locks/IRQ state/flag);
       * the address-space switch is reconciled by the scheduler below. */
      arch_uaccess_fault_fixup();
    }

    process_terminate(task->pid);
    fault_exit(); /* fault handled — unwind the recursion guard */
    return schedule(regs);
  }

  return NULL; /* kernel fault: caller dumps registers and panics */
}
