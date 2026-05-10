/*
 * kernel/sched/process.c
 * Process Management with Dynamic PID Allocation
 */
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <stdint.h>

/* Process pool - slots can be NULL if process terminated */
struct process *process_pool[MAX_PROCESSES];
static int active_count = 0; /* Number of active processes */
static int next_pid = 1;     /* Global PID counter (never resets) */

/* Global scheduler lock - still used for process_pool and PID allocation */
DEFINE_SPINLOCK(sched_lock);
static int rr_cpu = 0;

/* Keyboard Focus Management */
int keyboard_focus_pid = 7; /* Default to Shell PID */

/* Helper: Add task to runqueue */
/* Internal helper: Add task to runqueue (Caller MUST hold target->sched_lock) */
static void __enqueue_task(struct process *p) {
  int target_cpu = (int)p->on_cpu;
  if (target_cpu < 0)
    target_cpu = 0;

  struct cpu_info *target = &cpu_data[target_cpu];

  /* Skip if already on a runqueue */
  if (p->state == PROC_READY && p->run_list.next != &p->run_list) {
    return;
  }

  p->state = PROC_READY;
  int prio = p->priority;
  if (prio >= MAX_PRIO)
    prio = MAX_PRIO - 1;

  list_add_tail(&p->run_list, &target->runqueues[prio]);
  target->prio_bitmap |= (1 << prio);

  /* Wake up any idling CPUs */
  arch_sev();
}

/* Public API: Add task to runqueue (Safe for external calls) */
void enqueue_task(struct process *p) {
  int target_cpu = (int)p->on_cpu;
  if (target_cpu < 0)
    target_cpu = 0;

  struct cpu_info *target = &cpu_data[target_cpu];

  uint64_t flags;
  spin_lock_irqsave(&target->sched_lock, &flags);
  __enqueue_task(p);
  spin_unlock_irqrestore(&target->sched_lock, flags);
}

/* Helper: Remove task from runqueue (Caller MUST hold target->sched_lock) */
static void __dequeue_task(struct process *p) {
  int target_cpu = (p->on_cpu >= 0) ? p->on_cpu : 0;
  struct cpu_info *target = &cpu_data[target_cpu];

  /* Paranoia check for corruption */
  if (p->run_list.next == NULL || p->run_list.prev == NULL) {
    panic("SCHED: Corrupt run_list for PID %d", p->pid);
  }

  list_del_init(&p->run_list);
  if (p->priority < MAX_PRIO && list_empty(&target->runqueues[p->priority])) {
    target->prio_bitmap &= ~(1 << p->priority);
  }
}

void sleep_on(struct wait_queue_head *wq) {
  struct process *p = current_process;
  uint64_t flags;

  if (!p)
    return;

  spin_lock_irqsave(&wq->lock, &flags);

  p->state = PROC_SLEEPING;
  p->wait_queue_ptr = wq;
  /* Add to wait queue */
  list_add_tail(&p->run_list, &wq->task_list);

  spin_unlock_irqrestore(&wq->lock, flags);
}

void wake_up(struct wait_queue_head *wq) {
  uint64_t flags;
  spin_lock_irqsave(&wq->lock, &flags);

  if (list_empty(&wq->task_list)) {
    spin_unlock_irqrestore(&wq->lock, flags);
    return;
  }

  /* Get the first process in the wait queue */
  struct list_head *tmp = wq->task_list.next;
  struct process *p = list_entry(tmp, struct process, run_list);

  /* Remove from wait queue */
  list_del(&p->run_list);
  p->wait_queue_ptr = NULL;

  /* CRITICAL: Reinitialize run_list after deletion to prevent stale pointers */
  INIT_LIST_HEAD(&p->run_list);

  spin_unlock_irqrestore(&wq->lock, flags);

  /* Pick a target CPU if not yet assigned */
  if (p->on_cpu < 0) {
    uint64_t global_flags;
    spin_lock_irqsave(&sched_lock, &global_flags);
    p->on_cpu = rr_cpu;
    rr_cpu = (rr_cpu + 1) % 4;
    spin_unlock_irqrestore(&sched_lock, global_flags);
  }

  int target_cpu = (int)p->on_cpu;
  struct cpu_info *target = &cpu_data[target_cpu];

  spin_lock_irqsave(&target->sched_lock, &flags);
  __enqueue_task(p);
  spin_unlock_irqrestore(&target->sched_lock, flags);
}

/* Idle Task Entry Point */
void idle_task_entry(void) {
  while (1) {
    /* Wait for interrupt */
    arch_wfi();
    /* When we wake up, check if we need to reschedule?
       The interrupt handler (Timer) will have called schedule() if needed.
       If we are back here, it means no other task was ready.
       So loop again.
    */
  }
}

void process_init(void) {
  /* Initialize global structures if needed */
  pr_info("%s", "Process: Initializing scheduler subsystem...\n");
  for (int i = 0; i < MAX_PROCESSES; i++) {
    process_pool[i] = NULL;
  }

  /* Initialize ALL CPU Runqueues */
  for (int c = 0; c < 8; c++) {
    for (int i = 0; i < MAX_PRIO; i++) {
      INIT_LIST_HEAD(&cpu_data[c].runqueues[i]);
    }
    cpu_data[c].prio_bitmap = 0;
    spin_lock_init(&cpu_data[c].sched_lock);
  }
}

/*
 * Find an empty slot in process pool
 */
static int find_free_slot(void) {
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (process_pool[i] == NULL)
      return i;
  }
  return -1;
}

/*
 * Find process by PID
 */
struct process *process_find_by_pid(int pid) {
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (process_pool[i] && (int)process_pool[i]->pid == pid)
      return process_pool[i];
  }
  return NULL;
}

/*
 * Create a new process
 */
struct process *process_create(const char *name, uint8_t priority,
                               uint32_t permissions) {
  pr_info("Process: Creating '%s' (Prio=%d)\n", name, priority);
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);

  int slot = find_free_slot();
  if (slot < 0) {
    spin_unlock_irqrestore(&sched_lock, flags);
    pr_err("%s", "Process pool full!\n");
    return NULL;
  }

  struct process *proc = (struct process *)pmm_alloc_page();
  if (!proc) {
    spin_unlock_irqrestore(&sched_lock, flags);
    return NULL;
  }

  memset(proc, 0, sizeof(struct process)); // Safe Clear
  strncpy(proc->name, name, 15);
  proc->name[15] = '\0';

  /* Assign unique PID */
  proc->pid = next_pid++;

  /* Priority normalization */
  if (priority >= MAX_PRIO)
    priority = MAX_PRIO - 1;
  proc->priority = priority;

  proc->permissions = permissions;

  /* Init Scheduler Info */
  proc->state = PROC_CREATED;
  proc->first_run = 1; /* ELF loader will initialize context */
  proc->time_slice = DEFAULT_QUANTUM;
  proc->quantum_reset = DEFAULT_QUANTUM;
  proc->on_cpu = -1;
  INIT_LIST_HEAD(&proc->wait_queue.task_list);
  spin_lock_init(&proc->wait_queue.lock);
  proc->ipc_target_pid = -1;
  INIT_LIST_HEAD(&proc->run_list);
  INIT_LIST_HEAD(&proc->msg_queue);
  spin_lock_init(&proc->msg_lock);
  spin_lock_init(&proc->mm_lock);

  /* Add to pool */
  process_pool[slot] = proc;
  active_count++;

  spin_unlock_irqrestore(&sched_lock, flags);

  proc->page_table = vmm_create_pgd();

  pr_info("process_create: '%s' PID=%d slot=%d Prio=%d TTBR0=0x%lx\n", name,
          proc->pid, slot, proc->priority, virt_to_phys(proc->page_table));

  /* Allocate and Setup Kernel Stack (16KB) */
  void *kstack_base = pmm_alloc_pages(STACK_SIZE / 4096);
  if (!kstack_base) {
    /* Cleanup pgd and proc if failed */
    vmm_destroy_pgd(proc->page_table);
    /* Remove from pool since we are failing */
    spin_lock_irqsave(&sched_lock,
                      &flags); // Re-acquire lock to modify shared state
    process_pool[slot] = NULL;
    active_count--;
    spin_unlock_irqrestore(&sched_lock, flags); // Release lock
    pmm_free_page(proc);
    return NULL;
  }
  proc->kernel_stack = (uint64_t)kstack_base + STACK_SIZE;

  /* Initial frame on kernel stack */
  proc->context =
      (struct pt_regs *)(proc->kernel_stack - sizeof(struct pt_regs));
  memset(proc->context, 0, sizeof(struct pt_regs));
  proc->context->spsr = 0x0;

  pr_info("process_create: PID %d context allocated at %p (kstack=%lx)\n",
          proc->pid, (void *)proc->context, proc->kernel_stack);

  proc->on_cpu = -1; /* Not running on any CPU */

  return proc;
}

/*
 * Terminate a process and free its resources
 */
int process_terminate(int pid) {
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);

  struct process *proc = process_find_by_pid(pid);
  if (!proc) {
    spin_unlock_irqrestore(&sched_lock, flags);
    return -1;
  }

  /* Don't allow terminating system-protected processes */
  if (proc->permissions & PROC_PERM_SYSTEM) {
    pr_warn("Cannot terminate protected process '%s' (PID %d)\n", proc->name,
            pid);
    spin_unlock_irqrestore(&sched_lock, flags);
    return -1;
  }

  /* Find the slot for this process */
  int slot = -1;
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (process_pool[i] == proc) {
      slot = i;
      break;
    }
  }

  pr_info("Terminating process '%s' PID=%d\n", proc->name, pid);

  /* Remove from Runqueue OR Waitqueue if it was there */
  if (proc->wait_queue_ptr) {
    struct wait_queue_head *wq = proc->wait_queue_ptr;
    spin_lock(&wq->lock);
    list_del_init(&proc->run_list);
    proc->wait_queue_ptr = NULL;
    spin_unlock(&wq->lock);
  } else if (proc->state == PROC_READY) {
    /* CRITICAL: Acquire target CPU's sched_lock before removing from its runqueue */
    int t_id = (proc->on_cpu >= 0) ? proc->on_cpu : 0;
    struct cpu_info *t_cpu = &cpu_data[t_id];
    
    /* We already hold the global sched_lock, but we must also hold the per-CPU lock 
     * to prevent races with the scheduler on that CPU. */
    spin_lock(&t_cpu->sched_lock);
    __dequeue_task(proc);
    spin_unlock(&t_cpu->sched_lock);
  }

  /* Cleanup resources */
  extern void compositor_destroy_windows_by_pid(int pid);
  compositor_destroy_windows_by_pid(pid);

  /* If we are terminating OURSELVES */
  if (current_process == proc) {
    proc->state = PROC_ZOMBIE;

    /* If this is a user process without a parent or it is a system-level init,
     * we should just mark it for cleanup. For now, we free the slot if it's
     * not PID 1. */
    spin_unlock_irqrestore(&sched_lock, flags);

    /* We cannot free our own stack/pgd while running on it.
     * We just return 0. The caller (sys_exit) MUST call schedule().
     * schedule() will see we are ZOMBIE and switch away.
     * The reaper (process_wait) will free us later.
     */
    return 0;
  }

  proc->state = PROC_DEAD;

  /* Remove from pool */
  process_pool[slot] = NULL;
  active_count--;

  spin_unlock_irqrestore(&sched_lock, flags);

  if (proc->kernel_stack) {
    pmm_free_pages((void *)(proc->kernel_stack - STACK_SIZE), STACK_SIZE / 4096);
  }
  if (proc->page_table) {
    vmm_destroy_pgd(proc->page_table);
  }

  pmm_free_page(proc);

  return 0;
}

/* Assembly helper to jump to user mode */
extern void enter_user_mode(uint64_t entry, uint64_t sp, uint64_t ksp);

/*
 * This is called for the FIRST run of a process.
 */
void start_user_process(struct process *proc) {
  pr_info("Starting process '%s' PID=%d at 0x%lx\n", proc->name, proc->pid,
          proc->user_entry);

  /* We don't enqueue the first process, we jump directly to it */
  current_process = proc;

  uint64_t ttbr0 = virt_to_phys(proc->page_table);
  /* Set page table and flush TLB */
  arch_set_ttbr0(ttbr0);
  arch_tlb_flush_all();

  proc->state = PROC_RUNNING;
  proc->on_cpu = cpu_id();
  enter_user_mode(proc->user_entry, proc->user_stack, proc->kernel_stack);
}

/*
 * Schedule Next Process (O(1) Priority)
 */
struct pt_regs *schedule(struct pt_regs *regs) {
  struct cpu_info *cpu_ptr = get_cpu_info();
  if (!cpu_ptr)
    return regs;

  uint32_t cpu = cpu_ptr->cpu_id;
  struct process *prev = cpu_ptr->current_task;
  uint64_t flags;

  /* Use local lock for runqueue modifications */
  spin_lock_irqsave(&cpu_ptr->sched_lock, &flags);

  /* if (cpu == 0) pr_info("Schedule Core 0\n"); */
  /* Priority Boosting: identify the focused process from the compositor */
  extern int compositor_get_focus_pid(void);
  int focus_pid = compositor_get_focus_pid();

    /* 1. Handle Current Process */
    if (prev) {
      /* Save current context if it was running */
      if (regs) {
        prev->context = regs;
      }
      
      /* Clear first_run flag since it has now been scheduled and preempted/yielded */
      if (prev->first_run) {
        prev->first_run = 0;
      }

      if (prev->state == PROC_RUNNING) {
        prev->time_slice--;

        if (prev->time_slice <= 0) {
          /* Quantum Exhausted */
          prev->time_slice = prev->quantum_reset;
          prev->state = PROC_READY;
        } else {
          /* Preempted or yielded? Mark READY to allow others */
          prev->state = PROC_READY;
        }
      }

      if (prev->state == PROC_READY) {
        prev->on_cpu = -1;
        __enqueue_task(prev);
      } else {
        prev->on_cpu = -1;
      }
    }

  /* 2. Pick Next Process (O(1) Priority-based Selection) */
  struct process *next = NULL;

  if (focus_pid > 0) {
    for (int p = 0; p < MAX_PRIO; p++) {
      if (list_empty(&cpu_ptr->runqueues[p]))
        continue;
      struct process *it;
      list_for_each_entry(it, &cpu_ptr->runqueues[p], run_list) {
        if ((int)it->pid == focus_pid) {
          next = it;
          __dequeue_task(next);
          goto found;
        }
      }
    }
  }

  if (cpu_ptr->prio_bitmap != 0) {
    /* Find highest priority non-empty queue */
    int best_prio = __builtin_ctz(cpu_ptr->prio_bitmap);

    if (best_prio < MAX_PRIO && !list_empty(&cpu_ptr->runqueues[best_prio])) {
      struct list_head *entry = cpu_ptr->runqueues[best_prio].next;
      next = container_of(entry, struct process, run_list);
      __dequeue_task(next);
    }
  }

found:

  if (!next) {
    /* Work stealing from other CPUs */
    for (uint32_t i = 0; i < 4; i++) {
      if (i == cpu_ptr->cpu_id)
        continue; /* Skip self */

      struct cpu_info *other_cpu = &cpu_data[i];
      if (!other_cpu->online)
        continue;

      /* Try to lock other CPU's runqueue */
      /* Use trylock to avoid deadlock potential */
      if (spin_trylock(&other_cpu->sched_lock)) {
        if (other_cpu->prio_bitmap != 0) {
          int p = __builtin_ctz(other_cpu->prio_bitmap);
          if (!list_empty(&other_cpu->runqueues[p])) {
            struct list_head *entry = other_cpu->runqueues[p].next;
            next = container_of(entry, struct process, run_list);

            /* Only steal if not currently running on that CPU (redundant
             * check if dequeued?) */
            /* Actually dequeueing it makes it ours. */
            /* But if it WAS running, it wouldn't be in runqueue (unless we
             * re-queue running tasks?) */
            /* The logic removes running task from queue usually? */
            /* Checking `dequeue_task`: it uses `msg_queue`? No. */
            /* We assume tasks in runqueue are READY and not RUNNING. */

            /* Remove from other CPU */
            list_del_init(&next->run_list);
            if (list_empty(&other_cpu->runqueues[p])) {
              other_cpu->prio_bitmap &= ~(1 << p);
            }

            /* Add to our local queue (or just run it directly) */
            /* Running directly: set state, context switch. */
            /* `next` is now found. We own it. */
            spin_unlock(&other_cpu->sched_lock);
            goto found;
          }
        }
        spin_unlock(&other_cpu->sched_lock);
      }
    }
    /* If current task is still READY, just keep running it */
    if (prev && prev->state == PROC_RUNNING) {
      spin_unlock_irqrestore(&cpu_ptr->sched_lock, flags);
      return regs;
    }

    /* Otherwise, mandatory fallback to IDLE task */
    next = cpu_ptr->idle_task;
    if (!next) {
      /* Absolute fallback if idle task not yet set */
      spin_unlock_irqrestore(&cpu_ptr->sched_lock, flags);
      return regs;
    }
  }

  /* 3. Context Switch Logic */
  if (prev == next) {
    next->state = PROC_RUNNING;
    next->on_cpu = cpu;
    spin_unlock_irqrestore(&cpu_ptr->sched_lock, flags);
    return regs;
  }

  cpu_ptr->current_task = next;
  next->state = PROC_RUNNING;
  next->on_cpu = cpu;

  /* Update Page Table (Hardware Context Switch) */
  if (next == NULL) {
    panic("SCHED: Reschedule failed, next is NULL");
  }
  if (next->context == NULL) {
    panic("SCHED: Invalid context for PID %d", next->pid);
  }
  if (next->context->elr == 0) {
    /* Allow elr=0 if it's a kernel thread? No, kernel threads have function ptr
     */
    panic("SCHED: ELR is 0 for PID %d (Name: %s)", next->pid, next->name);
  }

  if (!prev || prev->page_table != next->page_table) {
    uint64_t next_ttbr0 = virt_to_phys(next->page_table);
    if (next_ttbr0 != 0) {
      arch_set_ttbr0(next_ttbr0);
      arch_tlb_flush_all();
      arch_isb();
    }
  }

  spin_unlock_irqrestore(&cpu_ptr->sched_lock, flags);
  return next->context;
}

/*
 * Wait for a process (non-blocking)
 * Returns PID if terminated, -1 if still running, -2 if not found
 */
int process_wait(int pid) {
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *proc = process_pool[i];
    if (proc && (int)proc->pid == pid) {
      if (proc->state == PROC_DEAD || proc->state == PROC_ZOMBIE) {

        if (proc->state == PROC_ZOMBIE) {
          /* Now we can safely free the zombie's resources */
          pr_info("Reaping zombie process %d\n", pid);
          if (proc->kernel_stack) {
            pmm_free_pages((void *)(proc->kernel_stack - STACK_SIZE),
                           STACK_SIZE / 4096);
          }
          if (proc->page_table) {
            vmm_destroy_pgd(proc->page_table);
          }
          pmm_free_page(proc);
        }

        /* Resource cleanup should have happened */
        process_pool[i] = NULL;
        active_count--;
        spin_unlock_irqrestore(&sched_lock, flags);
        return pid;
      }
      spin_unlock_irqrestore(&sched_lock, flags);
      return -1; /* Still alive */
    }
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return -2; /* Not found */
}
/*
 * IPC Helper: Pop message matching src_pid (or -1 for any)
 */
struct ipc_node *pop_message(struct process *proc, int src_pid) {
  /* pr_err("pop_message: proc=%d src=%d\n", proc->pid, src_pid); */
  uint64_t flags;
  struct ipc_node *node = NULL;
  struct list_head *pos, *q;

  spin_lock_irqsave(&proc->msg_lock, &flags);
  list_for_each_safe(pos, q, &proc->msg_queue) {
    struct ipc_node *tmp = list_entry(pos, struct ipc_node, list);
    if (src_pid == -1 || tmp->msg.from == (int)src_pid) {
      list_del(pos);
      node = tmp;
      break;
    }
  }
  spin_unlock_irqrestore(&proc->msg_lock, flags);
  return node;
}

/*
 * IPC Implementation (Internal)
 */
int kernel_ipc_send(int target_pid, struct ipc_message *msg) {
  /* pr_err("kernel_ipc_send: target=%d type=%d\n", target_pid, msg->type); */
  struct process *target = process_find_by_pid(target_pid);
  if (!target)
    return -1;

  struct ipc_node *node = (struct ipc_node *)kmalloc(sizeof(struct ipc_node));
  if (!node)
    return -1;

  memcpy(&node->msg, msg, sizeof(struct ipc_message));

  uint64_t flags;
  spin_lock_irqsave(&target->msg_lock, &flags);
  list_add_tail(&node->list, &target->msg_queue);

  /* Check if target is waiting for THIS sender or ANY sender */
  if (target->state == PROC_SLEEPING &&
      (target->ipc_target_pid == -1 ||
       target->ipc_target_pid == (int)msg->from)) {

    spin_unlock_irqrestore(&target->msg_lock, flags);

    /* Wake target: use its local sched_lock */
    int t_id = (target->on_cpu >= 0) ? target->on_cpu : 0;
    struct cpu_info *target_cpu = &cpu_data[t_id];
    spin_lock_irqsave(&target_cpu->sched_lock, &flags);
    if (target->state == PROC_SLEEPING) {
      target->state = PROC_READY;
      __enqueue_task(target);
    }
    spin_unlock_irqrestore(&target_cpu->sched_lock, flags);
    return 0;
  }

  spin_unlock_irqrestore(&target->msg_lock, flags);
  return 0;
}

int sys_ipc_send(int target_pid, void *msg_ptr) {
  struct ipc_message k_msg;
  extern int copy_from_user(void *dest, const void *src, size_t n);
  if (copy_from_user(&k_msg, msg_ptr, sizeof(struct ipc_message)) != 0) {
    return -1;
  }
  k_msg.from = current_process->pid;
  return kernel_ipc_send(target_pid, &k_msg);
}

int sys_ipc_recv(int src_pid, void *msg_ptr) {
  /* 1. Try to pop an existing message */
  struct ipc_node *node = pop_message(current_process, src_pid);
  if (node) {
    extern int copy_to_user(void *dest, const void *src, size_t n);
    if (copy_to_user(msg_ptr, &node->msg, sizeof(struct ipc_message)) != 0) {
      /* Drop node and return error */
      kfree(node);
      return -1;
    }
    kfree(node);
    return 0;
  }

  /* 2. No message ready, block */
  uint64_t flags;
  struct cpu_info *cpu = get_cpu_info();

  spin_lock_irqsave(&cpu->sched_lock, &flags);
  current_process->ipc_target_pid = src_pid;
  current_process->state = PROC_SLEEPING;
  spin_unlock_irqrestore(&cpu->sched_lock, flags);

  /* Set ELR back 4 bytes to re-execute the SVC instruction on wake-up */
  current_process->context->elr -= 4;

  return 0;
}

int sys_ipc_try_recv(int src_pid, void *msg_ptr) {
  struct ipc_node *node = pop_message(current_process, src_pid);
  if (node) {
    extern int copy_to_user(void *dest, const void *src, size_t n);
    if (copy_to_user(msg_ptr, &node->msg, sizeof(struct ipc_message)) != 0) {
      kfree(node);
      return -1;
    }
    kfree(node);
    return 0;
  }
  return -1; /* EAGAIN */
}

long sys_getprocs(struct ps_info *user_buf, size_t max_count) {
  if (!user_buf)
    return -1;

  struct ps_info *k_buf =
      (struct ps_info *)kmalloc(sizeof(struct ps_info) * max_count);
  if (!k_buf)
    return -1;

  int count = 0;
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  for (int i = 0; i < MAX_PROCESSES && (size_t)count < max_count; i++) {
    if (process_pool[i]) {
      k_buf[count].pid = process_pool[i]->pid;
      strncpy(k_buf[count].name, process_pool[i]->name, 32);
      k_buf[count].state = process_pool[i]->state;
      k_buf[count].priority = process_pool[i]->priority;
      k_buf[count].cpu_time = 0; /* Placeholder */
      k_buf[count].on_cpu = process_pool[i]->on_cpu;
      count++;
    }
  }
  spin_unlock_irqrestore(&sched_lock, flags);

  extern int copy_to_user(void *dest, const void *src, size_t n);
  copy_to_user(user_buf, k_buf, sizeof(struct ps_info) * count);
  kfree(k_buf);
  return count;
}
