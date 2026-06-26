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
 *   SCHED-01  (#83 RESOLVED) schedule() no longer calls
 * compositor_get_focus_pid(): the focus boost reads the scheduler-owned
 * keyboard_focus_pid hint directly (the compositor + SYS_SET_FOCUS push updates
 * to it), so the kernel scheduler no longer depends on the graphics compositor.
 * The deeper "userland focus-policy via capability" remains future work.
 *   SCHED-02  (W2 BAD-IMPL) schedule() is large and intricate; many pc==0
 *             panic guards betray past context-corruption bugs.
 *   SCHED-03  (W2 WRONG-DESIGN, MITIGATED) process_wait() is non-blocking
 *             (returns -1 while target is alive).  Zombies are now auto-reaped
 *             by schedule() (prev==ZOMBIE -> reap stack), so unwaited children
 *             no longer leak pool slots; exit-status collection for a future
 *             blocking wait() still needs parent/child links (SCHED-06).
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
 *   IPC-01    RESOLVED — lost-wakeup race: the sender wakes a target only if
 *             it reads PROC_SLEEPING, so a receiver between its failed queue
 *             check and setting SLEEPING could sleep forever on a non-empty
 *             queue.  sys_ipc_recv() now re-checks the queue under msg_lock
 *             (the lock the sender appends under) and sleeps only if still
 *             empty.
 */
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/object.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <stdint.h>
#include <sysstats.h> /* struct os1_sysstats — shared OS1_sys_stats ABI */

/* Process pool - slots can be NULL if process terminated */
/* process_pool[]: fixed-size table of active process descriptors.
 * A NULL slot means it is free.  Protected by sched_lock for modifications;
 * individual slots are also read locklessly by schedule() on the owning CPU. */
struct process *process_pool[MAX_PROCESSES];
static int active_count = 0; /* Number of active processes */
static int next_pid = 1;     /* Global PID counter (never resets) */

/* Real online-CPU count (kernel/cpu.c), set during SMP bring-up.  Reported by
 * sys_sysstats as sched_ncpu — the stats surface never hardcodes a CPU count.
 */
extern uint32_t nr_cpus;

/* Instrumentation (perf brief §1): cumulative real context switches (prev !=
 * next path only).  Directly tests the "full TLB flush per switch" thesis —
 * switches/sec is what that fixed tax is paid against.  Relaxed atomic. */
static uint64_t stat_ctx_switches;

/* SCHED-DOS-01 (#122): effective process limit, derived from usable memory
 * in process_init() (MAX_PROCESSES is only the pool array bound).  The
 * per-process budget below is the kernel-side floor of one process: 128KB
 * kernel stack + descriptor + page tables + a minimal user image ≈ 1 MB. */
#define PROC_MEM_BUDGET_PAGES 256
static int proc_limit = MAX_PROCESSES;

/* __child_count_dec - drop a dying process from its parent's live-children
 * quota (SCHED-DOS-01 #122).  Caller must hold sched_lock.  PIDs are never
 * reused, so a stale parent_pid (parent already gone) finds nothing. */
static void __child_count_dec(struct process *dead) {
  if (dead->parent_pid <= 0)
    return;
  struct process *parent = __process_find_by_pid(dead->parent_pid);
  if (parent && parent->child_count > 0)
    parent->child_count--;
}

/* __reparent_children - re-home a dying process's live children to its
 * nearest live ancestor (SCHED-DOS-02 #122 follow-up).  Caller must hold
 * sched_lock, and must have already removed `dead` from process_pool so the
 * scan cannot find it.
 *
 * Without this, children of a dead parent become permanent orphans: nobody
 * passes the descendant test in process_kill_allowed() (the shell could not
 * kill a dead fork-bomb's children, wedging their pool slots forever) and
 * their cost vanishes from every child_count, so a spawn-and-exit loop
 * evades MAX_PROCS_PER_PARENT.  Adopting them — preferring the dead
 * process's own parent, falling back to init (PID 1) — keeps them killable
 * from the ancestor's shell and keeps the quota charged to a live process.
 * The heir's child_count may transiently exceed MAX_PROCS_PER_PARENT; that
 * only blocks new spawns until the adoptees die, which is the point. */
static void __reparent_children(struct process *dead) {
  struct process *heir = NULL;
  if (dead->parent_pid > 0)
    heir = __process_find_by_pid(dead->parent_pid);
  if (!heir && dead->pid != 1)
    heir = __process_find_by_pid(1);
  int heir_pid = heir ? (int)heir->pid : 0;

  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *p = process_pool[i];
    if (!p || p == dead || p->parent_pid != (int)dead->pid)
      continue;
    p->parent_pid = heir_pid;
    if (heir)
      heir->child_count++;
  }
}

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
/* keyboard_focus_pid: scheduler-owned focus HINT — which PID keystrokes route
 * to (keyboard driver) and which the schedule() focus boost favours. Pushed
 * down by the compositor (window activate/close) and by SYS_SET_FOCUS; the
 * scheduler only reads it (#83). Mutate ONLY via sched_set_focus_pid()
 * (GFX-COMP-01 #67) so the scheduler is the single owner and the compositor
 * never writes this global directly. A single int ⇒ atomic access ⇒ lockless
 * reads (it is a hint). */
int keyboard_focus_pid = 7; /* Default to Shell PID */

/* sched_set_focus_pid - the ONE mutation point for keyboard_focus_pid.
 * Callers: compositor (focus change / window teardown) and SYS_SET_FOCUS. */
void sched_set_focus_pid(int pid) { keyboard_focus_pid = pid; }
/* sched_get_focus_pid - lockless snapshot of the focus hint. */
int sched_get_focus_pid(void) { return keyboard_focus_pid; }

/* window_request_close - window-close INTENT seam, owned by the process layer
 * (GFX-COMP-03 #69). The compositor (graphics) must NOT drive process lifecycle
 * directly: it calls this instead of process_terminate(), so the compositor no
 * longer references the process API and there is one place to evolve the
 * policy. Today it force-terminates to preserve the close-button behaviour.
 * DIR-02 target: deliver a CLOSE event to the window owner for a graceful quit
 * and force-kill only on timeout, and run the kill OUTSIDE mouse-IRQ context
 * (the IRQ-time process_terminate is the separate SCHED-03 follow-up). */
void window_request_close(int pid) { process_terminate(pid); }

/* Bounded focus boost (SCHED-01): the focused process is picked first for
 * snappy foreground response, but never more than FOCUS_BOOST_MAX times in a
 * row — after that one fair round-robin pick runs, so a CPU-bound focused
 * process can never monopolise a core and starve init/nxshell/everything else.
 * Per-CPU streak. */
#define FOCUS_BOOST_MAX 4
static uint32_t sched_focus_streak[MAX_CPUS];

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
/* reap_push - queue a terminated process for deferred destruction on this CPU.
 *
 * The process must already be off every runqueue and must not be current_task
 * on any CPU.  Nodes are chained through the otherwise-unused legacy `next`
 * field and drained at the top of the next schedule() on this CPU (outside
 * sched_lock), where the kernel stack, PGD and struct page are freed.
 *
 * Part of the SCHED-UAF-01 fix: process_terminate() never frees a runnable
 * victim; the scheduler reaps it here once it is provably no longer in use.
 *
 * Locking: caller MUST hold cpu->sched_lock.
 */
static void reap_push(struct cpu_info *cpu, struct process *p) {
  /* SCHED-UAF: queue a victim EXACTLY once.  prev==DEAD on one CPU racing a
   * stale runqueue pick of the same victim on another would otherwise chain it
   * into two deferred lists and double-free its pages.  test-and-set wins the
   * first push; later attempts are dropped (the per-process flag is atomic, so
   * it is safe even though the two reap_push callers hold different per-CPU
   * locks). */
  if (__sync_lock_test_and_set(&p->reaping, 1))
    return;
  p->next = cpu->deferred_free_proc;
  cpu->deferred_free_proc = p;
}

/* Internal helper: Add task to runqueue (Caller MUST hold target->sched_lock)
 */
static void __enqueue_task(struct process *p) {
  /* SCHED-UAF-01: never (re)enqueue a terminated process.  process_terminate()
   * marks a victim PROC_DEAD under the owning CPU's sched_lock; this guard
   * stops a concurrent schedule() from resurrecting it via re-enqueue. */
  if (p->state == PROC_DEAD || p->state == PROC_ZOMBIE)
    return;

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

  /* SCHED-DOS-01 (#122): derive the effective process limit from the memory
   * actually available instead of trusting the hardcoded pool size.  The
   * per-process budget is deliberately generous (kernel stack 128KB +
   * descriptor + page tables + a minimal user image ≈ 1 MB) so the cap
   * shrinks on small-RAM configurations; MAX_PROCESSES stays the array
   * bound.  Floor of 8 keeps init+services+shell bootable regardless. */
  uint64_t budget_pages = pmm_get_free_pages() / PROC_MEM_BUDGET_PAGES;
  proc_limit =
      (budget_pages < MAX_PROCESSES) ? (int)budget_pages : MAX_PROCESSES;
  if (proc_limit < 8)
    proc_limit = 8;
  pr_info("Process: limit %d (pool %d, %d reserved for SYSTEM/ROOT, "
          "%d children max per user process)\n",
          proc_limit, MAX_PROCESSES, RESERVED_PROC_SLOTS, MAX_PROCS_PER_PARENT);

  /* Initialize ALL CPU Runqueues */
  for (int c = 0; c < MAX_CPUS; c++) {
    for (int i = 0; i < MAX_PRIO; i++) {
      INIT_LIST_HEAD(&cpu_data[c].runqueues[i]);
    }
    cpu_data[c].prio_bitmap = 0;
    spin_lock_init(&cpu_data[c].sched_lock);
    INIT_LIST_HEAD(&cpu_data[c].timer_list);
    spin_lock_init(&cpu_data[c].timer_lock);
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
 * process_kill_allowed - ABI-04 capability check for SYS_KILL.
 *
 * Policy (checked under sched_lock so the target cannot be recycled
 * mid-decision):
 *   - privileged callers (machine/root) may kill anything
 *     (process_terminate itself still refuses machine targets);
 *   - any process may kill itself (exit alias) and its DESCENDANTS — the
 *     parent chain is walked, so grandchildren count too.  A dead link in
 *     the chain cannot hide a descendant: __reparent_children() re-homes
 *     orphans to the nearest live ancestor at reap time (the shell can
 *     always clean up after a dead fork-bomb, SCHED-DOS-02);
 *   - everything else is denied.
 * A missing target is "allowed": process_terminate() reports the real
 * -ESRCH-equivalent and keeps the historical return value for it.
 */
int process_kill_allowed(struct process *caller, int target_pid) {
  if (!caller)
    return 1; /* kernel context */
  if (proc_is_privileged(caller))
    return 1;
  if ((int)caller->pid == target_pid)
    return 1;

  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  struct process *target = __process_find_by_pid(target_pid);
  int allowed = !target;
  /* Ancestry walk: a parent always has an older (smaller) PID, so the chain
   * is acyclic and strictly decreasing; the depth bound is belt-and-braces. */
  for (int depth = 0; target && depth < MAX_PROCESSES; depth++) {
    if (target->parent_pid == (int)caller->pid) {
      allowed = 1;
      break;
    }
    if (target->parent_pid <= 0)
      break;
    target = __process_find_by_pid(target->parent_pid);
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return allowed;
}

/*
 * process_ipc_allowed - may 'caller' send IPC to target_pid without
 * CAP_IPC_ANY?  Allowed to the caller's parent or any descendant; the
 * descendant test reuses the acyclic ancestry walk (parent PID < child PID).
 * Acquires sched_lock internally; callers must NOT already hold it.
 */
int process_ipc_allowed(struct process *caller, int target_pid) {
  if (proc_has_cap(caller, CAP_IPC_ANY))
    return 1; /* machine/kernel and CAP_IPC_ANY holders: unrestricted */
  if ((int)caller->pid == target_pid)
    return 1; /* self-send */
  if (caller->parent_pid == target_pid)
    return 1; /* to parent */

  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  struct process *t = __process_find_by_pid(target_pid);
  int allowed = 0;
  for (int depth = 0; t && depth < MAX_PROCESSES; depth++) {
    if (t->parent_pid == (int)caller->pid) {
      allowed = 1;
      break;
    }
    if (t->parent_pid <= 0)
      break;
    t = __process_find_by_pid(t->parent_pid);
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return allowed;
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
/* Per-level capability ceiling = maximum grantable at that level, and also
 * the default preset (a process gets its level's ceiling unless the spawner
 * asks for less via SYS_SPAWN_CAPS).  machine/root/user get everything so
 * today's apps are unchanged; guest is draw-only. */
static const uint32_t level_ceiling[PLVL_COUNT] = {
    [PLVL_MACHINE] = CAP_ALL,
    [PLVL_ROOT] = CAP_ALL,
    [PLVL_USER] = CAP_ALL,
    [PLVL_GUEST] = CAP_WINDOW,
};

/* process_create - spawn at 'level' with that level's default preset. */
struct process *process_create(const char *name, uint8_t priority,
                               uint8_t level) {
  uint8_t lvl = (level < PLVL_COUNT) ? level : PLVL_GUEST;
  return process_create_caps(name, priority, lvl, level_ceiling[lvl]);
}

struct process *process_create_caps(const char *name, uint8_t priority,
                                    uint8_t level, uint32_t req_caps) {
  pr_debug("Process: Creating '%s' (Prio=%d)\n", name,
           priority); /* hot path: demoted (perf §1) */
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);

  /* SCHED-DOS-01 (#122): quotas BEFORE claiming a slot.  Privileged
   * creators (kernel boot, machine/root services) bypass the child quota
   * and may dig into the reserved tail, so recovery — init respawning the
   * shell, the shell killing the bomber — keeps working even when an
   * unprivileged fork bomb has saturated everything else.  pr_debug, not
   * pr_warn: a bomb hitting the quota thousands of times per second must
   * not turn the UART into a second DoS. */
  struct process *creator = current_process;
  int privileged = proc_is_privileged(creator);
  if (active_count >= proc_limit) {
    spin_unlock_irqrestore(&sched_lock, flags);
    pr_debug("Process: limit %d reached, refusing '%s'\n", proc_limit, name);
    return NULL;
  }
  if (!privileged) {
    if (creator->child_count >= MAX_PROCS_PER_PARENT) {
      spin_unlock_irqrestore(&sched_lock, flags);
      pr_debug("Process: PID %d hit the %d-children quota, refusing '%s'\n",
               creator->pid, MAX_PROCS_PER_PARENT, name);
      return NULL;
    }
    if (active_count >= proc_limit - RESERVED_PROC_SLOTS) {
      spin_unlock_irqrestore(&sched_lock, flags);
      pr_debug("Process: only reserved slots left, refusing user '%s'\n", name);
      return NULL;
    }
  }

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

  /* No memset here: pmm_alloc_page() already returns a fully zeroed page
   * (zone_alloc_page memsets PAGE_SIZE) and struct process fits in one page,
   * so the old memset(proc,0,sizeof) was a redundant second zeroing of the
   * same page on every spawn (SCHED-08, perf §1). */
  strncpy(proc->name, name, 15);
  proc->name[15] = '\0';

  /* Assign unique PID */
  proc->pid = next_pid++;

  /* Address-space tag for TLB-tagged switches (perf §3): the pool slot+1, so it
   * is unique among all live address spaces (slots are unique while occupied).
   * 0 is reserved for the kernel/idle space.  The ISA layer (ASID on aarch64 /
   * PCID on amd64) consumes this via arch_cpu_switch_context to switch without
   * a full TLB flush; teardown clears the tag before the slot is recycled. */
  proc->asid = (uint16_t)(slot + 1);

  /* Priority normalization */
  if (priority >= MAX_PRIO)
    priority = MAX_PRIO - 1;
  proc->priority = priority;

  /* Capability/level monotonic cut (USR-SEC-03 #79): the child is never more
   * privileged than its creator (level can only move toward guest), never
   * exceeds its level's ceiling, and never holds a capability the creator
   * lacks.  Escalation is impossible by construction.  A machine creator
   * (and the kernel-internal NULL creator) bypasses the caps clamp. */
  {
    uint8_t lvl = (level < PLVL_COUNT) ? level : PLVL_GUEST;
    if (creator && creator->level > lvl)
      lvl = creator->level;
    uint32_t caps = req_caps & level_ceiling[lvl];
    if (creator && !proc_is_machine(creator))
      caps &= creator->caps;
    proc->level = lvl;
    proc->caps = caps;
  }
  /* Parentage for the SYS_KILL capability check (ABI-04): the spawner is
   * whatever process is current on this CPU; kernel/boot creations get 0. */
  proc->parent_pid = current_process ? (int)current_process->pid : 0;

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

  /* Filesystem Init: a child inherits the spawner's working directory
   * (POSIX), so `kilo init.cfg` launched from /etc opens /etc/init.cfg and not
   * /init.cfg.  Kernel/boot creations (no creator) start at "/". */
  if (creator && creator->cwd[0])
    strncpy(proc->cwd, creator->cwd, sizeof(proc->cwd));
  else
    strncpy(proc->cwd, "/", sizeof(proc->cwd));
  proc->cwd[sizeof(proc->cwd) - 1] = '\0';

  /* Add to pool */
  process_pool[slot] = proc;
  active_count++;
  if (creator)
    creator->child_count++; /* paired with __child_count_dec at release */

  spin_unlock_irqrestore(&sched_lock, flags);

  /* Controlling terminal (USR-TTY-01 #123): the child inherits the spawner's
   * terminal window so a windowless CLI tool's stdout lands in the launching
   * shell (POSIX-like).  This is NOT blanket stdout redirection: sys_write
   * resolves the process's OWN window FIRST and only falls back to ctty_win,
   * so a process that opens its own window (doom, top, forkbomb) renders
   * there, not in the shell.  Resolved outside sched_lock (the compositor
   * lookup takes compositor_lock).  ctty propagates down the tree: the
   * spawner's own window if it has one, else its inherited ctty. */
  proc->ctty_win = -1;
  if (creator) {
    extern int compositor_get_window_by_pid(int pid);
    int term = compositor_get_window_by_pid((int)creator->pid);
    proc->ctty_win = (term > 0) ? term : creator->ctty_win;
  }

  /* stdin/stdout/stderr as capability handles 0/1/2 (ASTRA §6.2: the fd table
   * folded into the object table — replaces the old process_fd_init).  Done
   * outside sched_lock (it kmalloc's the handle table + console object, taking
   * object_lock) — after ctty so the console's stdout resolves correctly.  On
   * OOM roll back the pool slot exactly like the kernel-stack failure path. */
  if (process_install_stdio(proc) != 0) {
    spin_lock_irqsave(&sched_lock, &flags);
    process_pool[slot] = NULL;
    active_count--;
    __child_count_dec(proc);
    spin_unlock_irqrestore(&sched_lock, flags);
    pmm_free_page(proc);
    return NULL;
  }

  proc->page_table = vmm_create_pgd();

  pr_debug("process_create: '%s' PID=%u slot=%u Prio=%d PageTable=%p\n", name,
           (uint32_t)proc->pid, (uint32_t)slot, (int)proc->priority,
           (void *)proc->page_table);

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
    __child_count_dec(proc);
    spin_unlock_irqrestore(&sched_lock, flags); // Release lock
    pmm_free_page(proc);
    return NULL;
  }
  proc->kernel_stack = (uint64_t)kstack_base + STACK_SIZE;

  /* Initial frame on kernel stack */
  proc->context =
      (struct pt_regs *)(proc->kernel_stack - sizeof(struct pt_regs));
  memset(proc->context, 0, sizeof(struct pt_regs));

  pr_debug("process_create: PID %d context allocated at %p (kstack=%lx)\n",
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
 * arch_cpu_switch_context loads the shared kernel_pgd when page_table is
 * NULL (SCHED-UAF-01), so the idle task always runs on a live kernel
 * address space and never inherits a terminated process's PGD.
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

  if (cpu_id >= MAX_CPUS)
    return;

  struct process *idle = process_create("idle", PROC_PRIO_IDLE, PLVL_MACHINE);

  if (idle) {
    idle->on_cpu = cpu_id;

    /* Idle tasks are pure kernel threads — they never run user code.
     * Free the per-process PGD and use NULL: arch_cpu_switch_context loads
     * the shared kernel_pgd for NULL page_table (SCHED-UAF-01 — it must NOT
     * leave the previous process's possibly-freed PGD active). */
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
 * to switch away.  schedule() auto-reaps the zombie via the deferred-free
 * stack on its next pass (process_wait() can still reap it first).
 *
 * If the process is neither READY/RUNNING nor self-terminating (SLEEPING on
 * a wait queue or in IPC, or CREATED-never-scheduled), it is detached from
 * any wait queue and marked DEAD; if it is provably parked (not current_task
 * on its CPU, checked under that CPU's sched_lock) its resources are freed
 * immediately, otherwise the owning CPU's schedule() reaps it (prev==DEAD).
 * Supervisors polling process_wait() must treat -2 as "child gone": an
 * immediately-freed victim never appears as a waitable corpse.
 *
 * Machine-level processes cannot be terminated.
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
 * NOTE(SCHED-03, mitigated): zombies are auto-reaped by schedule(); the
 *          historical pool-slot leak no longer occurs without a waiter.
 * NOTE(ABI-04): the machine-level check here is the last-resort protection;
 *          the SYS_KILL dispatcher gate (process_kill_allowed) restricts a
 *          user process to itself and its descendants.
 */
/*
 * __process_release_created - tear down a half-built PROC_CREATED child (SCHED-
 * UAF Pitfall B).  Same teardown as process_terminate's CREATED immediate-free,
 * factored so process_finalize_spawn / process_abort_spawn share it.  Caller
 * holds NO lock (this takes sched_lock for the pool cleanup).
 */
static void __process_release_created(struct process *p) {
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  int slot = -1;
  for (int i = 0; i < MAX_PROCESSES; i++)
    if (process_pool[i] == p) {
      slot = i;
      break;
    }
  if (slot >= 0) {
    process_pool[slot] = NULL;
    active_count--;
    __child_count_dec(p);
    __reparent_children(p);
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  process_handles_destroy(p);
  if (p->kernel_stack)
    pmm_free_pages((void *)(p->kernel_stack - STACK_SIZE), STACK_SIZE / 4096);
  if (p->page_table)
    vmm_destroy_pgd(p->page_table);
  pmm_free_page(p);
}

/*
 * process_finalize_spawn - commit a freshly-loaded child against a concurrent
 * kill (SCHED-UAF Pitfall B).  Under sched_lock: if a kill was DEFERRED while
 * the ELF loaded (kill_pending), release the child; else enqueue it.  The
 * kill_pending check and the enqueue are BOTH under the global sched_lock, so a
 * kill on another CPU cannot slip between them (global -> per-CPU lock order, as
 * in kernel_ipc_send).
 */
void process_finalize_spawn(struct process *p) {
  if (!p)
    return;
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  int killed = p->kill_pending;
  if (!killed) {
    int tcpu = (p->on_cpu >= 0) ? (int)p->on_cpu : 0;
    struct cpu_info *tc = &cpu_data[tcpu];
    spin_lock(&tc->sched_lock);
    __enqueue_task(p);
    spin_unlock(&tc->sched_lock);
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  if (killed)
    __process_release_created(p);
}

/*
 * process_abort_spawn - the ELF load failed; release the half-built child.  A
 * concurrent kill may have set kill_pending (deferred to us) — either way the
 * child is torn down.
 */
void process_abort_spawn(struct process *p) {
  if (p)
    __process_release_created(p);
}

int process_terminate(int pid) {
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);

  struct process *proc = __process_find_by_pid(pid);
  if (!proc) {
    spin_unlock_irqrestore(&sched_lock, flags);
    return -1;
  }

  /* Don't allow terminating machine-level (system-protected) processes */
  if (proc_is_machine(proc)) {
    pr_warn("Cannot terminate protected process '%s' (PID %d)\n", proc->name,
            pid);
    spin_unlock_irqrestore(&sched_lock, flags);
    return -1;
  }

  /* IDEMPOTENT (SCHED-UAF, the PMM double-free): a victim already DEAD/ZOMBIE is
   * committed to the reaper (schedule's deferred-free) — its windows are already
   * torn down and its pages will be freed exactly once there.  A SECOND
   * terminate (external kill racing a self-exit; window-close + kill; a
   * double-kill) must NOT fall through to the immediate-free common tail below,
   * which would free the same pages a second time.  Short-circuit. */
  if (proc->state == PROC_DEAD || proc->state == PROC_ZOMBIE) {
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
  }

  /* Find the slot for this process */
  int slot = -1;
  for (int i = 0; i < MAX_PROCESSES; i++) {
    if (process_pool[i] == proc) {
      slot = i;
      break;
    }
  }

  pr_debug("Terminating process '%s' PID=%d\n", proc->name,
           pid); /* hot path: demoted (perf §1) */

  /* Tear down any windows this process owns (compositor uses trylock, so this
   * is safe to call while holding sched_lock). */
  extern void compositor_destroy_windows_by_pid(int pid);
  compositor_destroy_windows_by_pid(pid);

  /* Self-termination: we are standing on this process's kernel stack, so we
   * cannot free it now.  Mark ZOMBIE; the caller (sys_exit) MUST call
   * schedule() to switch away — schedule() then auto-reaps the zombie via
   * the per-CPU deferred-free stack (SCHED-03 mitigation). */
  if (current_process == proc) {
    proc->state = PROC_ZOMBIE;
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
  }

  /* SCHED-UAF (Pitfall B): a PROC_CREATED victim is mid-construction in
   * dispatch_spawn — process_load_elf_args is mapping its page table on the
   * creator's CPU (local IRQs off there).  Freeing it now (the immediate-free
   * common tail below) would pull that page table out from under the load (the
   * 0xcccc… UAF in arch_vmm_map ← process_load_elf_args).  DEFER instead: flag
   * it and leave it CREATED; the creator's process_finalize_spawn() releases it
   * after the load.  (Do NOT flip it to PROC_DEAD, or a stray reaper would
   * double-free it.) */
  if (proc->state == PROC_CREATED) {
    proc->kill_pending = 1;
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
  }

  /* Cancel any pending timed-sleep timer (embedded in struct process) so its
   * callback cannot fire on the victim after it is freed. No-op if not armed.
   * Lock order holds: global sched_lock > per-CPU timer_lock. */
  timer_del(&proc->sleep_timer);

  /* Drain the incoming IPC queue (the victim will never read it).  Held under
   * msg_lock to serialise against a concurrent pop_message() on the victim's
   * CPU. */
  {
    struct list_head *pos, *q;
    spin_lock(&proc->msg_lock);
    list_for_each_safe(pos, q, &proc->msg_queue) {
      struct ipc_node *node = list_entry(pos, struct ipc_node, list);
      list_del(pos);
      kfree(node);
    }
    spin_unlock(&proc->msg_lock);
  }

  /*
   * SCHED-UAF-01: do NOT free a process that may still be executing on, or be
   * queued on, another CPU — that is the use-after-free that crashes window
   * close on amd64.  Mark it PROC_DEAD and let the scheduler reap it once it is
   * provably no longer in use:
   *
   *   - On a wait queue: detach it (under wq->lock) and mark DEAD.  It is not
   *     on a runqueue and (we hold the global sched_lock, which kernel_ipc_send
   *     must also take) cannot be woken; left for the reaper.
   *   - PROC_READY / PROC_RUNNING: mark DEAD under the OWNING CPU's sched_lock.
   *     That serialises with the CPU's schedule(), preventing both resurrection
   *     (re-enqueue) and a free-while-referenced.  A running victim is reaped
   *     via schedule()'s prev==DEAD path; a queued victim via its pick==DEAD
   *     path.  We deliberately do not touch its runqueue or free it here.
   *   - PROC_CREATED (never scheduled) or otherwise idle: free immediately —
   *     it is not current_task anywhere and not on any runqueue.
   */
  if (proc->wait_queue_ptr) {
    /* Detach from the wait queue first so a concurrent wake_up() can never
     * resurrect the victim; the parked-vs-running decision is made by the
     * common tail below, exactly as for IPC sleepers. */
    struct wait_queue_head *wq = proc->wait_queue_ptr;
    spin_lock(&wq->lock);
    list_del_init(&proc->run_list);
    proc->wait_queue_ptr = NULL;
    spin_unlock(&wq->lock);
  }

  if (proc->state == PROC_READY || proc->state == PROC_RUNNING) {
    /* Mark DEAD under the owning CPU's sched_lock.  Re-validate on_cpu after
     * taking the lock: while held, the victim cannot migrate (work stealing
     * needs the same lock via trylock), so the mark is ordered against that
     * CPU's schedule(). */
    for (;;) {
      int vcpu = (proc->on_cpu >= 0) ? proc->on_cpu : 0;
      struct cpu_info *vc = &cpu_data[vcpu];
      spin_lock(&vc->sched_lock);
      int now = (proc->on_cpu >= 0) ? proc->on_cpu : 0;
      if (now != vcpu) {
        spin_unlock(&vc->sched_lock);
        continue;
      }
      proc->state = PROC_DEAD;
      spin_unlock(&vc->sched_lock);
      break;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
  }

  /* Common tail: not on a runqueue and not READY/RUNNING — the victim is
   * SLEEPING (wait-queue sleeper detached above, or an IPC sleeper from
   * sys_ipc_recv) or CREATED-never-scheduled.
   *
   * Mark DEAD first, then decide WHO frees.  A sleeper that has set
   * PROC_SLEEPING but not yet switched away is still current_task on its
   * CPU — sys_ipc_recv returns to user mode and only parks at the next
   * tick — so freeing it here would pull the kernel stack and PGD out from
   * under a CPU that is still executing on them (SCHED-UAF family).  That
   * case is left to the owning CPU's schedule(), which reaps it via the
   * prev==DEAD path.  Only a fully parked corpse (its CPU has provably
   * moved on, checked under that CPU's sched_lock) is freed immediately;
   * immediate freeing also means the pool slot disappears right away, so
   * supervisors must treat process_wait()==-2 as "child gone". */
  proc->state = PROC_DEAD;
  {
    int vcpu = (proc->on_cpu >= 0) ? proc->on_cpu : 0;
    struct cpu_info *vc = &cpu_data[vcpu];
    spin_lock(&vc->sched_lock);
    int still_current = (vc->current_task == proc);
    spin_unlock(&vc->sched_lock);
    if (still_current) {
      spin_unlock_irqrestore(&sched_lock, flags);
      return 0;
    }
  }

  if (slot >= 0) {
    process_pool[slot] = NULL;
    active_count--;
    __child_count_dec(proc);
    __reparent_children(proc);
  }
  spin_unlock_irqrestore(&sched_lock, flags);

  process_handles_destroy(proc); /* close capability handles, free objects */
  if (proc->kernel_stack) {
    pmm_free_pages((void *)(proc->kernel_stack - STACK_SIZE),
                   STACK_SIZE / 4096);
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
 *  2. Focus boost (SCHED-01, #83): read the scheduler-owned focus hint
 *     (keyboard_focus_pid) directly and search all priority levels for that PID
 *     first — no call into the compositor.
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
 * IRQ contract (SCHED-IRQ-01): schedule() masks IRQs itself at entry and is
 *          therefore safe to call with IRQs in ANY state (timer IRQ path:
 *          already masked; syscall paths: historically enabled).  No-switch
 *          exits restore the caller's IRQ state; the context-switch exit
 *          returns with IRQs masked and the dispatcher's IRET/ERET loads the
 *          next context's saved flags.
 *
 * SCHED-01 (#83) RESOLVED: the scheduler no longer calls into the compositor.
 *          The focus boost reads the scheduler-owned keyboard_focus_pid hint
 *          directly (compositor + SYS_SET_FOCUS push updates to it), removing
 * the kernel->compositor dependency and the per-schedule compositor_lock.
 * NOTE(SCHED-02): Many pc==0 panic guards reflect past context-corruption
 *          bugs; the function is large and hard to audit. [W2 BAD-IMPL]
 */
struct pt_regs *schedule(struct pt_regs *regs) {
  /* SCHED-IRQ-01: schedule() owns its IRQ state.  Syscall paths used to
   * enter with IRQs enabled; a timer IRQ nesting anywhere in this function
   * could re-enter schedule() on the same CPU — double-draining the
   * deferred-free list (SCHED-UAF-02 family), corrupting the runqueue walk,
   * or leaving cpu_ptr stale if the preemption migrated the task.  Mask for
   * the WHOLE function (before even get_cpu_info(), so cpu_ptr cannot go
   * stale): the non-switch exits restore the caller's IRQ state; the
   * context-switch exit deliberately returns with IRQs masked — the
   * dispatcher's IRET/ERET then loads the next context's saved flags. */
  uint64_t sched_irq_flags = local_irq_save();

  struct cpu_info *cpu_ptr = get_cpu_info();
  if (!cpu_ptr) {
    if (regs && pt_regs_pc(regs) == 0)
      panic("SCHED: [EARLY] pc==0 on return");
    local_irq_restore(sched_irq_flags);
    return regs;
  }

  /* Deferred process free: the only safe point to release a kernel stack and
   * PGD that was still in use during the previous schedule() call.  By the
   * time we reach here on this CPU, we have already context-switched to a
   * different task and are no longer touching the old stack.
   *
   * The list is popped without a lock: it is strictly per-CPU (reap_push
   * only ever runs on the owning CPU) and the function-wide IRQ mask above
   * (SCHED-IRQ-01) prevents a nested schedule() from double-draining it. */
  while (cpu_ptr->deferred_free_proc) {
    struct process *to_free = cpu_ptr->deferred_free_proc;
    cpu_ptr->deferred_free_proc = to_free->next;
    to_free->next = NULL;

    /* Remove from pool */
    uint64_t gflags;
    spin_lock_irqsave(&sched_lock, &gflags);
    for (int _i = 0; _i < MAX_PROCESSES; _i++) {
      if (process_pool[_i] == to_free) {
        process_pool[_i] = NULL;
        active_count--;
        __child_count_dec(to_free);
        __reparent_children(to_free);
        break;
      }
    }
    spin_unlock_irqrestore(&sched_lock, gflags);

    /* Drain leftover IPC messages.  The external-terminate path drains the
     * queue in process_terminate(), but a self-terminated zombie keeps any
     * already-queued nodes.  No lock needed: kernel_ipc_send() refuses
     * DEAD/ZOMBIE targets, so the queue is stable by the time we get here. */
    {
      struct list_head *pos, *q;
      list_for_each_safe(pos, q, &to_free->msg_queue) {
        struct ipc_node *node = list_entry(pos, struct ipc_node, list);
        list_del(pos);
        kfree(node);
      }
    }

    /* Cancel any pending per-process timed-sleep timer BEFORE freeing the PCB,
     * so its callback (proc_sleep_wake) can never fire on freed memory. The
     * external-kill path already does this in process_terminate(), and a
     * process with an armed timer is PROC_SLEEPING (so it cannot self-exit) —
     * but this defensive cancel makes the no-UAF invariant hold on EVERY free
     * path (self-exit zombie, fault-kill, deferred reap) regardless. No-op if
     * the timer is not armed; takes the owner CPU's timer_lock (no lock held
     * here, no inversion). */
    timer_del(&to_free->sleep_timer);

    process_handles_destroy(
        to_free); /* close capability handles, free objects */
    if (to_free->kernel_stack)
      pmm_free_pages((void *)(to_free->kernel_stack - STACK_SIZE),
                     STACK_SIZE / 4096);
    if (to_free->page_table)
      vmm_destroy_pgd(to_free->page_table);
    pmm_free_page(to_free);
  }

  uint32_t cpu = cpu_ptr->cpu_id;
  struct process *prev = cpu_ptr->current_task;
  int prev_reaped = 0; /* set when prev is pushed on the reap list below */
  uint64_t flags;

  /* Tier 3 CPU-time accounting (docs/TIMER-MODEL.md §4): charge the raw counter
   * delta since the last schedule on this CPU to whoever was running (prev),
   * then mark the start count for the task we are about to pick. A bare counter
   * read + subtraction — no divide in the hot path; conversion to ns happens
   * only when the value is read. arch_timer_get_count() is lock-free/IRQ-safe.
   */
  {
    uint64_t now_cnt = arch_timer_get_count();
    if (prev && cpu_ptr->sched_run_count)
      prev->cpu_time_counts += now_cnt - cpu_ptr->sched_run_count;
    cpu_ptr->sched_run_count = now_cnt;
  }

  /* Use local lock for runqueue modifications */
  spin_lock_irqsave(&cpu_ptr->sched_lock, &flags);

  /* if (cpu == 0) pr_info("Schedule Core 0\n"); */
  /* Priority Boosting: focus hint.
   * SCHED-01 (#83) resolved — dependency inverted: the scheduler no longer
   * calls INTO the compositor. It reads the scheduler-owned focus hint
   * (keyboard_focus_pid, defined in this file) directly; the compositor and
   * SYS_SET_FOCUS PUSH updates to it. This also drops
   * compositor_get_focus_pid()'s compositor_lock acquisition from the
   * schedule() hot path. The read is intentionally lockless: a stale value only
   * mis-targets one boost for one tick, which is harmless for a priority hint.
   */
  int focus_pid = keyboard_focus_pid;

  /* 1. Handle Current Process */
  if (prev) {
    /* PROC_DEAD: externally terminated while running on this CPU.
     * PROC_ZOMBIE: terminated itself (sys_exit or fault-path kill) and
     * entered schedule() to switch away.  Both are corpses standing on
     * their own kernel stack, so neither can be freed here: queue on the
     * reap stack; the NEXT schedule() on this CPU frees them after the
     * switch.  Auto-reaping zombies here (instead of waiting for a
     * process_wait() that the shell never issues) closes the SCHED-03
     * pool-slot/PGD leak: doom/demo3d no longer linger as ZOMBIE.
     * Idle tasks never reach this point (they never exit and are
     * machine-level-protected from process_terminate). */
    if (prev->state == PROC_DEAD || prev->state == PROC_ZOMBIE) {
      prev->on_cpu = -1;
      reap_push(cpu_ptr, prev);
      cpu_ptr->current_task = NULL;
      prev = NULL;
      prev_reaped = 1;
      goto pick_next;
    }

    /* Save current context if it was running */
    if (regs) {
      prev->context = regs;
    }

    /* Clear first_run flag since it has now been scheduled and
     * preempted/yielded */
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
      /* Task is sleeping or dying, keep on_cpu to indicate last CPU for
       * affinity or tracking */
    }
  }

pick_next:;

  /* 2. Pick Next Process (O(1) Priority-based Selection) */
  struct process *next = NULL;

pick_local_retry:
  next = NULL;

  if (focus_pid > 0 && sched_focus_streak[cpu] < FOCUS_BOOST_MAX) {
    for (int p = 0; p < MAX_PRIO; p++) {
      if (list_empty(&cpu_ptr->runqueues[p]))
        continue;
      struct process *it;
      list_for_each_entry(it, &cpu_ptr->runqueues[p], run_list) {
        if ((int)it->pid == focus_pid) {
          next = it;
          __dequeue_task(next);
          break;
        }
      }
      if (next)
        break;
    }
  }
  if (next)
    sched_focus_streak[cpu]++; /* honored the focus boost this pick */

  if (!next && cpu_ptr->prio_bitmap != 0) {
    /* O(1) pick: __builtin_ctz finds the index of the lowest set bit, which
     * corresponds to the highest-priority non-empty queue (lower index = higher
     * priority).  This is the core of the O(1) priority scheduler. */
    int best_prio = __builtin_ctz(cpu_ptr->prio_bitmap);

    if (best_prio < MAX_PRIO && !list_empty(&cpu_ptr->runqueues[best_prio])) {
      struct list_head *entry = cpu_ptr->runqueues[best_prio].next;
      next = container_of(entry, struct process, run_list);
      __dequeue_task(next);
      sched_focus_streak[cpu] = 0; /* fair pick ran → refill the focus budget */
    }
  }

  /* SCHED-UAF-01: a terminated process may still be sitting in a runqueue
   * (process_terminate() marks it DEAD without dequeuing).  Never run a corpse
   * — a fault in it would halt amd64 (EXC-AMD64-02).  Reap it and pick again.
   */
  if (next && next->state == PROC_DEAD) {
    reap_push(cpu_ptr, next);
    next = NULL;
    goto pick_local_retry;
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
             * so this also catches any idle task that migrated via wake_up.
             * SCHED-UAF-01: also never steal a terminated process — leave the
             * corpse for its owner CPU to reap via the pick==DEAD path. */
            if (next->priority == PROC_PRIO_IDLE || next->state == PROC_DEAD) {
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
      local_irq_restore(sched_irq_flags); /* SCHED-IRQ-01: no-switch exit */
      return regs;
    }

    /* Otherwise, mandatory fallback to IDLE task */
    next = cpu_ptr->idle_task;
    if (!next) {
      /* Absolute fallback if idle task not yet set.  Returning regs here
       * resumes whatever was interrupted — that is only legal if prev is
       * still alive.  If prev was just reaped (SCHED-UAF-01 family), resuming
       * it would run a corpse whose stack and PGD the next drain frees while
       * they are still in use; that state machine breakage must be fatal. */
      if (prev_reaped) {
        panic(
            "SCHED: [CPU%d] reaped current task with no idle task to switch to",
            cpu);
      }
      if (regs && pt_regs_pc(regs) == 0) {
        panic("SCHED: [CPU%d] BUG pc==0 on idle-fallback return, PID %d", cpu,
              prev ? (int)prev->pid : -1);
      }
      spin_unlock_irqrestore(&cpu_ptr->sched_lock, flags);
      local_irq_restore(sched_irq_flags); /* SCHED-IRQ-01: no-switch exit */
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
    local_irq_restore(sched_irq_flags); /* SCHED-IRQ-01: no-switch exit */
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

  /* Address-space switch is delegated entirely to arch_cpu_switch_context()
   * below — it is the SINGLE source of truth for both arches.  The previous
   * hal_vmm_set_pgd() block here was redundant AND buggy: next_pgd =
   * virt_to_phys(next->page_table) is 0 when page_table == NULL (kernel thread
   * / idle), so it SKIPPED the reload and left the previous (soon-to-be-freed)
   * process PGD active (SCHED-UAF-01).  arch_cpu_switch_context now loads the
   * shared kernel_pgd for NULL page_table on both amd64 and aarch64, and
   * carries the per-arch TLB flush / barriers.  This call is unconditional on
   * the switch path (the prev == next case returned early above). */
  __sync_fetch_and_add(&stat_ctx_switches, 1); /* perf brief §1 */
  arch_cpu_switch_context(next);

  spin_unlock_irqrestore(&cpu_ptr->sched_lock, flags);
  /* SCHED-IRQ-01: context-switch exit — deliberately NO local_irq_restore.
   * We are about to unwind into the dispatcher with another task's frame;
   * IRQs stay masked until IRET/ERET loads the flags saved in that frame.
   * (The flags captured at entry belong to the PREVIOUS task's kernel
   * context and are restored when that context is eventually resumed.) */
  return next->context;
}

/*
 * Wait for a process (non-blocking)
 * Returns PID if terminated (corpse still pending reap), -1 if still
 * running, -2 if not found (never existed, or already auto-reaped by the
 * scheduler).  Pure reporter: freeing belongs to the schedule() reaper.
 */
int process_wait(int pid) {
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *proc = process_pool[i];
    if (proc && (int)proc->pid == pid) {
      if (proc->state == PROC_DEAD || proc->state == PROC_ZOMBIE) {
        /* Corpse freeing is owned EXCLUSIVELY by the scheduler reaper
         * (per-CPU deferred-free stack): a zombie seen here is typically
         * already queued for reaping on its CPU, so freeing it now —
         * as this function historically did — would be a double free.
         * Just report the termination; the slot disappears once drained. */
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
  /* USR-SEC-03 #79: without CAP_IPC_ANY a process may only message its parent
   * or a descendant — a sandboxed worker cannot poke arbitrary services. */
  if (!process_ipc_allowed(current_process, target_pid))
    return -EPERM;
  k_msg.from = current_process->pid;
  return kernel_ipc_send(target_pid, &k_msg);
}

int sys_ipc_recv(int src_pid, void *msg_ptr) {
  /* 1. Try to pop an existing message */
  struct ipc_node *node = pop_message(current_process, src_pid);
  if (node) {
    if (vmm_copy_to_user(msg_ptr, &node->msg, sizeof(struct ipc_message)) !=
        0) {
      /* Drop node and return error */
      kfree(node);
      return -EFAULT;
    }
    kfree(node);
    return 0;
  }

  /* 2. No message ready: commit to sleep.  The gap between the failed pop
   * above and setting PROC_SLEEPING was the IPC-01 lost wakeup: a sender
   * appending in that window saw us still RUNNING and skipped the wake, and
   * we then slept on a non-empty queue with nobody left to wake us.  Close
   * it by re-checking the queue under msg_lock — the same lock
   * kernel_ipc_send() holds while appending and testing our state — and
   * sleeping only if it is still empty.  Lock order msg_lock ->
   * cpu->sched_lock matches the sender's msg_lock -> target-CPU sched_lock
   * (see the locking hierarchy in the file header). */
  uint64_t flags;
  spin_lock_irqsave(&current_process->msg_lock, &flags);

  int have_msg = 0;
  struct list_head *pos;
  list_for_each(pos, &current_process->msg_queue) {
    struct ipc_node *tmp = list_entry(pos, struct ipc_node, list);
    if (src_pid == -1 || tmp->msg.from == (int)src_pid) {
      have_msg = 1;
      break;
    }
  }

  if (!have_msg) {
    struct cpu_info *cpu = get_cpu_info();
    spin_lock(&cpu->sched_lock);
    current_process->ipc_target_pid = src_pid;
    current_process->state = PROC_SLEEPING;
    spin_unlock(&cpu->sched_lock);
  }
  spin_unlock_irqrestore(&current_process->msg_lock, flags);

  /* Retry the syscall instruction on wake-up.  If a message slipped in
   * during the window we stayed RUNNING: the dispatcher's schedule() simply
   * re-runs us and the retried pop succeeds immediately.
   *
   * IPC_RECV_RETRY tells the dispatcher the retry is armed and the trap
   * frame's argument registers must survive untouched (see sched.h). */
  pt_regs_retry_syscall(current_process->context);

  return IPC_RECV_RETRY;
}

int sys_ipc_try_recv(int src_pid, void *msg_ptr) {
  struct ipc_node *node = pop_message(current_process, src_pid);
  if (node) {
    if (vmm_copy_to_user(msg_ptr, &node->msg, sizeof(struct ipc_message)) !=
        0) {
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
      k_buf[count].cpu_time =
          timer_counts_to_ns(process_pool[i]->cpu_time_counts) /
          1000000ULL; /* real CPU time, ms */
      k_buf[count].on_cpu = process_pool[i]->on_cpu;
      count++;
    }
  }
  spin_unlock_irqrestore(&sched_lock, flags);

  vmm_copy_to_user(user_buf, k_buf, sizeof(struct ps_info) * count);
  kfree(k_buf);
  return count;
}

/*
 * proc_state_name - human-readable scheduler state (PROC_*).  Used by the
 * OBJ_TYPE_PROCESS object read: a process reports its live state THROUGH the
 * unified object mechanism (sys_object_read), not a side-channel.
 */
const char *proc_state_name(int state) {
  switch (state) {
  case PROC_CREATED:
    return "created";
  case PROC_RUNNING:
    return "running";
  case PROC_SLEEPING:
    return "sleeping";
  case PROC_ZOMBIE:
    return "zombie";
  case PROC_DEAD:
    return "dead";
  case PROC_READY:
    return "ready";
  default:
    return "unused";
  }
}

/*
 * proc_get_info - snapshot one live process by pid into *out (same fields as
 * sys_getprocs).  Returns 0, or -1 if no such live process.  Takes sched_lock.
 * The kernel-side accessor the OBJ_TYPE_PROCESS object read uses to expose a
 * process's state (object.c cannot touch process_pool/sched_lock directly).
 */
int proc_get_info(int pid, struct ps_info *out) {
  if (!out)
    return -1;
  int rc = -1;
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *p = process_pool[i];
    if (p && (int)p->pid == pid) {
      out->pid = (int)p->pid;
      strncpy(out->name, p->name, sizeof(out->name) - 1);
      out->name[sizeof(out->name) - 1] = '\0';
      out->state = p->state;
      out->priority = p->priority;
      out->cpu_time = timer_counts_to_ns(p->cpu_time_counts) / 1000000ULL;
      out->on_cpu = p->on_cpu;
      rc = 0;
      break;
    }
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return rc;
}

/*
 * proc_enum_pids - fill 'pids' with up to 'max' live process ids; returns the
 * count.  Backs the /proc namespace directory listing (each pid is a PROCESS
 * capability object).  Takes sched_lock.
 */
int proc_enum_pids(int *pids, int max) {
  if (!pids || max <= 0)
    return 0;
  int n = 0;
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  for (int i = 0; i < MAX_PROCESSES && n < max; i++)
    if (process_pool[i])
      pids[n++] = (int)process_pool[i]->pid;
  spin_unlock_irqrestore(&sched_lock, flags);
  return n;
}

/*
 * sys_sysstats - OS1_sys_stats(buf, buf_size): copy one struct os1_sysstats
 * snapshot to userland (perf brief §1 instrumentation surface).  Sibling of
 * sys_getprocs/sys_get_identity; high-level OS1_ introspection, ungated (a
 * process may always read aggregate system stats; no per-object authority is
 * exposed).  Assembles the snapshot from the per-subsystem accessors
 * (pmm/kmalloc/object) plus a single process_pool walk for scheduler load.
 *
 * Forward-compatible: copies min(sizeof(struct os1_sysstats), buf_size) so an
 * older/newer poller still gets the prefix it understands; the struct carries
 * its own version + struct_size.  Returns bytes copied, or -1 on bad pointer.
 *
 * NO HARDCODED CPU COUNT: sched_ncpu is the real nr_cpus; per-CPU load is
 * bucketed by the real on_cpu field into a MAX_CPUS-sized scratch (the real
 * kernel CPU bound), then reduced to runnable-total + deepest-runqueue.
 */
long sys_sysstats(struct os1_sysstats *user_buf, size_t buf_size) {
  if (!user_buf)
    return -1;

  struct os1_sysstats s;
  memset(&s, 0, sizeof(s));
  s.version = OS1_SYSSTATS_VERSION;
  s.struct_size = (unsigned int)sizeof(s);
  s.uptime_ns = timer_counts_to_ns(arch_timer_get_count());

  /* PMM.  Local uint64_t temporaries: the ABI struct uses unsigned long long
   * (dependency-free, identical 8-byte layout on both arches) while the kernel
   * accessors take uint64_t (== unsigned long here) — distinct types under
   * -Werror, so pass temporaries and assign (scalar conversion is fine). */
  s.pmm_total_pages = pmm_get_total_pages();
  s.pmm_free_pages = pmm_get_free_pages();
  {
    uint64_t frag_largest = 0, frag_runs = 0;
    pmm_get_fragmentation(&frag_largest, &frag_runs);
    s.pmm_largest_contig_run = frag_largest;
    s.pmm_free_run_count = frag_runs;

    uint64_t a_calls = 0, f_calls = 0, st_total = 0, st_max = 0;
    pmm_get_counters(&a_calls, &f_calls, &st_total, &st_max);
    s.pmm_alloc_calls = a_calls;
    s.pmm_free_calls = f_calls;
    s.pmm_alloc_search_ns_total = timer_counts_to_ns(st_total);
    s.pmm_alloc_search_ns_max = timer_counts_to_ns(st_max);
  }

  /* kmalloc */
  {
    uint64_t km_total = 0, km_inuse = 0, km_hw = 0, km_live = 0;
    kmalloc_get_stats(&km_total, &km_inuse, &km_hw, &km_live);
    s.km_heap_total_bytes = km_total;
    s.km_bytes_in_use = km_inuse;
    s.km_high_water_bytes = km_hw;
    s.km_live_allocs = km_live;
  }

  /* object / capability layer */
  {
    uint64_t live[OBJ_TYPE_COUNT];
    object_get_live_counts(live, OBJ_TYPE_COUNT);
    for (int i = 0; i < OBJ_TYPE_COUNT; i++)
      s.obj_live_by_type[i] = live[i];
  }

  /* scheduler: one process_pool walk under sched_lock; bucket runnable tasks
   * by their real on_cpu into MAX_CPUS scratch (no ABI CPU constant). */
  {
    uint32_t per_cpu[MAX_CPUS];
    memset(per_cpu, 0, sizeof(per_cpu));
    uint32_t nproc = 0, zombie = 0, runnable = 0;
    uint64_t flags;
    spin_lock_irqsave(&sched_lock, &flags);
    for (int i = 0; i < MAX_PROCESSES; i++) {
      struct process *p = process_pool[i];
      if (!p)
        continue;
      nproc++;
      if (p->state == PROC_DEAD || p->state == PROC_ZOMBIE)
        zombie++;
      if (p->state == PROC_READY || p->state == PROC_RUNNING) {
        runnable++;
        if (p->on_cpu >= 0 && p->on_cpu < MAX_CPUS)
          per_cpu[p->on_cpu]++;
      }
    }
    spin_unlock_irqrestore(&sched_lock, flags);

    uint32_t nc = nr_cpus;
    if (nc > MAX_CPUS)
      nc = MAX_CPUS;
    uint32_t runq_max = 0;
    for (uint32_t c = 0; c < nc; c++)
      if (per_cpu[c] > runq_max)
        runq_max = per_cpu[c];

    s.sched_ctx_switches = stat_ctx_switches;
    s.sched_nproc = nproc;
    s.sched_zombie_count = zombie;
    s.sched_ncpu = nr_cpus;
    s.sched_runnable = runnable;
    s.sched_runq_max = runq_max;
  }

  size_t n = sizeof(s);
  if (buf_size && buf_size < n)
    n = buf_size; /* forward-compat: hand the poller the prefix it asked for */
  if (vmm_copy_to_user(user_buf, &s, n) != 0)
    return -1;
  return (long)n;
}

/* SBRK_HEAP_LIMIT: hard ceiling for the user heap.  The user stack lives at
 * [0xC0000000, 0xC0100000); without a bound a process could sbrk() its heap
 * straight into (or past) the stack mappings.  16MB of guard gap below the
 * stack base. */
#define SBRK_HEAP_LIMIT 0xBF000000UL

long sys_sbrk(intptr_t increment) {
  struct process *proc = current_process;
  uint64_t old_brk = proc->heap_end;
  uint64_t new_brk = old_brk + increment;

  if (increment == 0) {
    return (long)old_brk;
  }

  if (increment > 0) {
    /* Bound the heap: no overflow past the guard below the user stack. */
    if (new_brk < old_brk || new_brk > SBRK_HEAP_LIMIT) {
      return -ENOMEM;
    }
    /* Map from current end up to new end */
    uint64_t start_map = (old_brk + 4095) & ~(4095ULL);
    uint64_t end_map = (new_brk + 4095) & ~(4095ULL);

    for (uint64_t vaddr = start_map; vaddr < end_map; vaddr += 4096) {
      void *paddr = pmm_alloc_page();
      if (!paddr) {
        return -ENOMEM;
      }
      memset(paddr, 0, 4096);
      /* PAGE_USER_DATA: the user heap is never executable (W^X, ELF-02). */
      if (vmm_map_page_locked(proc, vaddr, virt_to_phys(paddr),
                              PAGE_USER_DATA) != 0) {
        pmm_free_page(paddr);
        return -ENOMEM;
      }
    }
  } else {
    /* Shrinking the heap */
    if (new_brk < proc->heap_start) {
      return -EINVAL;
    }

    uint64_t start_unmap = (new_brk + 4095) & ~(4095ULL);
    uint64_t end_unmap = (old_brk + 4095) & ~(4095ULL);

    for (uint64_t vaddr = start_unmap; vaddr < end_unmap; vaddr += 4096) {
      uint64_t paddr = vmm_get_phys(proc->page_table, vaddr);
      if (paddr) {
        vmm_unmap_page_locked(proc, vaddr);
        pmm_free_page(phys_to_virt(paddr));
      }
    }
  }

  proc->heap_end = new_brk;
  return (long)old_brk;
}
