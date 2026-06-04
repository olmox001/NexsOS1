/*
 * kernel/sched/process.c
 * Process Management, Scheduler, and IPC
 *
 * This file owns the core process model and the OS1/NEXS scheduler:
 *   - A fixed pool of MAX_PROCESSES (64) process descriptors allocated from
 *     the PMM one page each.
 *   - Per-CPU O(1) priority-bitmap runqueues (MAX_PRIO levels) with
 *     work-stealing between CPUs using trylock to avoid AB-BA deadlocks.
 *   - Deferred-free: a process terminated while running on another CPU is
 *     marked PROC_DEAD and freed on the *next* schedule() call on that CPU,
 *     after the kernel stack is no longer in use.
 *   - A kmalloc-backed linked-list IPC per process (msg_queue), with
 *     sleeping-receiver wakeup on send.
 *   - sys_sbrk: demand-mapped user heap extending upward from the top of the
 *     ELF segments, with no upper-bound check against the user stack.
 *
 * Locking hierarchy (must be acquired in this order):
 *   sched_lock (global) -> target->msg_lock -> target_cpu->sched_lock
 *
 * Key invariants:
 *   - current_process is a per-CPU variable (accessed via get_cpu_info());
 *     safe to read/write without a lock during a syscall or IRQ on that CPU.
 *   - The idle task for each CPU is created by smp_create_idle_task(); its
 *     page_table is NULL and it is never enqueued or stolen by work-stealing.
 *   - process_pool[] slot is set to NULL only after the process struct and
 *     kernel stack are freed (or deferred via cpu_ptr->deferred_free_proc).
 *   - PIDs are assigned from next_pid (monotonically increasing, never reused).
 *
 * Known issues:
 *   SCHED-01  (W3 WRONG-DESIGN) schedule() calls compositor_get_focus_pid()
 *             and gives the focused window's process priority access to the
 *             runqueue — the kernel scheduler depends on the graphics
 *             compositor, inverting the correct dependency.
 *   SCHED-02  (W2 BAD-IMPL) schedule() is large and intricate; many pc==0
 *             panic guards betray past context-corruption bugs.
 *   SCHED-03  (W2 WRONG-DESIGN) process_wait() is non-blocking (returns -1
 *             while target is alive); zombies are only reaped via process_wait,
 *             so unwaited children permanently leak process pool slots.
 *   SCHED-04  (W2 BUG/DOC) Comment in process_create says "Kernel Stack (16KB)"
 *             but STACK_SIZE is 128KB.
 *   SCHED-05  (W3 BUG/SECURITY) kernel_ipc_send() nests sched_lock -> msg_lock
 *             -> cpu->sched_lock — an AB-BA deadlock risk acknowledged in code
 *             comments.  Additionally, msg_queue is unbounded: a sender can OOM
 *             a receiver with no flow control.
 *   SCHED-06  (W2 WRONG-DESIGN) No parent/child relationship; process_wait()
 *             accepts any PID; no process groups or sessions.
 *   SCHED-07  (W2 BUG) sys_sbrk() has no upper-bound check; heap can collide
 *             with the fixed user stack at 0xC0000000.
 *   SCHED-08  (W1 PERF) process_create() zeros the PMM page with memset()
 *             even though pmm_alloc_page() already zeroes it.
 *   IPC-01    (W3 BUG) Lost-wakeup race: sender wakes target only if it reads
 *             PROC_SLEEPING; a receiver that has checked the queue but not yet
 *             set SLEEPING can sleep indefinitely with a message waiting.
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
/* process_pool[]: fixed-size table of active process descriptors.
 * A NULL slot means it is free.  Protected by sched_lock for modifications;
 * individual slots are also read locklessly by schedule() on the owning CPU. */
struct process *process_pool[MAX_PROCESSES];
static int active_count = 0; /* Number of active processes */
static int next_pid = 1;     /* Global PID counter (never resets) */

/* Global scheduler lock - still used for process_pool and PID allocation */
/* sched_lock: global spinlock protecting process_pool[], active_count,
 * next_pid, rr_cpu, and the outer section of kernel_ipc_send().
 * Inner locks (per-CPU sched_lock, per-process msg_lock) may be taken while
 * holding sched_lock — see locking hierarchy in the file header.
 * NOTE(SCHED-05): Taking cpu->sched_lock while holding both sched_lock and
 * msg_lock creates the full AB-BA chain. */
DEFINE_SPINLOCK(sched_lock);
/* rr_cpu: round-robin CPU index for assigning a CPU to newly woken tasks.
 * Protected by sched_lock. */
static int rr_cpu = 0;

/* Keyboard Focus Management */
/* keyboard_focus_pid: PID of the process that currently holds keyboard focus.
 * Written by syscall 232 (SET_FOCUS) from userland; read by schedule() every
 * tick to bias runqueue selection.
 * NOTE(SCHED-01): This global couples the scheduler directly to the graphics
 * compositor; see SCHED-01 for the correct inversion direction. */
int keyboard_focus_pid = 7; /* Default to Shell PID */

/*
 * __enqueue_task - add a process to its assigned CPU's priority runqueue.
 *
 * Caller MUST hold target_cpu->sched_lock.  Sets p->state = PROC_READY,
 * appends p to the tail of runqueues[prio], and sets the prio_bitmap bit.
 * Calls hal_cpu_notify() to wake any idling CPUs.
 *
 * If p is already on a runqueue (run_list.next != &p->run_list) and in
 * PROC_READY, returns immediately (idempotent guard).
 *
 * Locking: caller must hold target_cpu->sched_lock (irqsave).
 */
/* Helper: Add task to runqueue */
/* Internal helper: Add task to runqueue (Caller MUST hold target->sched_lock) */
static void __enqueue_task(struct process *p) {
  int target_cpu_id = (int)p->on_cpu;
  if (target_cpu_id < 0)
    target_cpu_id = 0;

  struct cpu_info *target_cpu = &cpu_data[target_cpu_id];

  /* Skip if already on a runqueue */
  if (p->state == PROC_READY && p->run_list.next != &p->run_list) {
    return;
  }

  p->state = PROC_READY;
  p->on_cpu = target_cpu_id; /* Track which CPU's runqueue we are on */
  int prio = p->priority;
  if (prio >= MAX_PRIO)
    prio = MAX_PRIO - 1;

  list_add_tail(&p->run_list, &target_cpu->runqueues[prio]);
  target_cpu->prio_bitmap |= (1 << prio);

  /* Wake up any idling CPUs */
  hal_cpu_notify();
}

/*
 * enqueue_task - public wrapper: lock the target CPU's runqueue and enqueue p.
 *
 * Acquires target_cpu->sched_lock (irqsave), calls __enqueue_task(), releases.
 * Safe to call from process creation (before SMP) or from any context.
 *
 * Locking: acquires/releases target_cpu->sched_lock internally.
 * IRQ context: safe (irqsave).
 */
void enqueue_task(struct process *p) {
  uint64_t flags;
  int target_cpu_id = (int)p->on_cpu;
  if (target_cpu_id < 0)
    target_cpu_id = 0;

  struct cpu_info *target_cpu = &cpu_data[target_cpu_id];
  spin_lock_irqsave(&target_cpu->sched_lock, &flags);
  __enqueue_task(p);
  spin_unlock_irqrestore(&target_cpu->sched_lock, flags);
}

/*
 * __dequeue_task - remove a process from its CPU's priority runqueue.
 *
 * Caller MUST hold the target CPU's sched_lock.  Calls list_del_init() on
 * p->run_list and clears the prio_bitmap bit if the queue becomes empty.
 * Panics on a NULL run_list pointer (corruption guard, see SCHED-02).
 *
 * Locking: caller must hold target_cpu->sched_lock.
 */
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

/*
 * sleep_on - put the current process to sleep on a wait queue.
 *
 * Sets current_process->state = PROC_SLEEPING, stores a back-pointer to wq,
 * and adds the process to wq->task_list.  The caller is responsible for
 * calling schedule() afterward to actually yield the CPU.
 *
 * Locking: acquires wq->lock (irqsave) to protect the task_list modification.
 * IRQ context: safe (irqsave).
 */
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

/*
 * wake_up - wake the first process sleeping on a wait queue.
 *
 * Removes the head of wq->task_list, reinitialises its run_list (to clear
 * any stale prev/next pointers), assigns a CPU via round-robin if p->on_cpu
 * is -1, then acquires the target CPU's sched_lock and calls __enqueue_task().
 *
 * Locking: acquires wq->lock (irqsave), then releases it before acquiring
 *          sched_lock (irqsave) — lock-order: wq->lock released before
 *          cpu->sched_lock is taken, so no inversion with __enqueue_task.
 * IRQ context: safe (irqsave).
 */
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
    rr_cpu = (rr_cpu + 1) % MAX_CPUS;
    spin_unlock_irqrestore(&sched_lock, global_flags);
  }

  int target_cpu = (int)p->on_cpu;
  struct cpu_info *target = &cpu_data[target_cpu];

  spin_lock_irqsave(&target->sched_lock, &flags);
  __enqueue_task(p);
  spin_unlock_irqrestore(&target->sched_lock, flags);
}

/*
 * idle_task_entry - idle task body for each CPU.
 *
 * Runs when no other task is runnable on this CPU.  Calls hal_cpu_idle()
 * (typically a WFI/HLT instruction) to halt until the next interrupt.  The
 * timer IRQ will call schedule(), which will either resume this idle loop or
 * switch to a runnable task.
 *
 * This function never returns.  It must not call schedule() itself; the
 * timer IRQ handler is the only scheduler entry point from idle.
 *
 * IRQ context: no — this is a regular kernel thread.
 */
/* Idle Task Entry Point */
void idle_task_entry(void) {
  while (1) {
    /* Wait for interrupt */
    hal_cpu_idle();
    /* When we wake up, check if we need to reschedule?
       The interrupt handler (Timer) will have called schedule() if needed.
       If we are back here, it means no other task was ready.
       So loop again.
    */
  }
}

/*
 * process_init - initialise the process pool and all per-CPU runqueues.
 *
 * Called once from the boot path (CPU 0, single-threaded) before any process
 * is created.  Zeroes process_pool[], initialises all runqueue list heads and
 * prio_bitmaps, and initialises all per-CPU sched_locks.
 *
 * Locking: none — must be called before SMP secondary CPUs start.
 * IRQ context: no.
 */
void process_init(void) {
  /* Initialize global structures if needed */
  pr_info("%s", "Process: Initializing scheduler subsystem...\n");
  for (int i = 0; i < MAX_PROCESSES; i++) {
    process_pool[i] = NULL;
  }

  /* Initialize ALL CPU Runqueues */
  for (int c = 0; c < MAX_CPUS; c++) {
    for (int i = 0; i < MAX_PRIO; i++) {
      INIT_LIST_HEAD(&cpu_data[c].runqueues[i]);
    }
    cpu_data[c].prio_bitmap = 0;
    spin_lock_init(&cpu_data[c].sched_lock);
  }
}

/*
 * find_free_slot - find the first NULL slot in process_pool[].
 *
 * Caller MUST hold sched_lock.  Returns slot index [0, MAX_PROCESSES) or -1
 * if the pool is full.  Linear scan; O(MAX_PROCESSES).
 */
static int find_free_slot(void) {
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (process_pool[i] == NULL)
      return i;
  }
  return -1;
}

/*
 * __process_find_by_pid - find a process by PID without locking (internal).
 *
 * Caller MUST hold sched_lock to prevent concurrent pool modification.
 * Returns the matching process or NULL.  O(MAX_PROCESSES) linear scan.
 */
struct process *__process_find_by_pid(int pid) {
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (process_pool[i] && (int)process_pool[i]->pid == pid)
      return process_pool[i];
  }
  return NULL;
}

/*
 * process_find_by_pid - find a process by PID with locking (external).
 *
 * Acquires sched_lock (irqsave), calls __process_find_by_pid(), releases.
 * Returns the matching process or NULL.  The returned pointer is only valid
 * as long as the caller can guarantee the process is not terminated.
 *
 * Locking: acquires/releases sched_lock internally.
 */
struct process *process_find_by_pid(int pid) {
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  struct process *proc = __process_find_by_pid(pid);
  spin_unlock_irqrestore(&sched_lock, flags);
  return proc;
}

/*
 * process_create - allocate and initialise a new process descriptor.
 *
 * Allocates a single PMM page for the struct process, assigns a PID from
 * next_pid, allocates STACK_SIZE bytes for the kernel stack, creates a new
 * page table (vmm_create_pgd()), and initialises all scheduler / IPC fields.
 * The process is added to process_pool[] in state PROC_CREATED; the caller
 * must call process_load_elf() and enqueue_task() to make it runnable.
 *
 * On failure, partially-allocated resources (PGD, kernel stack, pool slot)
 * are freed and NULL is returned.
 *
 * Locking: holds sched_lock (irqsave) while modifying process_pool[] and
 *          next_pid; releases before vmm_create_pgd() (which takes mm_lock).
 * IRQ context: no.
 *
 * NOTE(SCHED-04): The comment below says "Kernel Stack (16KB)" but
 *          STACK_SIZE is 128KB.  [static, W2 BUG/DOC]
 * NOTE(SCHED-08): memset() zeroes the PMM page even though pmm_alloc_page()
 *          already zeroes; double-zero is harmless but wasteful. [W1 PERF]
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

  /* Filesystem Init */
  strncpy(proc->cwd, "/", sizeof(proc->cwd));

  /* Add to pool */
  process_pool[slot] = proc;
  active_count++;

  spin_unlock_irqrestore(&sched_lock, flags);

  proc->page_table = vmm_create_pgd();

  pr_info("process_create: '%s' PID=%u slot=%u Prio=%d PageTable=%p\n", name,
          (uint32_t)proc->pid, (uint32_t)slot, (int)proc->priority, (void*)proc->page_table);

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

  pr_info("process_create: PID %d context allocated at %p (kstack=%lx)\n",
          proc->pid, (void *)proc->context, proc->kernel_stack);

  proc->on_cpu = -1; /* Not running on any CPU */

  return proc;
}

/*
 * smp_create_idle_task - create and pin the idle task for a specific CPU.
 *
 * Called from the per-CPU bring-up path (CPU 0 creates tasks for all CPUs
 * before releasing secondaries).  The idle task is a pure kernel thread:
 * it never runs user code, so its PGD is destroyed and set to NULL —
 * arch_cpu_switch_context guards on page_table != NULL to skip CR3/TTBR0
 * updates when scheduling the idle task.
 *
 * The context is initialised to start at idle_task_entry() on the idle
 * task's kernel stack.  Memory barriers (hal_mb, hal_isb) and a D-cache
 * clean are issued to ensure the secondary CPU sees the fully initialised
 * context before it starts scheduling.
 *
 * Locking: calls process_create() which acquires sched_lock internally.
 * IRQ context: no.
 */
void smp_create_idle_task(uint32_t cpu_id) {
  extern void idle_task_entry(void);
  
  if (cpu_id >= MAX_CPUS) return;

  struct process *idle =
      process_create("idle", PROC_PRIO_IDLE, PROC_PERM_SYSTEM);
  
  if (idle) {
    idle->on_cpu = cpu_id;

    /* Idle tasks are pure kernel threads — they never run user code.
     * Free the per-process PGD and use NULL: the scheduler and
     * arch_cpu_switch_context both guard on page_table != NULL, so the
     * current kernel CR3 stays active when an idle task is scheduled. */
    if (idle->page_table) {
      vmm_destroy_pgd(idle->page_table);
      idle->page_table = NULL;
    }

    /* Ensure we are writing to the correct per-CPU structure */
    struct cpu_info *info = &cpu_data[cpu_id];
    info->idle_task = idle;

    memset(idle->context, 0, sizeof(struct pt_regs));
    pt_regs_init_kernel_task(idle->context, (uint64_t)idle_task_entry,
                             idle->kernel_stack);

    /* Memory barriers for multi-core visibility */
    hal_cache_clean(idle, sizeof(struct process));
    hal_cache_clean(idle->context, sizeof(struct pt_regs));
    hal_mb();
    hal_isb();
  }
}


/*
 * process_terminate - remove a process from the scheduler and free resources.
 *
 * If the target process is currently executing on another CPU (proc->on_cpu
 * >= 0 and proc != current_process), it is marked PROC_DEAD and left in
 * process_pool[] — the next schedule() call on that CPU will perform the
 * deferred free (cpu_ptr->deferred_free_proc).
 *
 * If the process is terminating itself (current_process == proc), it is
 * marked PROC_ZOMBIE; the caller (sys_exit) must immediately call schedule()
 * to switch away.  Zombie resources are freed by process_wait().
 *
 * If the process is not running on any CPU (on_cpu < 0), resources are freed
 * immediately: kernel stack (pmm_free_pages), page table (vmm_destroy_pgd),
 * and the struct process page (pmm_free_page).
 *
 * System processes (PROC_PERM_SYSTEM) cannot be terminated.
 *
 * IPC message queue is drained (kfree'd) before marking PROC_DEAD for
 * non-current, non-running processes.
 *
 * Locking: acquires sched_lock (irqsave) for pool manipulation; also acquires
 *          wq->lock or t_cpu->sched_lock (via spin_lock, not irqsave) to
 *          remove the process from its runqueue or wait queue.
 * IRQ context: no.
 * Returns: 0 on success, -1 if not found or protected.
 *
 * NOTE(SCHED-03): Zombies are only reaped via process_wait(); without an
 *          explicit waiter the pool slot is leaked permanently. [W2]
 * NOTE(ABI-04): The PROC_PERM_SYSTEM check is the only access-control gate;
 *          any user process may kill any non-system PID. [W4 SECURITY]
 */
int process_terminate(int pid) {
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);

  struct process *proc = __process_find_by_pid(pid);
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
   * NOTE(SCHED-03): if no process calls process_wait(pid), this zombie
   * occupies its pool slot forever. */
    return 0;
  }

  /* Free IPC message queue to prevent leak */
  {
    struct list_head *pos, *q;
    list_for_each_safe(pos, q, &proc->msg_queue) {
      struct ipc_node *node = list_entry(pos, struct ipc_node, list);
      list_del(pos);
      kfree(node);
    }
  }

  proc->state = PROC_DEAD;

  if (proc->on_cpu >= 0) {
    /* Process is currently executing on another CPU.
     * We cannot free its kernel stack while it's being used.
     * Leave it in the pool as PROC_DEAD; schedule() on that CPU
     * will perform the deferred free after switching away. */
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
  }

  /* Not running on any CPU — safe to free immediately */
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

/*
 * start_user_process - directly enter a freshly-created user process.
 *
 * Called only for the very first process (PID 1 / init) or any process that
 * is launched synchronously.  Sets current_process, installs the new PGD,
 * flushes the TLB, and jumps to userland via arch_enter_user_mode().
 *
 * This function does NOT return; arch_enter_user_mode() performs an EL/ring
 * transition and begins executing user code at proc->user_entry.
 *
 * Locking: none — typically called before SMP secondaries are online.
 * IRQ context: no.
 */
void start_user_process(struct process *proc) {
  pr_info("Starting process '%s' PID=%d at 0x%lx\n", proc->name, proc->pid,
          proc->user_entry);

  /* We don't enqueue the first process, we jump directly to it */
  current_process = proc;

  uint64_t pgd_phys = virt_to_phys(proc->page_table);
  /* Set page table and flush TLB */
  hal_vmm_set_pgd(pgd_phys);
  hal_tlb_flush_all();
  hal_isb();

  proc->state = PROC_RUNNING;
  proc->on_cpu = cpu_id();
  arch_enter_user_mode(proc->user_entry, proc->user_stack, proc->kernel_stack);
}

/*
 * schedule - select and switch to the next runnable process.
 *
 * The central scheduler function.  Called from:
 *   - kernel_timer_tick() (timer IRQ, preemption)
 *   - sys_exit / sys_ipc_recv / YIELD syscall (voluntary yield)
 *
 * Steps:
 *  0. Deferred free: if cpu_ptr->deferred_free_proc is set, free that
 *     process's kernel stack, PGD, and struct page now that we have switched
 *     away from it in the previous call.  This is the safe point to free a
 *     stack we were standing on.
 *  1. Save current context (regs) and re-enqueue prev if PROC_RUNNING;
 *     idle tasks are never re-enqueued (they are not on any runqueue).
 *  2. Focus boost (SCHED-01): call compositor_get_focus_pid() and search
 *     all priority levels for the focused PID first.
 *  3. O(1) pick: __builtin_ctz(prio_bitmap) finds the lowest-numbered
 *     non-empty priority queue in one instruction; pop the head task.
 *  4. Work stealing: if local runqueue is empty, iterate over other CPUs
 *     with spin_trylock (to avoid deadlock) and steal the highest-priority
 *     task.  Idle-priority tasks are never stolen (they own their CPU's
 *     kernel stack).
 *  5. Context switch: install next->page_table (if changed), call
 *     arch_cpu_switch_context(next), and return next->context.
 *
 * Locking: acquires cpu_ptr->sched_lock (irqsave) for the duration of steps
 *          1-5; temporarily acquires other_cpu->sched_lock (trylock) during
 *          work stealing; acquires sched_lock (irqsave) during deferred free.
 *          Releases all locks before returning.
 * IRQ context: yes when called from the timer IRQ; no when called from a
 *          syscall.  The function is safe in both contexts because it uses
 *          irqsave variants.
 *
 * NOTE(SCHED-01): compositor_get_focus_pid() is called on every schedule()
 *          invocation — the kernel scheduler has a compile-time dependency on
 *          the graphics compositor. [W3 WRONG-DESIGN]
 * NOTE(SCHED-02): Many pc==0 panic guards reflect past context-corruption
 *          bugs; the function is large and hard to audit. [W2 BAD-IMPL]
 */
struct pt_regs *schedule(struct pt_regs *regs) {
  struct cpu_info *cpu_ptr = get_cpu_info();
  if (!cpu_ptr) {
    if (regs && pt_regs_pc(regs) == 0) panic("SCHED: [EARLY] pc==0 on return");
    return regs;
  }

  /* Deferred process free: the only safe point to release a kernel stack and
   * PGD that was still in use during the previous schedule() call.  By the
   * time we reach here on this CPU, we have already context-switched to a
   * different task and are no longer touching the old stack. */
  if (cpu_ptr->deferred_free_proc) {
    struct process *to_free = cpu_ptr->deferred_free_proc;
    cpu_ptr->deferred_free_proc = NULL;

    /* Remove from pool */
    uint64_t gflags;
    spin_lock_irqsave(&sched_lock, &gflags);
    for (int _i = 0; _i < MAX_PROCESSES; _i++) {
      if (process_pool[_i] == to_free) {
        process_pool[_i] = NULL;
        active_count--;
        break;
      }
    }
    spin_unlock_irqrestore(&sched_lock, gflags);

    if (to_free->kernel_stack)
      pmm_free_pages((void *)(to_free->kernel_stack - STACK_SIZE), STACK_SIZE / 4096);
    if (to_free->page_table)
      vmm_destroy_pgd(to_free->page_table);
    pmm_free_page(to_free);
  }

  uint32_t cpu = cpu_ptr->cpu_id;
  struct process *prev = cpu_ptr->current_task;
  uint64_t flags;

  /* Use local lock for runqueue modifications */
  spin_lock_irqsave(&cpu_ptr->sched_lock, &flags);

  /* if (cpu == 0) pr_info("Schedule Core 0\n"); */
  /* Priority Boosting: identify the focused process from the compositor.
   * NOTE(SCHED-01): This call creates a kernel scheduler -> compositor
   * dependency.  The correct design inverts this: a userland policy server
   * adjusts priority via a capability. [W3 WRONG-DESIGN] */
  extern int compositor_get_focus_pid(void);
  int focus_pid = compositor_get_focus_pid();

    /* 1. Handle Current Process */
    if (prev) {
      /* Check if externally terminated while running on this CPU */
      if (prev->state == PROC_DEAD) {
        /* Cannot free kernel_stack here (we're standing on it).
         * Defer the free to the NEXT schedule() call. */
        prev->on_cpu = -1;
        cpu_ptr->deferred_free_proc = prev;
        cpu_ptr->current_task = NULL;
        prev = NULL;
        goto pick_next;
      }

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
        /* Never re-enqueue the CPU-bound idle task. It must stay out of the
         * runqueue to prevent work-stealing: two CPUs sharing one idle task's
         * kernel stack causes context corruption (ELR=0 crashes). */
        if (prev->priority != PROC_PRIO_IDLE) {
          __enqueue_task(prev);
        }
      } else {
        /* Task is sleeping or dying, keep on_cpu to indicate last CPU for affinity or tracking */
      }
    }

pick_next:;

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
    /* O(1) pick: __builtin_ctz finds the index of the lowest set bit, which
     * corresponds to the highest-priority non-empty queue (lower index = higher
     * priority).  This is the core of the O(1) priority scheduler. */
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
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
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

            /* Never steal idle-priority tasks — they are CPU-bound and share a
             * kernel stack with their owner CPU. Check priority, not pointer,
             * so this also catches any idle task that migrated via wake_up. */
            if (next->priority == PROC_PRIO_IDLE) {
              next = NULL;
              spin_unlock(&other_cpu->sched_lock);
              continue;
            }

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
      if (pt_regs_pc(regs) == 0) {
        panic("SCHED: [CPU%d] BUG pc==0 on PROC_RUNNING fast-path, PID %d", cpu,
              prev->pid);
      }
      spin_unlock_irqrestore(&cpu_ptr->sched_lock, flags);
      return regs;
    }

    /* Otherwise, mandatory fallback to IDLE task */
    next = cpu_ptr->idle_task;
    if (!next) {
      /* Absolute fallback if idle task not yet set */
      if (regs && pt_regs_pc(regs) == 0) {
        panic("SCHED: [CPU%d] BUG pc==0 on idle-fallback return, PID %d", cpu,
              prev ? (int)prev->pid : -1);
      }
      spin_unlock_irqrestore(&cpu_ptr->sched_lock, flags);
      return regs;
    }
  }

  /* 3. Context Switch Logic */
  if (prev == next) {
    if (pt_regs_pc(regs) == 0) {
      panic("SCHED: [CPU%d] BUG pc==0 on same-task return, PID %d", cpu,
            prev->pid);
    }
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
  if (pt_regs_pc(next->context) == 0) {
    panic("SCHED: PC is 0 for PID %d (Name: %s)", next->pid, next->name);
  }

  if (!prev || prev->page_table != next->page_table) {
    uint64_t next_pgd = virt_to_phys(next->page_table);
    if (next_pgd != 0) {
      hal_vmm_set_pgd(next_pgd);
      hal_tlb_flush_all();
      hal_isb();
    }
  }

  /* Final architecture-specific context switch hook */
  arch_cpu_switch_context(next);

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
  struct ipc_node *node = (struct ipc_node *)kmalloc(sizeof(struct ipc_node));
  if (!node)
    return -1;

  memcpy(&node->msg, msg, sizeof(struct ipc_message));

  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);

  struct process *target = __process_find_by_pid(target_pid);
  if (!target || target->state == PROC_DEAD || target->state == PROC_ZOMBIE) {
    spin_unlock_irqrestore(&sched_lock, flags);
    kfree(node);
    return -1;
  }

  /* We must hold target->msg_lock while adding to its queue. 
   * To avoid AB-BA deadlocks with sched_lock, we use trylock or 
   * we ensure a fixed order: sched_lock -> target->msg_lock.
   */
  spin_lock(&target->msg_lock);
  list_add_tail(&node->list, &target->msg_queue);
  
  /* Check if target is waiting */
  if (target->state == PROC_SLEEPING &&
      (target->ipc_target_pid == -1 ||
       target->ipc_target_pid == (int)msg->from)) {
    
    /* Wake target: transition state while holding sched_lock is safe */
    target->state = PROC_READY;
    
    /* To enqueue, we need target CPU's sched_lock */
    int t_id = (target->on_cpu >= 0) ? target->on_cpu : 0;
    struct cpu_info *target_cpu = &cpu_data[t_id];
    
    spin_lock(&target_cpu->sched_lock);
    __enqueue_task(target);
    spin_unlock(&target_cpu->sched_lock);
  }
  
  spin_unlock(&target->msg_lock);
  spin_unlock_irqrestore(&sched_lock, flags);
  
  return 0;
}

int sys_ipc_send(int target_pid, void *msg_ptr) {
  struct ipc_message k_msg;
  if (vmm_copy_from_user(&k_msg, msg_ptr, sizeof(struct ipc_message)) != 0) {
    return -EINVAL;
  }
  k_msg.from = current_process->pid;
  return kernel_ipc_send(target_pid, &k_msg);
}

int sys_ipc_recv(int src_pid, void *msg_ptr) {
  /* 1. Try to pop an existing message */
  struct ipc_node *node = pop_message(current_process, src_pid);
  if (node) {
    if (vmm_copy_to_user(msg_ptr, &node->msg, sizeof(struct ipc_message)) != 0) {
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

  /* Retry the syscall instruction on wake-up */
  pt_regs_retry_syscall(current_process->context);

  return 0;
}

int sys_ipc_try_recv(int src_pid, void *msg_ptr) {
  struct ipc_node *node = pop_message(current_process, src_pid);
  if (node) {
    if (vmm_copy_to_user(msg_ptr, &node->msg, sizeof(struct ipc_message)) != 0) {
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

  /* SCHED-09 (#98): max_count is a raw user argument; the fill loop never
   * writes more than MAX_PROCESSES entries. Clamp to that — this also makes
   * the sizeof(struct ps_info) * max_count product unable to overflow. */
  if (max_count > MAX_PROCESSES)
    max_count = MAX_PROCESSES;

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

  vmm_copy_to_user(user_buf, k_buf, sizeof(struct ps_info) * count);
  kfree(k_buf);
  return count;
}
long sys_sbrk(intptr_t increment) {
  struct process *proc = current_process;
  uint64_t old_brk = proc->heap_end;
  uint64_t new_brk = old_brk + increment;

  if (increment == 0) {
    return (long)old_brk;
  }

  if (increment > 0) {
    /* Map from current end up to new end */
    uint64_t start_map = (old_brk + 4095) & ~(4095ULL);
    uint64_t end_map = (new_brk + 4095) & ~(4095ULL);

    for (uint64_t vaddr = start_map; vaddr < end_map; vaddr += 4096) {
      void *paddr = pmm_alloc_page();
      if (!paddr) {
        return -1;
      }
      memset(paddr, 0, 4096);
      if (vmm_map_page_locked(proc, vaddr, (uint64_t)paddr, PAGE_USER) != 0) {
        pmm_free_page(paddr);
        return -1;
      }
    }
  } else {
    /* Shrinking the heap */
    if (new_brk < proc->heap_start) {
      return -1;
    }

    uint64_t start_unmap = (new_brk + 4095) & ~(4095ULL);
    uint64_t end_unmap = (old_brk + 4095) & ~(4095ULL);

    for (uint64_t vaddr = start_unmap; vaddr < end_unmap; vaddr += 4096) {
      uint64_t paddr = vmm_get_phys(proc->page_table, vaddr);
      if (paddr) {
        vmm_unmap_page_locked(proc, vaddr);
        pmm_free_page((void *)paddr);
      }
    }
  }

  proc->heap_end = new_brk;
  return (long)old_brk;
}
