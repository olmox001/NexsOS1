#ifndef _KERNEL_CPU_H
#define _KERNEL_CPU_H

#ifndef __ASSEMBLER__
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

/* Forward declaraton */
struct process;

/* Per-CPU information structure */
struct cpu_info {
  struct cpu_info *self; /* Must be at offset 0 for %gs:0 access on x86_64 */
  uint32_t cpu_id;
  uint32_t online;
  uint64_t stack_top;      /* Kernel Stack Top */
  uint64_t user_stack_tmp; /* Temp storage for user RSP during syscall/interrupt */
  struct process *current_task;
  uint64_t next_tick_target;
  uint64_t tick_error_acc;
  uint64_t tick_count;
  /* Raw hardware-counter value at which the current task last started running on
   * this CPU; schedule() charges (arch_timer_get_count() - sched_run_count) to
   * the outgoing task's cpu_time_counts (Tier 3 per-process CPU time, converted
   * to ns only on read — docs/TIMER-MODEL.md §4). */
  uint64_t sched_run_count;

  /* Scheduler Local Data (Multicore Optimization) */
  struct list_head runqueues[32];
  uint32_t prio_bitmap;
  spinlock_t sched_lock; /* Local runqueue protection */
  struct process *idle_task;

  /* Per-CPU software timer wheel (kernel/core/timer.c): each CPU fires its OWN
   * timers in kernel_timer_tick against the global jiffies clock, so a process
   * sleeping on this core is woken locally — no cross-CPU dependency, no global
   * timer-lock contention. */
  struct list_head timer_list;
  spinlock_t timer_lock;

  char printk_buf[2048];
  char syscall_buf[2048];
  uint32_t in_printk;
  uint32_t printk_dropped;

  /* Fault recursion depth (Phase A, kernel/fault.h).  Incremented by
   * fault_enter() at exception-dispatch entry; depth > 1 means a fault
   * occurred inside a fault handler — the dispatcher emits one raw line via
   * fault_printf and halts instead of recursing to a triple fault. */
  uint32_t in_fault;

  /* Set while arch_copy_{from,to}_user is dereferencing user memory.  The
   * fault classifier treats a kernel-mode fault on a user VA as a recoverable
   * uaccess fault ONLY when this is set (CPU-AARCH64-01: a wild kernel
   * pointer that merely lands in user VA range must panic, not silently
   * terminate the current process). */
  uint32_t uaccess_active;

  /* Reap stack head (SCHED-UAF-01): processes terminated by process_terminate()
   * awaiting deferred destruction, chained via the legacy process.next field;
   * drained at the top of the next schedule() on this CPU, after we have
   * switched away from them. */
  struct process *deferred_free_proc;
};
#endif

#define MAX_CPUS 64

#ifndef __ASSEMBLER__
/* API */
extern struct cpu_info cpu_data[MAX_CPUS];
struct cpu_info *get_cpu_info(void);
void smp_create_idle_task(uint32_t cpu_id);

/* These are now provided by arch.h HAL macros/functions */
#include <kernel/hal_unified.h>
#include <kernel/arch.h>

#define cpu_id() hal_cpu_id()
#define cpu_init() arch_cpu_init()
#define local_irq_enable() hal_irq_enable()
#define local_irq_disable() hal_irq_disable()
#define local_irq_save() hal_irq_save_val()
#define local_irq_restore(flags) hal_irq_restore(flags)
struct pt_regs; /* forward decl */
struct pt_regs *serror_handler(struct pt_regs *frame);
/* aarch64 coverage handlers (exception.S, Phase A step 13) */
struct pt_regs *fiq_handler(struct pt_regs *frame);
struct pt_regs *aarch32_handler(struct pt_regs *frame);
#endif

#endif /* _KERNEL_CPU_H */
