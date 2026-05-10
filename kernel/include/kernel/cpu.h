#ifndef _KERNEL_CPU_H
#define _KERNEL_CPU_H

#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

/* Forward declaraton */
struct process;

/* Per-CPU information structure */
struct cpu_info {
  uint32_t cpu_id;
  uint32_t online;
  uint64_t stack_top;
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

/* API */
extern struct cpu_info cpu_data[8];
uint32_t cpu_id(void);
struct cpu_info *get_cpu_info(void);
void cpu_init(void);

/* Interrupt/Exception handling */
void local_irq_enable(void);
void local_irq_disable(void);
uint64_t local_irq_save(void);
void local_irq_restore(uint64_t flags);
struct pt_regs; /* forward decl */
struct pt_regs *serror_handler(struct pt_regs *frame);

#endif /* _KERNEL_CPU_H */
