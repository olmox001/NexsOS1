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
  uint32_t cpu_id;
  uint32_t online;
  uint64_t stack_top;      /* Kernel Stack Top */
  uint64_t user_stack_tmp; /* Temp storage for user RSP during syscall/interrupt */
  struct process *current_task;
  uint64_t next_tick_target;
  uint64_t tick_error_acc;
  uint64_t tick_count;

  /* Scheduler Local Data (Multicore Optimization) */
  struct list_head runqueues[32];
  uint32_t prio_bitmap;
  spinlock_t sched_lock; /* Local runqueue protection */
  struct process *idle_task;
  char printk_buf[2048];
  char syscall_buf[2048];
  uint32_t in_printk;

  /* Deferred process free: freed on next schedule() call after context switch */
  struct process *deferred_free_proc;
};
#endif

#define MAX_CPUS 16

#ifndef __ASSEMBLER__
/* API */
extern struct cpu_info cpu_data[MAX_CPUS];
struct cpu_info *get_cpu_info(void);

/* These are now provided by arch.h HAL macros/functions */
#include <kernel/arch.h>

#define cpu_id() arch_get_cpu_id()
#define cpu_init() arch_cpu_init()
#define local_irq_enable() arch_local_irq_enable()
#define local_irq_disable() arch_local_irq_disable()
#define local_irq_save() arch_local_irq_save_val()
#define local_irq_restore(flags) arch_local_irq_restore(flags)
struct pt_regs; /* forward decl */
struct pt_regs *serror_handler(struct pt_regs *frame);
#endif

#endif /* _KERNEL_CPU_H */
