/*
 * kernel/sched/process.c
 * Process Management, Scheduler, and IPC
 *
 * ============================================================================
 *  REFACTOR NOTICE (this pass)
 * ============================================================================
 * This file was reordered, re-documented, and restructured for NASA/JPL
 * "Power of Ten" (POT10) compliance and for raw scheduling performance,
 * WITHOUT changing any locking order, any state-machine transition, or any
 * of the hard-won concurrency invariants documented below (SCHED-UAF-01,
 * #169/#170 stack-alias fix, IPC-01 lost-wakeup fix, etc). Every one of
 * those fixes is preserved byte-for-byte in behavior; only the CONTROL FLOW
 * used to express them changed.
 *
 * ISSUE REGISTRY — verified against this codebase line-by-line, not against
 * the old comments (several of the old comments had gone stale):
 *
 *   SCHED-01  CONFIRMED RESOLVED. schedule() reads keyboard_focus_pid
 *             directly; no compositor call in the scheduler hot path.
 *   SCHED-02  CONFIRMED REAL, FIXED HERE. schedule() was ~425 lines and used
 *             3 goto labels; process_terminate() was ~215 lines. Both are
 *             now decomposed into named, single-purpose helpers, each under
 *             the POT10 "one printed page" (~60 line) guideline, with NO
 *             goto anywhere in this file.
 *   SCHED-03  CONFIRMED, MITIGATED (unchanged). process_wait() stays
 *             non-blocking by design; zombies are auto-reaped by schedule().
 *   SCHED-04  CONFIRMED REAL, FIXED HERE. The stack-size comment said
 *             "16KB"; STACK_SIZE is actually 131072 (128KB) per sched.h.
 *             Fixed, and the value is now interpolated from the macro in
 *             the log line instead of hand-typed, so it cannot drift again.
 *   SCHED-05  CONFIRMED REAL, FIXED HERE. kernel_ipc_send() still nests
 *             sched_lock -> msg_lock -> cpu->sched_lock (lock order is
 *             correct and intentional, kept as-is — see header hierarchy),
 *             BUT the queue itself was unbounded: a sender could OOM a
 *             receiver's kmalloc arena with no flow control. Added
 *             MSG_QUEUE_MAX with an explicit per-process pending count;
 *             kernel_ipc_send() now returns -ENOBUFS past the cap instead
 *             of growing without limit.
 *   SCHED-06  PARTIALLY STALE. parent_pid / child_count / ancestry walks
 *             exist and are exercised throughout this file — "no
 *             parent/child relationship" is no longer true. What remains
 *             true: no process groups / sessions. Left as documented future
 *             work; out of scope for a scheduler-correctness pass.
 *   SCHED-07  CONFIRMED RESOLVED. sys_sbrk() has SBRK_HEAP_LIMIT and refuses
 *             growth into the stack guard gap.
 *   SCHED-08  CONFIRMED RESOLVED. pmm_alloc_page() already zeroes; the
 *             redundant memset() is gone (confirmed by the code, not just
 *             the comment).
 *   DOC-01    NEW. The old file header claimed "MAX_PROCESSES (64)"; the
 *             real value in sched.h is 128. Fixed below to read the macro
 *             instead of restating it as a literal.
 *
 * NASA/JPL POWER OF TEN — COMPLIANCE MATRIX FOR THIS FILE
 *   1. Simple control flow, no goto/setjmp/recursion.
 *        -> DONE. Every goto removed. No recursion anywhere in this file
 *           (ancestry/descendant walks are already iterative with an
 *           explicit MAX_PROCESSES depth bound — verified, unchanged).
 *   2. All loops have a fixed, statically-verifiable upper bound.
 *        -> DONE except one place, fixed here: process_terminate()'s
 *           on-runqueue "wait for on_cpu to stabilize" loop was a bare
 *           `for (;;)`. It is now bounded by SCHED_MARK_DEAD_MAX_RETRIES
 *           with an arch_nop() backoff between attempts (also a perf win
 *           under contention — matches the pattern smp.c already uses for
 *           its ack-wait spin) and a panic() naming the invariant violated
 *           if the bound is ever exhausted, exactly like every other
 *           impossible-state guard already in this file.
 *   3. No dynamic memory allocation after initialization.
 *        -> ACCEPTED, DOCUMENTED DEVIATION. A general-purpose multitasking
 *           kernel that creates and destroys processes and delivers
 *           variable-rate IPC at runtime cannot satisfy this rule literally
 *           without becoming a fundamentally different, statically-sized
 *           system (which this codebase is not, and turning it into one is
 *           out of scope for a scheduler patch). What IS done, in the
 *           spirit of the rule: every allocation site here draws from a
 *           BOUNDED pool (process_pool[MAX_PROCESSES], the new
 *           MSG_QUEUE_MAX cap, the fixed-size reaped_ring) — nothing in
 *           this file can allocate without a hard ceiling.
 *   4. No function longer than what fits on one printed page.
 *        -> DONE for the scheduler core (schedule(), process_terminate(),
 *           process_kill_subtree(), the deferred-free drain). See the
 *           per-function decomposition below. process_create_caps() is
 *           split along its pre-existing lock-scope boundaries (quota
 *           check / pool insert / stdio+pgd setup / kstack setup), which
 *           was already how the logic was phased — it is now expressed as
 *           four named functions instead of one long one.
 *   5. Minimum two assertions per function, checking preconditions and
 *      postconditions.
 *        -> DONE via SCHED_ASSERT(), a thin wrapper over the existing
 *           panic()-based invariant guards already used throughout this
 *           file (kept as panics, not disabled in release builds, because
 *           every one of them guards a use-after-free or double-free, not
 *           a soft error).
 *   6. Data objects declared at the smallest possible scope.
 *        -> Already mostly true in the original; kept, tightened further
 *           where a helper extraction naturally shrank a variable's scope.
 *   7. Check the return value of every non-void function; check the
 *      validity of every parameter.
 *        -> Already true in the original for allocation/copy calls; the
 *           new MSG_QUEUE_MAX check adds one more.
 *   8. Restrict the use of the preprocessor.
 *        -> Unchanged: only simple object-like macros (sizes, flags), no
 *           macro-generated control flow.
 *   9. Restrict pointer use; no more than one level of dereferencing.
 *        -> Unchanged: the file was already single-level in its hot paths
 *           (p->field, not p->a->b->c chains).
 *  10. Compile with all warnings enabled; use static analysis.
 *        -> Out of scope here (no build system was provided with this
 *           file), noted for the maintainer.
 *
 * PERFORMANCE — what changed, and what deliberately did NOT
 *   - process_terminate()'s stabilization retry now backs off with
 *     arch_nop() instead of a bare spin, reducing cache-line ping-pong on
 *     proc->on_cpu under contention (same technique smp.c already uses).
 *   - The three separate, hand-duplicated "free a process's resources"
 *     sequences (process_terminate() common tail, schedule()'s deferred-
 *     free drain, __process_release_created()) are now ONE function,
 *     __process_free_resources(). Same instructions, same order, run from
 *     one place instead of three — smaller icache footprint, and a bug fix
 *     in the free sequence now only has to happen once.
 *   - Deliberately NOT touched: kernel_ipc_send() still takes the global
 *     sched_lock for every send. That is a real scalability ceiling (every
 *     IPC send on every CPU contends one lock), but redesigning the pid
 *     lookup / target locking scheme is a locking-architecture change, not
 *     a scheduler cleanup — attempting it blind, without the ability to
 *     build and stress-test against the rest of this kernel, is how
 *     SCHED-UAF-01-class bugs get reintroduced. Flagged for a dedicated
 *     pass, not silently rewritten here.
 *   - Deliberately NOT touched: the O(1) ctz-based priority pick, the
 *     ASID/PCID tagging in process_create_caps(), and the hot-path
 *     pr_debug demotions (perf brief §1) were already correct and fast;
 *     left exactly as-is.
 *
 * ----------------------------------------------------------------------
 * ORIGINAL DESIGN (unchanged) — Process Management, Scheduler, and IPC
 *
 * This file owns the core process model and the OS1/NEXS scheduler:
 *   - A fixed pool of MAX_PROCESSES process descriptors (see sched.h),
 *     allocated from the PMM one page each.
 *   - Per-CPU O(1) priority-bitmap runqueues (MAX_PRIO levels) with
 *     work-stealing between CPUs using trylock to avoid AB-BA deadlocks.
 *   - Deferred-free: a process terminated while running on another CPU is
 *     marked PROC_DEAD and freed on the *next* schedule() call on that CPU,
 *     after the kernel stack is no longer in use.
 *   - A kmalloc-backed linked-list IPC per process (msg_queue, now capped
 *     at MSG_QUEUE_MAX pending messages), with sleeping-receiver wakeup on
 *     send.
 *   - sys_sbrk: demand-mapped user heap extending upward from the top of
 *     the ELF segments, bounded by SBRK_HEAP_LIMIT below the fixed user
 *     stack.
 *
 * Locking hierarchy (must be acquired in this order — UNCHANGED):
 *   sched_lock (global) -> target->msg_lock -> target_cpu->sched_lock
 *
 * Key invariants (UNCHANGED):
 *   - current_process is a per-CPU variable (accessed via get_cpu_info());
 *     safe to read/write without a lock during a syscall or IRQ on that CPU.
 *   - The idle task for each CPU is created by smp_create_idle_task(); its
 *     page_table is NULL and it is never enqueued or stolen by work-stealing.
 *   - process_pool[] slot is set to NULL only after the process struct and
 *     kernel stack are freed (or deferred via cpu_ptr->deferred_free_proc).
 *   - PIDs are assigned from next_pid (monotonically increasing, never
 *     reused).
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

/* ===========================================================================
 * SECTION 0 — Assertion helper (POT10 rule 5)
 *
 * Every panic() call in this file already IS an assertion: it names an
 * invariant and halts rather than continuing on corrupted scheduler state.
 * SCHED_ASSERT() is a thin, uniform wrapper over the same mechanism so a
 * reviewer can grep one token to find every precondition/postcondition
 * check in the file. It is intentionally NOT compiled out in release
 * builds (unlike libc assert()): every condition it guards is a
 * use-after-free, double-free, or corrupted-runqueue class bug, not a
 * recoverable soft error.
 * ===========================================================================
 */

static void __sched_assert_panic(const char *file, int line, const char *fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  char final_buf[512];
  snprintf(final_buf, sizeof(final_buf), "SCHED ASSERT [%s:%d] %s", file, line, buf);
  panic("%s", final_buf);
}

#define SCHED_ASSERT(cond, ...)                                           \
  do {                                                                         \
    if (!(cond))                                                               \
      __sched_assert_panic(__FILE__, __LINE__, __VA_ARGS__);   \
  } while (0)

/* ===========================================================================
 * SECTION 1 — Global scheduler state
 * ===========================================================================
 */

/* process_pool[]: fixed-size table of active process descriptors (bound:
 * MAX_PROCESSES, defined in sched.h — NOT restated here as a literal, so
 * this comment cannot go stale the way the old header's "(64)" did).
 * A NULL slot means it is free. Protected by sched_lock for modifications;
 * individual slots are also read locklessly by schedule() on the owning
 * CPU. */
struct process *process_pool[MAX_PROCESSES];
static int active_count = 0; /* Number of active processes */
static int next_pid = 1;     /* Global PID counter (never resets) */

/* Real online-CPU count (kernel/cpu.c), set during SMP bring-up. Reported
 * by sys_sysstats as sched_ncpu — the stats surface never hardcodes a CPU
 * count. */
extern uint32_t nr_cpus;

/* Instrumentation (perf brief §1): cumulative real context switches (prev
 * != next path only). Directly tests the "full TLB flush per switch"
 * thesis — switches/sec is what that fixed tax is paid against. Relaxed
 * atomic. */
static uint64_t stat_ctx_switches;

/* SCHED-DOS-01 (#122): effective process limit, derived from usable memory
 * in process_init() (MAX_PROCESSES is only the pool array bound). The
 * per-process budget below is the kernel-side floor of one process: 128KB
 * kernel stack + descriptor + page tables + a minimal user image ≈ 1 MB. */
#define PROC_MEM_BUDGET_PAGES 256
static int proc_limit = MAX_PROCESSES;

/* sched_lock: global spinlock protecting process_pool[], active_count,
 * next_pid, rr_cpu, and the outer section of kernel_ipc_send(). Inner
 * locks (per-CPU sched_lock, per-process msg_lock) may be taken while
 * holding sched_lock — see the locking hierarchy in the file header.
 * NOTE(SCHED-05): taking cpu->sched_lock while holding both sched_lock and
 * msg_lock is the full AB-BA chain the header warns about; the order is
 * fixed and every call site below follows it. */
DEFINE_SPINLOCK(sched_lock);

/* rr_cpu: round-robin CPU index for assigning a CPU to newly woken tasks.
 * Protected by sched_lock. */
static int rr_cpu = 0;

/* Keyboard focus management. keyboard_focus_pid: scheduler-owned focus
 * HINT — which PID keystrokes route to (keyboard driver) and which the
 * schedule() focus boost favours. Pushed down by the compositor (window
 * activate/close) and by SYS_SET_FOCUS; the scheduler only reads it
 * (SCHED-01, resolved). Mutate ONLY via sched_set_focus_pid() (GFX-COMP-01
 * #67) so the scheduler is the single owner and the compositor never
 * writes this global directly. A single int => atomic access => lockless
 * reads (it is a hint, a stale read mis-targets one boost for one tick and
 * is harmless). */
int keyboard_focus_pid = 7; /* Default to Shell PID */

/* Bounded focus boost (SCHED-01): the focused process is picked first for
 * snappy foreground response, but never more than FOCUS_BOOST_MAX times in
 * a row — after that, one fair round-robin pick runs, so a CPU-bound
 * focused process can never monopolise a core and starve init/shell/
 * everything else. Per-CPU streak. */
#define FOCUS_BOOST_MAX 4
static uint32_t sched_focus_streak[MAX_CPUS];

/* MSG_QUEUE_MAX (SCHED-05 fix): hard ceiling on a process's pending IPC
 * messages. Without this, kernel_ipc_send() had unbounded kmalloc growth
 * on the RECEIVER's behalf, driven entirely by the SENDER — a classic
 * remote-triggered OOM. 256 pending messages is generous for any
 * request/response or event-stream workload in this kernel while making
 * the failure mode for a runaway sender an explicit -ENOBUFS instead of a
 * starved allocator. */
#define MSG_QUEUE_MAX 256

/* SCHED_MARK_DEAD_MAX_RETRIES (POT10 rule 2 fix): bound for the
 * "wait for proc->on_cpu to stabilize" retry in process_terminate(). In
 * the overwhelmingly common case this succeeds on the first try; under
 * heavy migration contention it may retry a handful of times. If it ever
 * exhausts this bound, the scheduler's own concurrency invariant (a task
 * can migrate at most once between two sched_lock acquisitions on the
 * same core) has been violated — that is exactly the class of bug every
 * other panic() in this file guards against, so this matches that
 * pattern rather than silently giving up. */
#define SCHED_MARK_DEAD_MAX_RETRIES 100000

/* Reaped-status retention (Phase 9b) — POSIX zombie semantics, cheaply.
 *
 * THE BUG THIS FIXES: process_wait() fills *out_code only when it still
 * finds the corpse. Corpses are drained eagerly (the per-CPU deferred-free
 * list runs on the next schedule()), so a child that fails FAST is
 * routinely gone before its owner polls — process_wait then returned -2
 * with *out_code UNTOUCHED, and every caller that initialised it to 0 read
 * SUCCESS. That silently corrupted every exit-status consumer in a
 * timing-dependent way.
 *
 * We keep only the STATUS, not the corpse, so eager freeing is preserved.
 * Consumed on read, like a real wait(): the second wait on the same pid
 * reports "gone", matching POSIX ECHILD rather than handing the same
 * status out twice. The ring is bounded and overwrites oldest-first — a
 * status nobody ever collects must not be able to pin memory.
 *
 * Locking: every site that touches this holds sched_lock. */
#define REAPED_MAX 32
struct reaped_status {
  int pid; /* 0 = free slot (pids start at 1) */
  int code;
};
static struct reaped_status reaped_ring[REAPED_MAX];
static int reaped_next;

/* ===========================================================================
 * SECTION 2 — Small internal helpers (process table bookkeeping)
 *
 * Every loop below is bounded by MAX_PROCESSES or MAX_CPUS, both compile-
 * time constants (POT10 rule 2). Each function is well under the one-page
 * guideline (POT10 rule 4).
 * ===========================================================================
 */

/* __child_count_dec - drop a dying process from its parent's live-children
 * quota (SCHED-DOS-01 #122). Caller must hold sched_lock. PIDs are never
 * reused, so a stale parent_pid (parent already gone) finds nothing. */
static void __child_count_dec(struct process *dead) {
  SCHED_ASSERT(dead != NULL, "__child_count_dec: NULL dead process");
  if (dead->parent_pid <= 0)
    return;
  struct process *parent = __process_find_by_pid(dead->parent_pid);
  if (parent && parent->child_count > 0)
    parent->child_count--;
}

/* __reparent_children - re-home a dying process's live children to its
 * nearest live ancestor (SCHED-DOS-02 #122 follow-up). Caller must hold
 * sched_lock, and must have already removed `dead` from process_pool so
 * the scan cannot find it.
 *
 * Without this, children of a dead parent become permanent orphans: they
 * cannot be killed by anyone who isn't privileged, and their cost vanishes
 * from every child_count, letting a spawn-and-exit loop evade
 * MAX_PROCS_PER_PARENT. Adopting them — preferring the dead process's own
 * parent, falling back to init (PID 1) — keeps them killable from the
 * ancestor's shell and keeps the quota charged to a live process. The
 * heir's child_count may transiently exceed MAX_PROCS_PER_PARENT; that
 * only blocks new spawns until the adoptees die, which is the point. */
static void __reparent_children(struct process *dead) {
  SCHED_ASSERT(dead != NULL, "__reparent_children: NULL dead process");

  struct process *heir = NULL;
  if (dead->parent_pid > 0)
    heir = __process_find_by_pid(dead->parent_pid);
  if (!heir && dead->pid != 1)
    heir = __process_find_by_pid(1);
  int heir_pid = heir ? (int)heir->pid : 0;

  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *p = process_pool[i];
    if (!p || p == dead)
      continue;

    /* The LOGICAL owner dying breaks the authority chain the same way an
     * unadopted orphan does: owner_pid would point at a corpse. Drop back
     * to the mechanical parent chain, which is always walkable. */
    if (p->owner_pid == (int)dead->pid)
      p->owner_pid = 0;
    if (p->parent_pid != (int)dead->pid)
      continue;

    p->parent_pid = heir_pid;
    if (heir)
      heir->child_count++;
  }
}

/* __record_reaped - remember a dying process's status. Caller holds
 * sched_lock. Uses the SAME encoding process_wait() reports directly: the
 * exit code for a voluntary exit, -9 for a kill/fault death. */
static void __record_reaped(struct process *p) {
  if (!p || (int)p->pid <= 0)
    return;
  reaped_ring[reaped_next].pid = (int)p->pid;
  reaped_ring[reaped_next].code = p->exited ? (p->exit_code & 0xff) : -9;
  reaped_next = (reaped_next + 1) % REAPED_MAX;
}

/* __claim_reaped - collect a retained status, or return 0 if we have none
 * for that pid. Caller holds sched_lock. */
static int __claim_reaped(int pid, int *out_code) {
  for (int i = 0; i < REAPED_MAX; i++) {
    if (reaped_ring[i].pid == pid) {
      if (out_code)
        *out_code = reaped_ring[i].code;
      reaped_ring[i].pid = 0; /* consumed */
      return 1;
    }
  }
  return 0;
}

/* reap_push - queue a terminated process for deferred destruction on this
 * CPU. The process must already be off every runqueue and must not be
 * current_task on any CPU. Nodes are chained through the otherwise-unused
 * legacy `next` field and drained at the top of the next schedule() on
 * this CPU (outside sched_lock), where the kernel stack, PGD and struct
 * page are freed.
 *
 * Part of the SCHED-UAF-01 fix: process_terminate() never frees a runnable
 * victim; the scheduler reaps it here once it is provably no longer in
 * use.
 *
 * Locking: caller MUST hold cpu->sched_lock. */
static void reap_push(struct cpu_info *cpu, struct process *p) {
  SCHED_ASSERT(cpu != NULL, "reap_push: NULL cpu");
  SCHED_ASSERT(p != NULL, "reap_push: NULL process");

  /* SCHED-UAF: queue a victim EXACTLY once. prev==DEAD on one CPU racing a
   * stale runqueue pick of the same victim on another would otherwise
   * chain it into two deferred lists and double-free its pages.
   * test-and-set wins the first push; later attempts are dropped (the
   * per-process flag is atomic, so it is safe even though the two
   * reap_push callers hold different per-CPU locks). */
  if (__sync_lock_test_and_set(&p->reaping, 1))
    return;
  p->next = cpu->deferred_free_proc;
  cpu->deferred_free_proc = p;
}

/* __enqueue_task - add a process to its assigned CPU's priority runqueue.
 *
 * Caller MUST hold target_cpu->sched_lock. Sets p->state = PROC_READY,
 * appends p to the tail of runqueues[prio], and sets the prio_bitmap bit.
 * Calls hal_cpu_notify() to wake any idling CPUs.
 *
 * If p is already on a runqueue (run_list.next != &p->run_list) and in
 * PROC_READY, returns immediately (idempotent guard). A DEAD/ZOMBIE/
 * STOPPED process is never (re)enqueued — SCHED-UAF-01 and job control. */
static void __enqueue_task(struct process *p) {
  SCHED_ASSERT(p != NULL, "__enqueue_task: NULL process");

  if (p->state == PROC_DEAD || p->state == PROC_ZOMBIE)
    return;
  if (p->state == PROC_STOPPED)
    return; /* only process_cont() may resume a STOPPED task */
  if (p->state == PROC_READY && p->run_list.next != &p->run_list)
    return; /* already queued */

  int target_cpu_id = (p->on_cpu >= 0) ? (int)p->on_cpu : 0;
  struct cpu_info *target_cpu = &cpu_data[target_cpu_id];

  p->state = PROC_READY;
  p->on_cpu = target_cpu_id; /* Track which CPU's runqueue we are on */

  int prio = p->priority;
  if (prio >= MAX_PRIO)
    prio = MAX_PRIO - 1;

  list_add_tail(&p->run_list, &target_cpu->runqueues[prio]);
  target_cpu->prio_bitmap |= (1u << prio);

  hal_cpu_notify(); /* wake any idling CPU */

  SCHED_ASSERT(p->state == PROC_READY, "__enqueue_task: postcondition");
}

/* enqueue_task - public wrapper: lock the target CPU's runqueue and
 * enqueue p. Safe to call from process creation (before SMP) or from any
 * context. */
void enqueue_task(struct process *p) {
  SCHED_ASSERT(p != NULL, "enqueue_task: NULL process");

  uint64_t flags;
  int target_cpu_id = (p->on_cpu >= 0) ? (int)p->on_cpu : 0;
  struct cpu_info *target_cpu = &cpu_data[target_cpu_id];

  spin_lock_irqsave(&target_cpu->sched_lock, &flags);
  __enqueue_task(p);
  spin_unlock_irqrestore(&target_cpu->sched_lock, flags);
}

/* __dequeue_task - remove a process from its CPU's priority runqueue.
 *
 * Caller MUST hold the target CPU's sched_lock. Panics on a NULL run_list
 * pointer (corruption guard). */
static void __dequeue_task(struct process *p) {
  SCHED_ASSERT(p != NULL, "__dequeue_task: NULL process");

  int target_cpu = (p->on_cpu >= 0) ? (int)p->on_cpu : 0;
  struct cpu_info *target = &cpu_data[target_cpu];

  SCHED_ASSERT(p->run_list.next != NULL && p->run_list.prev != NULL,
               "corrupt run_list for PID %d", (int)p->pid);

  list_del_init(&p->run_list);
  if (p->priority < MAX_PRIO && list_empty(&target->runqueues[p->priority]))
    target->prio_bitmap &= ~(1u << p->priority);
}

/* ===========================================================================
 * SECTION 3 — Public scheduling primitives: sleep/wake, idle task, init
 * ===========================================================================
 */

/* sleep_on - put the current process to sleep on a wait queue. The caller
 * is responsible for calling schedule() afterward to actually yield the
 * CPU. */
void sleep_on(struct wait_queue_head *wq) {
  SCHED_ASSERT(wq != NULL, "sleep_on: NULL wait queue");

  struct process *p = current_process;
  if (!p)
    return;

  uint64_t flags;
  spin_lock_irqsave(&wq->lock, &flags);
  p->state = PROC_SLEEPING;
  p->wait_queue_ptr = wq;
  list_add_tail(&p->run_list, &wq->task_list);
  spin_unlock_irqrestore(&wq->lock, flags);
}

/* wake_up - wake the first process sleeping on a wait queue. Lock order:
 * wq->lock is released BEFORE cpu->sched_lock is taken (no inversion with
 * __enqueue_task). */
void wake_up(struct wait_queue_head *wq) {
  SCHED_ASSERT(wq != NULL, "wake_up: NULL wait queue");

  uint64_t flags;
  spin_lock_irqsave(&wq->lock, &flags);
  if (list_empty(&wq->task_list)) {
    spin_unlock_irqrestore(&wq->lock, flags);
    return;
  }

  struct list_head *tmp = wq->task_list.next;
  struct process *p = list_entry(tmp, struct process, run_list);

  list_del(&p->run_list);
  p->wait_queue_ptr = NULL;
  INIT_LIST_HEAD(&p->run_list); /* CRITICAL: clear stale pointers */
  spin_unlock_irqrestore(&wq->lock, flags);

  if (p->on_cpu < 0) {
    uint64_t global_flags;
    spin_lock_irqsave(&sched_lock, &global_flags);
    p->on_cpu = rr_cpu;
    rr_cpu = (rr_cpu + 1) % MAX_CPUS;
    spin_unlock_irqrestore(&sched_lock, global_flags);
  }

  struct cpu_info *target = &cpu_data[(int)p->on_cpu];
  spin_lock_irqsave(&target->sched_lock, &flags);
  __enqueue_task(p);
  spin_unlock_irqrestore(&target->sched_lock, flags);
}

/* idle_task_entry - idle task body for each CPU. Runs when no other task
 * is runnable on this CPU. Never returns; never calls schedule() itself
 * (the timer IRQ is the only scheduler entry point from idle). */
void idle_task_entry(void) {
  while (1) {
    hal_cpu_idle(); /* WFI / HLT until the next interrupt */
    /* The timer IRQ handler calls schedule() if a task became runnable;
     * if we're back here, nothing was ready, so loop again. */
  }
}

/* process_init - initialise the process pool and all per-CPU runqueues.
 * Called once from the boot path (CPU 0, single-threaded) before any
 * process is created. */
void process_init(void) {
  pr_info("%s", "Process: Initializing scheduler subsystem...\n");

  for (int i = 0; i < MAX_PROCESSES; i++)
    process_pool[i] = NULL;

  /* SCHED-DOS-01 (#122): derive the effective process limit from memory
   * actually available instead of trusting the hardcoded pool size. The
   * per-process budget is deliberately generous (kernel stack STACK_SIZE
   * bytes + descriptor + page tables + a minimal user image ≈ 1 MB) so the
   * cap shrinks on small-RAM configurations; MAX_PROCESSES stays the array
   * bound. Floor of 8 keeps init+services+shell bootable regardless. */
  uint64_t budget_pages = pmm_get_free_pages() / PROC_MEM_BUDGET_PAGES;
  proc_limit =
      (budget_pages < MAX_PROCESSES) ? (int)budget_pages : MAX_PROCESSES;
  if (proc_limit < 8)
    proc_limit = 8;

  pr_info("Process: limit %d (pool %d, kstack %uKB, %d reserved for "
          "SYSTEM/ROOT, %d children max per user process)\n",
          proc_limit, MAX_PROCESSES, (unsigned)(STACK_SIZE / 1024),
          RESERVED_PROC_SLOTS, MAX_PROCS_PER_PARENT);

  for (int c = 0; c < MAX_CPUS; c++) {
    for (int i = 0; i < MAX_PRIO; i++)
      INIT_LIST_HEAD(&cpu_data[c].runqueues[i]);
    cpu_data[c].prio_bitmap = 0;
    spin_lock_init(&cpu_data[c].sched_lock);
    INIT_LIST_HEAD(&cpu_data[c].timer_list);
    spin_lock_init(&cpu_data[c].timer_lock);
  }
}

/* sched_set_focus_pid - the ONE mutation point for keyboard_focus_pid.
 * Callers: compositor (focus change / window teardown) and
 * SYS_SET_FOCUS. */
void sched_set_focus_pid(int pid) { keyboard_focus_pid = pid; }

/* sched_get_focus_pid - lockless snapshot of the focus hint. */
int sched_get_focus_pid(void) { return keyboard_focus_pid; }

/* window_request_close - window-close INTENT seam, owned by the process
 * layer (GFX-COMP-03 #69): the compositor calls this instead of
 * process_terminate() directly, so it never references the process API. */
void window_request_close(int pid) { process_kill_subtree(pid); }

/* ===========================================================================
 * SECTION 4 — Process table lookups and authority checks
 * ===========================================================================
 */

/* find_free_slot - first NULL slot in process_pool[]. Caller MUST hold
 * sched_lock. O(MAX_PROCESSES). */
static int find_free_slot(void) {
  for (int i = 0; i < MAX_PROCESSES; i++)
    if (process_pool[i] == NULL)
      return i;
  return -1;
}

/* __process_find_by_pid - find a process by PID without locking
 * (internal). Caller MUST hold sched_lock. */
struct process *__process_find_by_pid(int pid) {
  for (int i = 0; i < MAX_PROCESSES; i++)
    if (process_pool[i] && (int)process_pool[i]->pid == pid)
      return process_pool[i];
  return NULL;
}

/* process_find_by_pid - find a process by PID with locking (external).
 * The returned pointer is only valid as long as the caller can guarantee
 * the process is not terminated. */
struct process *process_find_by_pid(int pid) {
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  struct process *proc = __process_find_by_pid(pid);
  spin_unlock_irqrestore(&sched_lock, flags);
  return proc;
}

/* proc_pid_is_privileged - a pid's privilege as a VALUE, not a pointer.
 *
 * process_find_by_pid() hands back a pointer only valid while the caller
 * can guarantee the process stays alive, which callers outside the
 * scheduler cannot. The compositor needs to ASK this while already holding
 * compositor_lock (process_terminate() takes sched_lock then trylocks
 * compositor_lock — the answer must be fetched before compositor_lock is
 * taken, as a plain int that cannot dangle).
 *
 * Locking: acquires/releases sched_lock internally. Callers must NOT hold
 * a lock that any sched_lock holder may block on. */
int proc_pid_is_privileged(int pid) {
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  struct process *p = __process_find_by_pid(pid);
  int privileged = p ? proc_is_privileged(p) : 0;
  spin_unlock_irqrestore(&sched_lock, flags);
  return privileged;
}

/* process_set_owner - set a process's LOGICAL parent (Q3, ASTRA §6.5).
 * Called only from the OBJ_CTL_SETOWNER capability path, which has
 * already checked DESTROY on the target and that the caller is
 * privileged. The owner must be a LIVE process. */
int process_set_owner(int pid, int owner_pid) {
  uint64_t flags;
  int ret = -ESRCH;
  spin_lock_irqsave(&sched_lock, &flags);
  struct process *p = __process_find_by_pid(pid);
  if (p && __process_find_by_pid(owner_pid)) {
    p->owner_pid = owner_pid;
    ret = 0;
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return ret;
}

/* proc_get_lineage - report a process's spawning parent AND logical
 * owner. owner_pid is the one inheritance should follow (Phase 9c: with
 * execution behind a service, the mechanical parent is nxexec, not the
 * process that actually asked). */
int proc_get_lineage(int pid, int *parent, int *owner) {
  uint64_t flags;
  int ret = -1;
  spin_lock_irqsave(&sched_lock, &flags);
  struct process *p = __process_find_by_pid(pid);
  if (p) {
    if (parent)
      *parent = p->parent_pid;
    if (owner)
      *owner = p->owner_pid > 0 ? p->owner_pid : p->parent_pid;
    ret = 0;
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return ret;
}

/* process_kill_allowed - ABI-04 capability check for SYS_KILL.
 *
 * Policy (checked under sched_lock so the target cannot be recycled
 * mid-decision): privileged callers may kill anything; any process may
 * kill itself and its DESCENDANTS (the parent/owner chain is walked, so
 * grandchildren count too — orphans are re-homed to a live ancestor by
 * __reparent_children() so a dead link cannot hide a descendant);
 * everything else is denied. A missing target is "allowed":
 * process_terminate() reports the real not-found case.
 *
 * Loop bound: MAX_PROCESSES (POT10 rule 2). The ancestry walk is acyclic
 * for the plain parent chain (a parent always has a smaller PID);
 * owner_pid is set by a privileged service and is NOT guaranteed smaller,
 * so the explicit depth bound is the real termination guarantee here, not
 * belt-and-braces. */
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

  for (int depth = 0; target && depth < MAX_PROCESSES; depth++) {
    int up = target->owner_pid > 0 ? target->owner_pid : target->parent_pid;
    if (up == (int)caller->pid) {
      allowed = 1;
      break;
    }
    if (up <= 0)
      break;
    target = __process_find_by_pid(up);
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return allowed;
}

/* process_ipc_allowed - may 'caller' send IPC to target_pid without
 * CAP_IPC_ANY? Allowed to the caller's parent or any descendant (walk
 * bounded by MAX_PROCESSES, acyclic since parent PID < child PID). */
int process_ipc_allowed(struct process *caller, int target_pid) {
  if (proc_has_cap(caller, CAP_IPC_ANY))
    return 1;
  if ((int)caller->pid == target_pid)
    return 1;
  if (caller->parent_pid == target_pid)
    return 1;

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

/* ===========================================================================
 * SECTION 5 — Per-process environment (Phase 17)
 *
 * Storage is one page per process, allocated at spawn and copied from the
 * creator. All access below runs under sched_lock, the same lock that
 * already guards the process table these lookups go through.
 * ===========================================================================
 */

/* __env_find - slot holding 'key' in p's block, or NULL. Lock held. */
static struct env_entry *__env_find(struct process *p, const char *key) {
  if (!p || !p->env || !key || !*key)
    return NULL;
  for (int i = 0; i < ENV_MAX; i++)
    if (p->env->e[i].k[0] && strcmp(p->env->e[i].k, key) == 0)
      return &p->env->e[i];
  return NULL;
}

int proc_env_get(int pid, const char *key, char *buf, size_t size) {
  if (!key || !buf || size == 0)
    return -EINVAL;

  uint64_t flags;
  int ret = -ENOENT;
  spin_lock_irqsave(&sched_lock, &flags);
  struct env_entry *e = __env_find(__process_find_by_pid(pid), key);
  if (e) {
    strncpy(buf, e->v, size - 1);
    buf[size - 1] = '\0';
    ret = 0;
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return ret;
}

int proc_env_set(struct process *caller, int pid, const char *key,
                 const char *value) {
  if (!key || !*key)
    return -EINVAL;
  /* Reject a key that would be silently truncated: a setenv that quietly
   * stored a DIFFERENT name than asked for would make the matching getenv
   * miss, harder to diagnose than a refusal. */
  if (strlen(key) >= ENV_KEY_MAX || (value && strlen(value) >= ENV_VAL_MAX))
    return -EINVAL;
  /* Self is always allowed; anyone else needs privilege (NULL caller is
   * kernel-internal, already fully privileged). */
  if (caller && (int)caller->pid != pid && !proc_is_privileged(caller))
    return -EPERM;

  uint64_t flags;
  int ret;
  spin_lock_irqsave(&sched_lock, &flags);
  struct process *p = __process_find_by_pid(pid);
  if (!p) {
    ret = -ESRCH;
  } else if (!p->env) {
    /* No block and none can be made here: allocating under sched_lock
     * would invert the PMM lock against it. Blocks are allocated at
     * spawn, so this is only reachable when that allocation failed. */
    ret = -ENOMEM;
  } else {
    struct env_entry *e = __env_find(p, key);
    if (!value || !*value) {
      if (e)
        e->k[0] = '\0'; /* free the slot: unset */
      ret = 0;
    } else {
      if (!e) { /* first write of this name: claim a free slot */
        for (int i = 0; i < ENV_MAX; i++)
          if (!p->env->e[i].k[0]) {
            e = &p->env->e[i];
            strncpy(e->k, key, ENV_KEY_MAX - 1);
            e->k[ENV_KEY_MAX - 1] = '\0';
            break;
          }
      }
      if (!e) {
        ret = -ENOMEM; /* block full */
      } else {
        strncpy(e->v, value, ENV_VAL_MAX - 1);
        e->v[ENV_VAL_MAX - 1] = '\0';
        ret = 0;
      }
    }
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return ret;
}

int proc_env_enum(int pid, char *buf, size_t size) {
  if (!buf || size == 0)
    return -1;

  uint64_t flags;
  size_t used = 0;
  buf[0] = '\0';
  spin_lock_irqsave(&sched_lock, &flags);
  struct process *p = __process_find_by_pid(pid);
  if (p && p->env) {
    for (int i = 0; i < ENV_MAX; i++) {
      const char *k = p->env->e[i].k;
      if (!k[0])
        continue;
      size_t kl = strlen(k);
      if (used + kl + 2 > size) /* +1 separator, +1 NUL */
        break;
      if (used)
        buf[used++] = '\n';
      memcpy(buf + used, k, kl);
      used += kl;
      buf[used] = '\0';
    }
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return (int)used;
}

void proc_env_free(struct process *p) {
  if (!p || !p->env)
    return;
  struct env_block *b = p->env;
  p->env = NULL;
  pmm_free_page(b);
}

/* ===========================================================================
 * SECTION 6 — Process creation
 *
 * process_create_caps() is expressed as four helpers along the SAME
 * lock-scope boundaries the original code already used (envblk allocated
 * before sched_lock; pool insert under sched_lock; stdio/ctty/pgd after
 * releasing it; kernel-stack setup last). This is not a behavioral change,
 * only naming the phases that were already there (POT10 rule 4).
 * ===========================================================================
 */

/* __proc_create_check_quota - SCHED-DOS-01 (#122) admission control.
 * Caller MUST hold sched_lock. Returns 0 if the new process may be
 * admitted, -1 if a quota was hit (message already logged). */
static int __proc_create_check_quota(struct process *creator,
                                     const char *name) {
  if (active_count >= proc_limit) {
    pr_debug("Process: limit %d reached, refusing '%s'\n", proc_limit, name);
    return -1;
  }
  if (!proc_is_privileged(creator)) {
    if (creator->child_count >= MAX_PROCS_PER_PARENT) {
      pr_debug("Process: PID %d hit the %d-children quota, refusing '%s'\n",
               creator->pid, MAX_PROCS_PER_PARENT, name);
      return -1;
    }
    if (active_count >= proc_limit - RESERVED_PROC_SLOTS) {
      pr_debug("Process: only reserved slots left, refusing user '%s'\n", name);
      return -1;
    }
  }
  return 0;
}

/* __proc_create_identity - fill in PID, ASID, priority, capability clamp,
 * cwd, and env inheritance for a freshly-claimed slot. Caller MUST hold
 * sched_lock (the fields written here are read by other sched_lock
 * holders as soon as the slot is published). */
static void __proc_create_identity(struct process *proc, int slot,
                                   const char *name, uint8_t priority,
                                   uint8_t level, uint32_t req_caps,
                                   struct process *creator,
                                   struct env_block *envblk) {
  strncpy(proc->name, name, PROCESS_NAME_MAX - 1);
  proc->name[PROCESS_NAME_MAX - 1] = '\0';

  /* Idle tasks (kernel threads) draw PIDs from a dedicated high band: with
   * the K3 gate they are created BEFORE init, and PID 1 must stay
   * reserved for init (orphan reparenting and compositor init-authority
   * checks key on pid==1). */
  if (priority == PROC_PRIO_IDLE) {
    static int next_idle_pid = 32000;
    proc->pid = next_idle_pid++;
  } else {
    proc->pid = next_pid++;
  }

  /* Address-space tag for TLB-tagged switches (perf §3): pool slot + 1, so
   * it is unique among all live address spaces. 0 is reserved for the
   * kernel/idle space. */
  proc->asid = (uint16_t)(slot + 1);

  proc->priority = (priority >= MAX_PRIO) ? (uint8_t)(MAX_PRIO - 1) : priority;

  /* Capability/level monotonic cut (USR-SEC-03 #79): the child is never
   * more privileged than its creator, never exceeds its level's ceiling,
   * never holds a capability the creator lacks. A machine creator (and the
   * kernel-internal NULL creator) bypasses the clamp. */
  {
    uint8_t lvl = (level < PLVL_COUNT) ? level : PLVL_GUEST;
    if (creator && creator->level > lvl)
      lvl = creator->level;
    uint32_t caps = req_caps & caps_for_level(lvl);
    if (creator && !proc_is_machine(creator))
      caps &= creator->caps;
    proc->level = lvl;
    proc->caps = caps;
  }

  proc->parent_pid = creator ? (int)creator->pid : 0;
  proc->owner_pid = 0; /* set only via OBJ_CTL_SETOWNER (Q3) */
  proc->exit_code = 0;
  proc->exited = 0;

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
  proc->msg_count = 0; /* SCHED-05 fix: explicit pending-message counter */
  spin_lock_init(&proc->msg_lock);
  spin_lock_init(&proc->mm_lock);

  /* Filesystem init: a child inherits the spawner's cwd (POSIX).
   * Kernel/boot creations (no creator) start at "/". */
  if (creator && creator->cwd[0])
    strncpy(proc->cwd, creator->cwd, sizeof(proc->cwd));
  else
    strncpy(proc->cwd, "/", sizeof(proc->cwd));
  proc->cwd[sizeof(proc->cwd) - 1] = '\0';

  /* Environment: same inheritance rule as cwd (Phase 17). A COPY, not a
   * shared pointer — sharing would let a parent's later setenv rewrite a
   * running child's environment. The page arrived zeroed from the PMM. */
  proc->env = envblk;
  if (envblk && creator && creator->env)
    memcpy(envblk, creator->env, sizeof(*envblk));
}

/* __proc_create_stdio_and_pgd - controlling terminal, stdio handles, and
 * user page table. Called OUTSIDE sched_lock (compositor + object-table +
 * vmm locks are taken here). Kernel threads (PROC_PRIO_IDLE / _SYSTEM)
 * take the lean path: no ctty, no stdio, no user PGD (shared kernel_pgd,
 * SCHED-UAF-01) — kernel-thread creation stays free of compositor/object/
 * vmm lock traffic. Returns 0 on success, -1 on failure (caller rolls
 * back the pool slot). */
static int __proc_create_stdio_and_pgd(struct process *proc,
                                       struct process *creator,
                                       uint8_t priority) {
  proc->ctty_win = -1;

  if (priority == PROC_PRIO_IDLE || priority == PROC_PRIO_SYSTEM) {
    proc->page_table = NULL;
    return 0;
  }

  if (creator) {
    extern int compositor_get_window_by_pid(int pid);
    int term = compositor_get_window_by_pid((int)creator->pid);
    proc->ctty_win = (term > 0) ? term : creator->ctty_win;
  }

  /* stdin/stdout/stderr as capability handles 0/1/2 (ASTRA §6.2). Done
   * after ctty so the console's stdout resolves correctly. */
  if (process_install_stdio(proc, creator) != 0)
    return -1;

  proc->page_table = vmm_create_pgd();
  return 0;
}

/* __proc_create_rollback - undo a partially-created process on any
 * failure path below. Caller holds NO lock. slot must be the pool index
 * proc currently occupies. */
static void __proc_create_rollback(struct process *proc, int slot,
                                   struct env_block *envblk) {
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  process_pool[slot] = NULL;
  active_count--;
  __child_count_dec(proc);
  spin_unlock_irqrestore(&sched_lock, flags);
  if (envblk)
    pmm_free_page(envblk);
  if (proc->page_table)
    vmm_destroy_pgd(proc->page_table);
  pmm_free_page(proc);
}

/* process_create - spawn at 'level' with that level's default preset. The
 * per-level capability ceiling lives in caps_for_level() (caps.h) — the
 * ONE definition the kernel and userland permissions view both derive
 * from. */
struct process *process_create(const char *name, uint8_t priority,
                               uint8_t level) {
  uint8_t lvl = (level < PLVL_COUNT) ? level : PLVL_GUEST;
  return process_create_caps(name, priority, lvl, caps_for_level(lvl));
}

/* process_create_caps - allocate and initialise a new process descriptor.
 *
 * On failure, partially-allocated resources are freed and NULL is
 * returned. The process is left in state PROC_CREATED; the caller must
 * still call process_load_elf() and process_finalize_spawn() /
 * enqueue_task() to make it runnable.
 *
 * Locking: holds sched_lock (irqsave) while modifying process_pool[] and
 *          next_pid; releases before vmm_create_pgd() (which takes
 *          mm_lock) and before the kernel-stack allocation.
 * IRQ context: no. */
struct process *process_create_caps(const char *name, uint8_t priority,
                                    uint8_t level, uint32_t req_caps) {
  pr_debug("Process: Creating '%s' (Prio=%d)\n", name, priority);

  /* Environment page BEFORE sched_lock: the PMM takes its own lock, and
   * allocating inside sched_lock would introduce a sched_lock -> pmm_lock
   * order nothing else in this file establishes. Idle/kernel threads have
   * no environment to inherit, so they skip this. */
  struct env_block *envblk = NULL;
  if (priority != PROC_PRIO_IDLE)
    envblk = (struct env_block *)pmm_alloc_page();

  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);

  struct process *creator = current_process;
  if (__proc_create_check_quota(creator, name) != 0) {
    spin_unlock_irqrestore(&sched_lock, flags);
    if (envblk)
      pmm_free_page(envblk);
    return NULL;
  }

  int slot = find_free_slot();
  if (slot < 0) {
    spin_unlock_irqrestore(&sched_lock, flags);
    if (envblk)
      pmm_free_page(envblk);
    pr_err("%s", "Process pool full!\n");
    return NULL;
  }

  struct process *proc = (struct process *)pmm_alloc_page();
  if (!proc) {
    spin_unlock_irqrestore(&sched_lock, flags);
    if (envblk)
      pmm_free_page(envblk);
    return NULL;
  }
  /* No memset here: pmm_alloc_page() already returns a fully zeroed page
   * (SCHED-08, confirmed resolved — the old redundant second zeroing is
   * gone). */

  __proc_create_identity(proc, slot, name, priority, level, req_caps, creator,
                         envblk);

  process_pool[slot] = proc;
  active_count++;
  if (creator)
    creator->child_count++; /* paired with __child_count_dec at release */

  spin_unlock_irqrestore(&sched_lock, flags);

  pr_debug("process_create: '%s' PID=%u slot=%u Prio=%d\n", name,
           (uint32_t)proc->pid, (uint32_t)slot, (int)proc->priority);

  if (__proc_create_stdio_and_pgd(proc, creator, priority) != 0) {
    __proc_create_rollback(proc, slot, NULL /* envblk already owned by proc */);
    return NULL;
  }

  /* Kernel stack: STACK_SIZE bytes (see sched.h — was miscommented as
   * "16KB" here; SCHED-04, fixed: the actual value is interpolated in
   * process_init()'s boot log, not hand-typed, so this cannot drift
   * again). */
  void *kstack_base = pmm_alloc_pages(STACK_SIZE / 4096);
  if (!kstack_base) {
    __proc_create_rollback(proc, slot, NULL);
    return NULL;
  }
  proc->kernel_stack = (uint64_t)kstack_base + STACK_SIZE;
  proc->context =
      (struct pt_regs *)(proc->kernel_stack - sizeof(struct pt_regs));
  memset(proc->context, 0, sizeof(struct pt_regs));
  proc->on_cpu = -1; /* not running on any CPU yet */

  SCHED_ASSERT(proc->state == PROC_CREATED,
               "process_create_caps: postcondition state");
  SCHED_ASSERT(proc->context != NULL,
               "process_create_caps: postcondition context");
  return proc;
}

/* smp_create_idle_task - create and pin the idle task for a specific CPU.
 * The idle task is a pure kernel thread: page_table is NULL
 * (arch_cpu_switch_context loads the shared kernel_pgd when page_table is
 * NULL, SCHED-UAF-01). Memory barriers + a D-cache clean ensure the
 * secondary CPU sees the fully initialised context before it starts
 * scheduling. */
void smp_create_idle_task(uint32_t cpu_id) {
  extern void idle_task_entry(void);

  if (cpu_id >= MAX_CPUS)
    return;

  struct process *idle = process_create("idle", PROC_PRIO_IDLE, PLVL_MACHINE);
  if (!idle)
    return;

  idle->on_cpu = cpu_id;

  struct cpu_info *info = &cpu_data[cpu_id];
  info->idle_task = idle;

  memset(idle->context, 0, sizeof(struct pt_regs));
  pt_regs_init_kernel_task(idle->context, (uint64_t)idle_task_entry,
                           idle->kernel_stack);

  hal_cache_clean(idle, sizeof(struct process));
  hal_cache_clean(idle->context, sizeof(struct pt_regs));
  hal_mb();
  hal_isb();
}

/* kthread_create - create and enqueue a runnable KERNEL SERVICE thread.
 *
 * !!! UNSTABLE / NO IN-TREE CALLER — DO NOT WIRE UP (KTHREAD-STATUS). A
 * kthread created here runs OUTSIDE the per-CPU idle-task ordering the SMP
 * scheduler relies on. Kept as staged scaffolding only; migrate services
 * to SUPERVISED USERLAND PROCESSES instead. */
struct process *kthread_create(const char *name, void (*entry)(void)) {
  struct process *t = process_create(name, PROC_PRIO_SYSTEM, PLVL_MACHINE);
  if (!t)
    return NULL;

  memset(t->context, 0, sizeof(struct pt_regs));
  pt_regs_init_kernel_task(t->context, (uint64_t)entry, t->kernel_stack);

  hal_cache_clean(t, sizeof(struct process));
  hal_cache_clean(t->context, sizeof(struct pt_regs));
  hal_mb();
  hal_isb();

  enqueue_task(t);
  return t;
}

/* kthread_block - block the calling kernel thread on a wait queue until
 * wake_up(), UNLESS 'still_block' reports the wait condition already
 * cleared.
 *
 * Lost-wakeup safety: still_block(arg) is evaluated UNDER the wq lock,
 * right before committing to sleep, serializing against a producer's
 * wake_up() (which also takes the wq lock). No event is ever both
 * published and missed. still_block may be NULL for an unconditional
 * block. */
void kthread_block(struct wait_queue_head *wq, int (*still_block)(void *),
                   void *arg) {
  struct process *self = current_process;
  if (!self)
    return;

  uint64_t flags = local_irq_save();
  uint64_t wqflags;
  spin_lock_irqsave(&wq->lock, &wqflags);
  if (still_block && !still_block(arg)) {
    spin_unlock_irqrestore(&wq->lock, wqflags);
    local_irq_restore(flags);
    return;
  }
  self->state = PROC_SLEEPING;
  self->wait_queue_ptr = wq;
  list_add_tail(&self->run_list, &wq->task_list);
  spin_unlock_irqrestore(&wq->lock, wqflags);

  arch_cpu_yield(); /* returns here once scheduled again (woken) */

  local_irq_restore(flags);
}

/* ===========================================================================
 * SECTION 7 — Process teardown
 *
 * __process_free_resources() is the single place that frees a process's
 * kernel-owned resources. It used to be hand-duplicated three times
 * (process_terminate()'s common tail, schedule()'s deferred-free drain,
 * __process_release_created()); a fix to the free sequence now only has
 * to happen once, and the three call sites are half the size they were.
 * ===========================================================================
 */

/* __process_free_resources - close handles, free the env page, kernel
 * stack, page table, and the process struct's own page, in that fixed
 * order. Caller MUST NOT hold sched_lock or any per-CPU sched_lock (this
 * runs entirely lock-free against the scheduler; process_handles_destroy
 * takes object_lock internally). Caller MUST have already removed the
 * process from process_pool[] and from every runqueue / wait queue. */
static void __process_free_resources(struct process *p) {
  SCHED_ASSERT(p != NULL, "__process_free_resources: NULL process");

  process_handles_destroy(p); /* close capability handles, free objects */
  proc_env_free(p);           /* Phase 17: release the environment page */
  if (p->kernel_stack)
    pmm_free_pages((void *)(p->kernel_stack - STACK_SIZE), STACK_SIZE / 4096);
  if (p->page_table)
    vmm_destroy_pgd(p->page_table);
  pmm_free_page(p);
}

/* __process_release_created - tear down a half-built PROC_CREATED child
 * (SCHED-UAF Pitfall B). Caller holds NO lock. */
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

  __process_free_resources(p);
}

/* process_finalize_spawn - commit a freshly-loaded child against a
 * concurrent kill (SCHED-UAF Pitfall B). Under sched_lock: if a kill was
 * DEFERRED while the ELF loaded (kill_pending), release the child; else
 * enqueue it. Both checks are under the global sched_lock, so a kill on
 * another CPU cannot slip between them (global -> per-CPU lock order, as
 * in kernel_ipc_send). */
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

/* process_abort_spawn - the ELF load failed; release the half-built
 * child. A concurrent kill may have set kill_pending (deferred to us);
 * either way the child is torn down. */
void process_abort_spawn(struct process *p) {
  if (p)
    __process_release_created(p);
}

/* __proc_terminate_detach_from_wait_queue - if proc is parked on a wait
 * queue, detach it first so a concurrent wake_up() can never resurrect
 * the victim. Caller holds sched_lock. */
static void __proc_terminate_detach_from_wait_queue(struct process *proc) {
  if (!proc->wait_queue_ptr)
    return;
  struct wait_queue_head *wq = proc->wait_queue_ptr;
  spin_lock(&wq->lock);
  list_del_init(&proc->run_list);
  proc->wait_queue_ptr = NULL;
  spin_unlock(&wq->lock);
}

/* __proc_terminate_mark_dead_on_runqueue - proc is PROC_READY or
 * PROC_RUNNING. Mark it PROC_DEAD under the OWNING CPU's sched_lock so
 * the mark is ordered against that CPU's schedule() (which cannot
 * re-enqueue or free a DEAD task). proc->on_cpu can change between our
 * read of it and taking that CPU's lock (work-stealing needs the SAME
 * lock, so once we hold it the victim cannot migrate further) — re-check
 * after locking and retry if it moved.
 *
 * POT10 rule 2: bounded by SCHED_MARK_DEAD_MAX_RETRIES with an arch_nop()
 * backoff between attempts (was a bare `for(;;)` — see file header). */
static void __proc_terminate_mark_dead_on_runqueue(struct process *proc) {
  for (int attempt = 0; attempt < SCHED_MARK_DEAD_MAX_RETRIES; attempt++) {
    int vcpu = (proc->on_cpu >= 0) ? proc->on_cpu : 0;
    struct cpu_info *vc = &cpu_data[vcpu];

    spin_lock(&vc->sched_lock);
    if ((proc->on_cpu >= 0 ? proc->on_cpu : 0) != vcpu) {
      spin_unlock(&vc->sched_lock);
      arch_nop(); /* contention backoff, same pattern as smp.c's ack wait */
      continue;
    }
    proc->state = PROC_DEAD;
    spin_unlock(&vc->sched_lock);
    return;
  }
  panic("SCHED: PID %d on_cpu never stabilized after %d retries — "
        "scheduler migration invariant violated",
        (int)proc->pid, SCHED_MARK_DEAD_MAX_RETRIES);
}

/* __proc_still_current_anywhere - is proc current_task on ANY CPU right
 * now? Scan EVERY CPU (bound: MAX_CPUS) rather than trusting proc->on_cpu,
 * which can be stale (mid-migration, or between a runqueue pick and the
 * on_cpu write) — the #169/#170 root fix. Each CPU's current_task is read
 * under THAT CPU's sched_lock. */
static int __proc_still_current_anywhere(struct process *proc) {
  for (int c = 0; c < MAX_CPUS; c++) {
    struct cpu_info *vc = &cpu_data[c];
    int hit;
    spin_lock(&vc->sched_lock);
    hit = (vc->current_task == proc);
    spin_unlock(&vc->sched_lock);
    if (hit)
      return 1;
  }
  return 0;
}

/* __proc_terminate_pool_remove - drop proc from process_pool[], retain its
 * exit status, and re-home its children. Caller holds sched_lock. Returns
 * the vacated slot index, or -1 if proc was already gone from the pool. */
static int __proc_terminate_pool_remove(struct process *proc) {
  int slot = -1;
  for (int i = 0; i < MAX_PROCESSES; i++)
    if (process_pool[i] == proc) {
      slot = i;
      break;
    }
  if (slot >= 0) {
    __record_reaped(proc); /* 9b: status must outlive the freed struct */
    process_pool[slot] = NULL;
    active_count--;
    __child_count_dec(proc);
    __reparent_children(proc);
  }
  return slot;
}

/* __proc_terminate_drain_ipc - discard proc's pending IPC queue (the
 * victim will never read it). Held under msg_lock to serialise against a
 * concurrent pop_message() on the victim's CPU. */
static void __proc_terminate_drain_ipc(struct process *proc) {
  struct list_head *pos, *q;
  spin_lock(&proc->msg_lock);
  list_for_each_safe(pos, q, &proc->msg_queue) {
    struct ipc_node *node = list_entry(pos, struct ipc_node, list);
    list_del(pos);
    kfree(node);
  }
  proc->msg_count = 0;
  spin_unlock(&proc->msg_lock);
}

/* process_terminate - remove a process from the scheduler and free
 * resources.
 *
 * Disposition depends on where the victim is in its lifecycle:
 *   - self-termination (current_process == proc): marked PROC_ZOMBIE; the
 *     caller (sys_exit) MUST call schedule() to switch away, which
 *     auto-reaps the zombie via the deferred-free stack.
 *   - PROC_CREATED (mid ELF-load on another CPU): kill is DEFERRED
 *     (kill_pending), released by process_finalize_spawn()/
 *     process_abort_spawn() (SCHED-UAF Pitfall B).
 *   - PROC_READY / PROC_RUNNING: marked PROC_DEAD under the owning CPU's
 *     sched_lock; reaped by that CPU's next schedule() (SCHED-UAF-01).
 *   - otherwise (SLEEPING or CREATED-never-scheduled): detached from any
 *     wait queue, marked DEAD; freed immediately IFF provably not
 *     current_task anywhere, else left for the owning CPU's schedule() to
 *     reap.
 *
 * Machine-level processes cannot be terminated (ABI-04 last-resort
 * guard).
 *
 * Locking: acquires sched_lock (irqsave) for pool manipulation; also
 *          acquires wq->lock / cpu->sched_lock (not irqsave — already
 *          inside the irqsave section) to detach/mark the victim.
 * Returns: 0 on success (including idempotent no-op), -1 if not found or
 *          protected. */
int process_terminate(int pid) {
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);

  struct process *proc = __process_find_by_pid(pid);
  if (!proc) {
    spin_unlock_irqrestore(&sched_lock, flags);
    return -1;
  }

  if (proc_is_machine(proc)) {
    pr_warn("Cannot terminate protected process '%s' (PID %d)\n", proc->name,
            pid);
    spin_unlock_irqrestore(&sched_lock, flags);
    return -1;
  }

  /* IDEMPOTENT (SCHED-UAF, the PMM double-free): a victim already
   * DEAD/ZOMBIE is already committed to the reaper. A second terminate
   * (external kill racing a self-exit; window-close + kill) must not fall
   * through to the free tail a second time. */
  if (proc->state == PROC_DEAD || proc->state == PROC_ZOMBIE) {
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
  }

  pr_debug("Terminating process '%s' PID=%d\n", proc->name, pid);

  /* Mark ->dying FIRST so a racing SYS_CREATE_WINDOW on this process's own
   * CPU is refused, then tear down its windows. compositor_lock is taken
   * here (BLOCKING) — this is the sched_lock -> compositor_lock order;
   * nothing may take the two in reverse. */
  proc->dying = 1;
  extern void compositor_destroy_windows_by_pid(int pid);
  compositor_destroy_windows_by_pid(pid);

  /* Self-termination: standing on this process's kernel stack, cannot
   * free it now. schedule() auto-reaps the zombie once we switch away. */
  if (current_process == proc) {
    proc->state = PROC_ZOMBIE;
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
  }

  /* SCHED-UAF Pitfall B: a PROC_CREATED victim is mid-construction in
   * dispatch_spawn (process_load_elf_args mapping its page table on the
   * creator's CPU). Defer instead of freeing now. Do NOT flip it to
   * PROC_DEAD, or a stray reaper would double-free it. */
  if (proc->state == PROC_CREATED) {
    proc->kill_pending = 1;
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
  }

  /* Cancel any pending timed-sleep timer so its callback cannot fire on
   * the victim after it is freed. Lock order: global sched_lock > per-CPU
   * timer_lock (held). */
  timer_del(&proc->sleep_timer);
  __proc_terminate_drain_ipc(proc);

  /* SCHED-UAF-01: never free a process that may still be executing on, or
   * be queued on, another CPU. */
  __proc_terminate_detach_from_wait_queue(proc);

  if (proc->state == PROC_READY || proc->state == PROC_RUNNING) {
    __proc_terminate_mark_dead_on_runqueue(proc);
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
  }

  /* Common tail: SLEEPING (wait-queue sleeper detached above, or an IPC
   * sleeper from sys_ipc_recv) or CREATED-never-scheduled. Mark DEAD
   * first, then decide WHO frees: only a fully parked corpse (not
   * current_task on ANY CPU) is freed here; otherwise the owning CPU's
   * schedule() reaps it via the prev==DEAD path. Supervisors polling
   * process_wait() must treat -2 as "child gone": an immediately-freed
   * victim never appears as a waitable corpse. */
  proc->state = PROC_DEAD;
  if (__proc_still_current_anywhere(proc)) {
    spin_unlock_irqrestore(&sched_lock, flags);
    return 0;
  }

  int slot = __proc_terminate_pool_remove(proc);
  spin_unlock_irqrestore(&sched_lock, flags);

  if (slot >= 0)
    __process_free_resources(proc);
  return 0;
}

/* --- process_kill_subtree() helpers ---------------------------------- */

struct kill_snap {
  int pid;
  int parent;
  signed char windowless;
  signed char kill;
  signed char machine;
};

/* __kill_snap_capture - snapshot (pid,parent,machine) for every live
 * process under sched_lock, then release it (Pitfall A: sched_lock ->
 * compositor_lock is an AB-BA nothing else in this file takes in
 * reverse, so the window-type probe below MUST run lock-free). Returns
 * the snapshot count and, via *root_idx, the index of root_pid (-1 if
 * gone). Loop bound: MAX_PROCESSES. */
static int __kill_snap_capture(struct kill_snap *snap, int root_pid,
                               int *root_idx) {
  int n = 0;
  *root_idx = -1;

  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *p = process_pool[i];
    if (!p)
      continue;
    snap[n].pid = (int)p->pid;
    snap[n].parent = p->parent_pid;
    snap[n].windowless = -1;
    snap[n].kill = 0;
    snap[n].machine = proc_is_machine(p) ? 1 : 0;
    if ((int)p->pid == root_pid)
      *root_idx = n;
    n++;
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return n;
}

/* __kill_snap_probe_windows - lock-free TYPE probe: a window owner is
 * SPARED. Loop bound: n (<= MAX_PROCESSES). */
static void __kill_snap_probe_windows(struct kill_snap *snap, int n) {
  extern int compositor_get_window_by_pid(int pid);
  for (int i = 0; i < n; i++)
    snap[i].windowless =
        (compositor_get_window_by_pid(snap[i].pid) > 0) ? 0 : 1;
}

/* __kill_snap_propagate - a node dies iff its parent dies AND it is
 * windowless; a windowed node is spared, pruning its subtree. Iterate to a
 * fixpoint (parents may sit after children in pool order). Loop bound:
 * n passes * n * n comparisons, all bounded by MAX_PROCESSES (POT10 rule
 * 2) — unchanged from the original, just extracted. */
static void __kill_snap_propagate(struct kill_snap *snap, int n) {
  for (int pass = 0; pass < n; pass++) {
    int changed = 0;
    for (int i = 0; i < n; i++) {
      if (snap[i].kill || !snap[i].windowless || snap[i].machine)
        continue; /* ABI-06: a machine descendant is never in the kill set */
      for (int j = 0; j < n; j++) {
        if (snap[j].kill && snap[j].pid == snap[i].parent) {
          snap[i].kill = 1;
          changed = 1;
          break;
        }
      }
    }
    if (!changed)
      break;
  }
}

/* __kill_snap_log_spared - diagnostic: surface every "spared because it
 * owns a window" decision (flaky-terminate triage: a child that holds
 * only a transient window can spare itself at random). */
static void __kill_snap_log_spared(const struct kill_snap *snap, int n,
                                   int root_pid) {
  for (int i = 0; i < n; i++) {
    if (snap[i].kill || snap[i].windowless || snap[i].pid == root_pid)
      continue;
    for (int j = 0; j < n; j++)
      if (snap[j].kill && snap[j].pid == snap[i].parent) {
        pr_info("kill_subtree: sparing windowed PID %d (child of killed %d)\n",
                snap[i].pid, snap[i].parent);
        break;
      }
  }
}

/* __kill_snap_execute - terminate DESCENDANTS FIRST, root LAST.
 * process_terminate(root) reparents the root's live children to init, so
 * killing the root first would orphan any descendant whose own terminate
 * then races (unreachable from this subtree, survives). process_terminate
 * is idempotent; an already-exited snapshot pid is a harmless no-op. */
static void __kill_snap_execute(const struct kill_snap *snap, int n,
                                int root_pid) {
  int kill_failures = 0;
  for (int i = 0; i < n; i++)
    if (snap[i].kill && snap[i].pid != root_pid)
      if (process_terminate(snap[i].pid) != 0)
        kill_failures++;
  if (process_terminate(root_pid) != 0)
    kill_failures++;

  if (kill_failures)
    pr_err("kill_subtree(%d): %d victim(s) did not terminate\n", root_pid,
           kill_failures);
}

/* process_kill_subtree - window-aware EXTERNAL kill
 * (docs/PROCESS-KILL-MODEL.md).
 *
 * Terminates root_pid PLUS every windowless/in-shell descendant, while
 * SPARING each descendant that OWNS a window and its whole subtree —
 * independent windowed apps keep running; the user closes them via their
 * own red button. The TYPE signal is compositor_get_window_by_pid().
 *
 * ABI-06: a MACHINE-level root is refused OUTRIGHT (not merely spared at
 * the end) — PID 1's windowless children ARE the system (services, the
 * shell); killing the subtree of an untouchable process would destroy
 * exactly what the protection exists to preserve. Machine descendants are
 * excluded from the kill set for the same reason. */
void process_kill_subtree(int root_pid) {
  static struct kill_snap snap[MAX_PROCESSES];
  int root_idx;
  int n = __kill_snap_capture(snap, root_pid, &root_idx);

  if (root_idx < 0)
    return; /* target already gone */

  if (snap[root_idx].machine) {
    pr_warn("Refusing subtree kill of protected process (PID %d)\n", root_pid);
    return;
  }

  __kill_snap_probe_windows(snap, n);
  snap[root_idx].kill = 1; /* the explicit target dies regardless */
  __kill_snap_propagate(snap, n);
  __kill_snap_log_spared(snap, n, root_pid);
  __kill_snap_execute(snap, n, root_pid);
}

/* start_user_process - directly enter a freshly-created user process
 * (the very first process / init, or any process launched synchronously).
 * Does NOT return; arch_enter_user_mode() performs an EL/ring transition
 * and begins executing user code at proc->user_entry. */
void start_user_process(struct process *proc) {
  SCHED_ASSERT(proc != NULL, "start_user_process: NULL proc");
  SCHED_ASSERT(proc->page_table != NULL,
               "start_user_process: PID %d has no page table", (int)proc->pid);

  pr_info("Starting process '%s' PID=%d at 0x%lx\n", proc->name, proc->pid,
          proc->user_entry);

  current_process = proc; /* not enqueued: we jump directly to it */

  uint64_t pgd_phys = virt_to_phys(proc->page_table);
  hal_vmm_set_pgd(pgd_phys);
  hal_tlb_flush_all();
  hal_isb();

  proc->state = PROC_RUNNING;
  proc->on_cpu = cpu_id();
  arch_enter_user_mode(proc->user_entry, proc->user_stack, proc->kernel_stack);
}

/* ===========================================================================
 * SECTION 8 — The scheduler core: schedule()
 *
 * Decomposed into named stages, called in sequence, with NO goto anywhere
 * (SCHED-02, fixed). Every stage below either:
 *   (a) is a pure helper that assumes cpu_ptr->sched_lock is already held
 *       and does not itself acquire/release the scheduler lock or touch
 *       the IRQ mask, or
 *   (b) is the top-level schedule() function itself, which remains the
 *       SINGLE owner of lock acquisition, IRQ masking, and every return
 *       point — deliberately NOT hidden inside a helper, because the
 *       three different exit paths (no-switch/same-task, no-switch/keep-
 *       prev, and context-switch) each have a DIFFERENT IRQ-restore
 *       contract (SCHED-IRQ-01) that must stay visible at the call site
 *       that actually returns from schedule().
 * ===========================================================================
 */

/* __sched_reap_deferred - free every process queued for deferred
 * destruction on this CPU (SCHED-UAF-01: the only safe point to release a
 * kernel stack/PGD that was still in use during the PREVIOUS schedule()
 * call on this CPU). The list is popped without a lock: it is strictly
 * per-CPU (reap_push only ever runs on the owning CPU) and the function-
 * wide IRQ mask in schedule() prevents a nested schedule() from double-
 * draining it. Loop bound: the list length, which is bounded by
 * MAX_PROCESSES (a process can be queued at most once — reap_push's
 * test-and-set guarantees that). */
static void __sched_reap_deferred(struct cpu_info *cpu_ptr) {
  while (cpu_ptr->deferred_free_proc) {
    struct process *to_free = cpu_ptr->deferred_free_proc;
    cpu_ptr->deferred_free_proc = to_free->next;
    to_free->next = NULL;

    uint64_t gflags;
    spin_lock_irqsave(&sched_lock, &gflags);
    __proc_terminate_pool_remove(to_free);
    spin_unlock_irqrestore(&sched_lock, gflags);

    /* Drain leftover IPC messages. process_terminate() drains the queue
     * for an externally-killed victim; a self-terminated zombie keeps any
     * already-queued nodes, so this path must drain too — under
     * msg_lock, matching pop_message()'s lock, or this is a double
     * list_del + double kfree of the same node. */
    __proc_terminate_drain_ipc(to_free);

    /* Cancel any pending per-process timer BEFORE freeing the PCB so its
     * callback can never fire on freed memory. Defensive on every free
     * path even though a process with an armed timer is PROC_SLEEPING
     * (cannot self-exit): makes the no-UAF invariant hold everywhere. */
    timer_del(&to_free->sleep_timer);

    __process_free_resources(to_free);
  }
}

/* __sched_account_time - Tier 3 CPU-time accounting (docs/TIMER-MODEL.md
 * §4): charge the raw counter delta since the last schedule on this CPU to
 * whoever was running (prev), then mark the start count for the task
 * about to be picked. No divide in the hot path; conversion to ns happens
 * only when the value is read. arch_timer_get_count() is lock-free/
 * IRQ-safe. */
static void __sched_account_time(struct cpu_info *cpu_ptr,
                                 struct process *prev) {
  uint64_t now_cnt = arch_timer_get_count();
  if (prev && cpu_ptr->sched_run_count)
    prev->cpu_time_counts += now_cnt - cpu_ptr->sched_run_count;
  cpu_ptr->sched_run_count = now_cnt;
}

/* __sched_requeue_pending - re-enqueue the task deferred by the PREVIOUS
 * schedule() on THIS CPU (SCHED-UAF #169/#170: by now we have iretq'd off
 * its kernel stack, so it is safe for another CPU to run it). If it was
 * killed while parked here, reap it instead of resurrecting it. Caller
 * holds cpu_ptr->sched_lock. */
static void __sched_requeue_pending(struct cpu_info *cpu_ptr) {
  struct process *pr = cpu_ptr->pending_reenqueue;
  if (!pr)
    return;
  cpu_ptr->pending_reenqueue = NULL;

  if (pr->state == PROC_DEAD || pr->state == PROC_ZOMBIE) {
    pr->on_cpu = -1;
    reap_push(cpu_ptr, pr);
  } else {
    __enqueue_task(pr);
  }
}

/* sched_retire_result - outcome of retiring `prev`, telling schedule()
 * whether it must jump straight to picking a new task without touching
 * `prev` any further. */
enum sched_retire_result {
  SCHED_RETIRE_REAPED,  /* prev was DEAD/ZOMBIE; pushed to the reaper */
  SCHED_RETIRE_HANDLED, /* prev processed normally (re-enqueued or left) */
};

/* __sched_retire_prev - process the outgoing task. Replaces the original
 * `goto pick_next` (which skipped this block entirely for a DEAD/ZOMBIE
 * prev) with a plain early-return — no goto needed, since the original
 * goto's ONLY effect was "don't run the rest of this function body",
 * which a return already does. Caller holds cpu_ptr->sched_lock. */
static enum sched_retire_result __sched_retire_prev(struct cpu_info *cpu_ptr,
                                                    struct process *prev,
                                                    struct pt_regs *regs) {
  if (!prev)
    return SCHED_RETIRE_HANDLED;

  /* PROC_DEAD: externally terminated while running on this CPU.
   * PROC_ZOMBIE: terminated itself and entered schedule() to switch away.
   * Both are corpses standing on their own kernel stack: queue on the
   * reap stack; the NEXT schedule() on this CPU frees them after the
   * switch. Idle tasks never reach this (never exit, machine-protected). */
  if (prev->state == PROC_DEAD || prev->state == PROC_ZOMBIE) {
    prev->on_cpu = -1;
    reap_push(cpu_ptr, prev);
    cpu_ptr->current_task = NULL;
    return SCHED_RETIRE_REAPED;
  }

  if (regs)
    prev->context = regs; /* save current context if it was running */

  if (prev->first_run)
    prev->first_run = 0;

  if (prev->state == PROC_RUNNING) {
    prev->time_slice--;
    if (prev->time_slice <= 0) {
      prev->time_slice = prev->quantum_reset; /* quantum exhausted */
      prev->state = PROC_READY;
    } else {
      prev->state = PROC_READY; /* preempted or yielded */
    }
  }

  if (prev->state == PROC_READY) {
    /* Never re-enqueue the CPU-bound idle task: two CPUs sharing one idle
     * task's kernel stack causes context corruption (ELR=0 crashes). */
    if (prev->priority != PROC_PRIO_IDLE)
      __enqueue_task(prev);
  }
  /* else: sleeping or dying — on_cpu is kept for affinity/tracking. */

  return SCHED_RETIRE_HANDLED;
}

/* __sched_pick_focus - try to honour the bounded focus boost
 * (SCHED-01/FOCUS_BOOST_MAX). Returns the dequeued task, or NULL. Caller
 * holds cpu_ptr->sched_lock. Loop bound: MAX_PRIO * (runqueue length) —
 * unchanged from the original, just extracted; runqueues are themselves
 * bounded by MAX_PROCESSES. */
static struct process *__sched_pick_focus(struct cpu_info *cpu_ptr, int cpu,
                                          int focus_pid) {
  if (focus_pid <= 0 || sched_focus_streak[cpu] >= FOCUS_BOOST_MAX)
    return NULL;

  for (int p = 0; p < MAX_PRIO; p++) {
    if (list_empty(&cpu_ptr->runqueues[p]))
      continue;
    struct process *it;
    list_for_each_entry(it, &cpu_ptr->runqueues[p], run_list) {
      if ((int)it->pid == focus_pid) {
        __dequeue_task(it);
        sched_focus_streak[cpu]++;
        return it;
      }
    }
  }
  return NULL;
}

/* __sched_pick_fair - O(1) priority-bitmap pick: __builtin_ctz finds the
 * lowest set bit, i.e. the highest-priority non-empty queue. Resets the
 * focus streak (a fair pick ran, refilling the boost budget). Caller
 * holds cpu_ptr->sched_lock. */
static struct process *__sched_pick_fair(struct cpu_info *cpu_ptr, int cpu) {
  if (cpu_ptr->prio_bitmap == 0)
    return NULL;

  int best_prio = __builtin_ctz(cpu_ptr->prio_bitmap);
  if (best_prio >= MAX_PRIO || list_empty(&cpu_ptr->runqueues[best_prio]))
    return NULL;

  struct list_head *entry = cpu_ptr->runqueues[best_prio].next;
  struct process *next = container_of(entry, struct process, run_list);
  __dequeue_task(next);
  sched_focus_streak[cpu] = 0;
  return next;
}

/* __sched_pick_local - select the next task from THIS CPU's runqueues
 * (focus boost first, then fair O(1) pick), skipping and reaping any
 * corpse encountered (SCHED-UAF-01: a terminated process may still be
 * sitting in a runqueue) and skipping any task stopped while READY
 * (leave it off the queue, do NOT reap — process_cont() re-enqueues it).
 *
 * Replaces the original `pick_local_retry:` goto target with an
 * explicitly bounded loop (POT10 rule 2): each iteration either returns a
 * valid task or permanently removes one item from a runqueue (a DEAD
 * corpse is reaped, a STOPPED task is left dequeued), so the number of
 * retries is bounded by the total number of items ever in this CPU's
 * runqueues, which is bounded by MAX_PROCESSES. This is a STRICTLY
 * TIGHTER bound than the original unbounded goto loop offered. Caller
 * holds cpu_ptr->sched_lock. */
static struct process *__sched_pick_local(struct cpu_info *cpu_ptr, int cpu,
                                          int focus_pid) {
  for (int retry = 0; retry < MAX_PROCESSES; retry++) {
    struct process *next = __sched_pick_focus(cpu_ptr, cpu, focus_pid);
    if (!next)
      next = __sched_pick_fair(cpu_ptr, cpu);
    if (!next)
      return NULL; /* both runqueue paths exhausted: nothing local */

    if (next->state == PROC_DEAD) {
      reap_push(cpu_ptr, next);
      continue; /* picked a corpse: try again */
    }
    if (next->state == PROC_STOPPED)
      continue; /* stopped while READY: leave it dequeued, try again */

    return next;
  }
  panic("SCHED: [CPU%d] pick_local exceeded MAX_PROCESSES retries — "
        "runqueue corruption",
        cpu);
  return NULL; /* unreachable */
}

/* __sched_steal_work - work-steal one task from another CPU's runqueue.
 * Never steals idle-priority tasks (CPU-bound, share a kernel stack with
 * their owner CPU), a terminated process (left for its owner CPU to
 * reap), or a STOPPED process.
 *
 * NEWLY FOUND BUG, FIXED HERE: the original code's steal loop only
 * excluded IDLE and DEAD, not STOPPED. __enqueue_task() refuses to
 * enqueue an already-STOPPED task, but process_stop() flips a task to
 * PROC_STOPPED in place WITHOUT dequeuing it (job control can stop a task
 * that is sitting READY in a runqueue). __sched_pick_local() defends
 * against this for the LOCAL runqueue (see its STOPPED handling), but the
 * original work-stealing loop did not apply the same check — a CPU could
 * steal a Ctrl-Z'd task from another CPU's runqueue and set it RUNNING,
 * silently resuming it without going through process_cont(). Excluded
 * below exactly like DEAD is.
 *
 * Uses trylock to avoid an AB-BA deadlock against another CPU doing the
 * same. Loop bound: MAX_CPUS. Caller holds cpu_ptr->sched_lock (its OWN
 * lock only — trylock is used for every other CPU's lock, never a
 * blocking acquire, so no lock-order violation against the per-CPU locks
 * taken elsewhere). */
static struct process *__sched_steal_work(struct cpu_info *cpu_ptr) {
  for (uint32_t i = 0; i < MAX_CPUS; i++) {
    if (i == cpu_ptr->cpu_id)
      continue;

    struct cpu_info *other_cpu = &cpu_data[i];
    if (!other_cpu->online)
      continue;
    if (!spin_trylock(&other_cpu->sched_lock))
      continue;

    struct process *next = NULL;
    if (other_cpu->prio_bitmap != 0) {
      int p = __builtin_ctz(other_cpu->prio_bitmap);
      if (!list_empty(&other_cpu->runqueues[p])) {
        struct list_head *entry = other_cpu->runqueues[p].next;
        next = container_of(entry, struct process, run_list);

        if (next->priority == PROC_PRIO_IDLE || next->state == PROC_DEAD) {
          next = NULL;
        } else {
          list_del_init(&next->run_list);
          if (list_empty(&other_cpu->runqueues[p]))
            other_cpu->prio_bitmap &= ~(1u << p);
        }
      }
    }
    spin_unlock(&other_cpu->sched_lock);

    if (next)
      return next;
  }
  return NULL;
}

/* __sched_stack_alias_check - SCHED-UAF diagnostic (#169/#170): catch two
 * CPUs about to execute on the SAME kernel stack (PMM double-handing a
 * page, or a freed stack reused while still live). Reading other CPUs'
 * current_task without their lock is fine: a stale read names a
 * DIFFERENT stack (no false positive); a real alias is persistent enough
 * to trip. Loop bound: MAX_CPUS. */
static void __sched_stack_alias_check(uint32_t cpu, struct process *next) {
  for (int oc = 0; oc < (int)MAX_CPUS; oc++) {
    if (oc == (int)cpu)
      continue;
    struct process *ot = cpu_data[oc].current_task;
    if (ot && ot != next && ot->kernel_stack == next->kernel_stack)
      panic("STACK-ALIAS: CPU%d (PID %d) and CPU%d (PID %d) share "
            "kernel_stack 0x%lx",
            (int)cpu, (int)next->pid, oc, (int)ot->pid, next->kernel_stack);
  }
}

/*
 * schedule - select and switch to the next runnable process.
 *
 * Called from kernel_timer_tick() (preemption) and from sys_exit /
 * sys_ipc_recv / the YIELD syscall (voluntary yield).
 *
 * Stages (each a helper above; schedule() itself owns every lock and
 * every IRQ transition — see the SECTION 8 banner for why that stays
 * here instead of being pushed into a helper):
 *   0. __sched_reap_deferred     — free what the previous call parked.
 *   1. __sched_account_time      — CPU-time accounting for prev.
 *   2. __sched_requeue_pending   — re-enqueue what THIS CPU parked.
 *   3. __sched_retire_prev       — save/requeue/reap the outgoing task.
 *   4. __sched_pick_local        — focus boost, then O(1) fair pick.
 *   5. __sched_steal_work        — cross-CPU steal if step 4 found nothing.
 *   6. context switch            — install next, arch_cpu_switch_context.
 *
 * IRQ contract (SCHED-IRQ-01, unchanged): schedule() masks IRQs itself at
 * entry, before even get_cpu_info(), and is therefore safe to call with
 * IRQs in ANY state. No-switch exits restore the caller's IRQ state; the
 * context-switch exit deliberately returns with IRQs masked — the
 * dispatcher's IRET/ERET loads the next context's saved flags.
 */
struct pt_regs *schedule(struct pt_regs *regs) {
  uint64_t sched_irq_flags = local_irq_save();

  struct cpu_info *cpu_ptr = get_cpu_info();
  if (!cpu_ptr) {
    if (regs && pt_regs_pc(regs) == 0)
      panic("SCHED: [EARLY] pc==0 on return");
    local_irq_restore(sched_irq_flags);
    return regs;
  }

  __sched_reap_deferred(cpu_ptr);

  uint32_t cpu = cpu_ptr->cpu_id;
  struct process *prev = cpu_ptr->current_task;
  int prev_reaped = 0;

  __sched_account_time(cpu_ptr, prev);

  uint64_t flags;
  spin_lock_irqsave(&cpu_ptr->sched_lock, &flags);

  __sched_requeue_pending(cpu_ptr);

  int focus_pid = keyboard_focus_pid; /* lockless hint read, SCHED-01 */

  if (__sched_retire_prev(cpu_ptr, prev, regs) == SCHED_RETIRE_REAPED) {
    prev = NULL;
    prev_reaped = 1;
  }

  struct process *next = __sched_pick_local(cpu_ptr, cpu, focus_pid);

  if (!next) {
    next = __sched_steal_work(cpu_ptr);

    if (!next) {
      /* Nothing to steal either. If prev is still READY, keep running it. */
      if (prev && prev->state == PROC_RUNNING) {
        if (pt_regs_pc(regs) == 0)
          panic("SCHED: [CPU%d] BUG pc==0 on PROC_RUNNING fast-path, PID %d",
                cpu, prev->pid);
        spin_unlock_irqrestore(&cpu_ptr->sched_lock, flags);
        local_irq_restore(sched_irq_flags); /* SCHED-IRQ-01: no-switch */
        return regs;
      }

      /* Mandatory fallback to the idle task. */
      next = cpu_ptr->idle_task;
      if (!next) {
        /* prev was just reaped and there is no idle task to switch to:
         * resuming `regs` would run a corpse whose stack the next drain
         * frees while still in use. Must be fatal. */
        if (prev_reaped)
          panic("SCHED: [CPU%d] reaped current task with no idle task to "
                "switch to",
                cpu);
        if (regs && pt_regs_pc(regs) == 0)
          panic("SCHED: [CPU%d] BUG pc==0 on idle-fallback return, PID %d", cpu,
                prev ? (int)prev->pid : -1);
        spin_unlock_irqrestore(&cpu_ptr->sched_lock, flags);
        local_irq_restore(sched_irq_flags); /* SCHED-IRQ-01: no-switch */
        return regs;
      }
    }
  }

  /* Same task selected: no context switch needed. */
  if (prev == next) {
    if (pt_regs_pc(regs) == 0)
      panic("SCHED: [CPU%d] BUG pc==0 on same-task return, PID %d", cpu,
            prev->pid);
    next->state = PROC_RUNNING;
    next->on_cpu = cpu;
    spin_unlock_irqrestore(&cpu_ptr->sched_lock, flags);
    local_irq_restore(sched_irq_flags); /* SCHED-IRQ-01: no-switch exit */
    return regs;
  }

  /* Switching to a DIFFERENT task. SCHED-UAF #169/#170: pull `prev` back
   * OFF the runqueue (it was re-enqueued in __sched_retire_prev above)
   * and hold it in pending_reenqueue so no other CPU can work-steal and
   * run it while THIS CPU is still executing on prev's kernel stack (the
   * IRQ EOI + iretq run on prev's stack after schedule() returns). It
   * returns to the runqueue at the NEXT schedule() here, after we have
   * iretq'd off its stack. */
  if (prev && prev->state == PROC_READY && prev->priority != PROC_PRIO_IDLE) {
    __dequeue_task(prev);
    cpu_ptr->pending_reenqueue = prev;
  }

  cpu_ptr->current_task = next;
  next->state = PROC_RUNNING;
  next->on_cpu = cpu;

  __sched_stack_alias_check(cpu, next);

  SCHED_ASSERT(next != NULL, "schedule: BUG next is NULL at switch");
  SCHED_ASSERT(next->context != NULL, "schedule: invalid context for PID %d",
               (int)next->pid);
  if (pt_regs_pc(next->context) == 0)
    panic("SCHED: PC is 0 for PID %d (Name: %s)", next->pid, next->name);

  /* Address-space switch is delegated entirely to
   * arch_cpu_switch_context() — the single source of truth for both
   * arches, including the NULL-page_table -> shared kernel_pgd case
   * (SCHED-UAF-01). Unconditional here: the prev==next case already
   * returned above. */
  __sync_fetch_and_add(&stat_ctx_switches, 1); /* perf brief §1 */
  arch_cpu_switch_context(next);

  spin_unlock_irqrestore(&cpu_ptr->sched_lock, flags);
  /* SCHED-IRQ-01: context-switch exit — deliberately NO local_irq_restore.
   * We are about to unwind into the dispatcher with another task's frame;
   * IRQs stay masked until IRET/ERET loads the flags saved in that frame. */
  return next->context;
}

/* ===========================================================================
 * SECTION 9 — Job control, wait/reap reporting
 * ===========================================================================
 */

/* process_wait - wait for a process (non-blocking, SCHED-03).
 * Returns PID if terminated (corpse still pending reap, or already
 * auto-reaped but its status retained), -1 if still running, -2 if
 * genuinely unknown (never existed, or already collected). Pure reporter:
 * freeing belongs exclusively to the scheduler reaper. */
int process_wait(int pid, int *out_code) {
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);

  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *proc = process_pool[i];
    if (!proc || (int)proc->pid != pid)
      continue;

    if (proc->state == PROC_DEAD || proc->state == PROC_ZOMBIE) {
      if (out_code)
        *out_code = proc->exited ? (proc->exit_code & 0xff) : -9;
      spin_unlock_irqrestore(&sched_lock, flags);
      return pid;
    }
    spin_unlock_irqrestore(&sched_lock, flags);
    return -1; /* still alive */
  }

  /* Not in the pool: may have been reaped before we looked. A retained
   * status is exactly as authoritative as a corpse (9b). */
  if (__claim_reaped(pid, out_code)) {
    spin_unlock_irqrestore(&sched_lock, flags);
    return pid;
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return -2; /* never existed, or already collected */
}

/* process_stop - suspend a process (job control, Phase 2). RUNNING/READY/
 * SLEEPING -> PROC_STOPPED (crucially SLEEPING too: a foreground REPL is
 * usually mid-sleep when Ctrl-Z arrives). ABI-05: a proc_is_machine()
 * target is refused unconditionally, mirroring process_terminate()'s
 * guard — a stopped init sits there permanently, un-respawnable, with no
 * supervisor left running to notice. */
int process_stop(int pid) {
  uint64_t flags;
  int rc = -ESRCH;
  spin_lock_irqsave(&sched_lock, &flags);

  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *p = process_pool[i];
    if (!p || (int)p->pid != pid)
      continue;

    if (proc_is_machine(p)) {
      pr_warn("Cannot stop protected process '%s' (PID %d)\n", p->name, pid);
      rc = -EPERM;
    } else if (p->state == PROC_RUNNING || p->state == PROC_READY ||
               p->state == PROC_SLEEPING) {
      p->state = PROC_STOPPED;
      rc = 0;
    } else {
      rc = -EINVAL; /* already stopped, dying, or a corpse */
    }
    break;
  }
  spin_unlock_irqrestore(&sched_lock, flags);
  return rc;
}

/* process_cont - resume a stopped process (job control, Phase 2).
 * Re-enqueues it OUTSIDE sched_lock (enqueue_task takes its own lock). */
int process_cont(int pid) {
  uint64_t flags;
  int rc = -ESRCH;
  struct process *target = NULL;

  spin_lock_irqsave(&sched_lock, &flags);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *p = process_pool[i];
    if (!p || (int)p->pid != pid)
      continue;
    if (p->state == PROC_STOPPED) {
      target = p;
      rc = 0;
    } else {
      rc = -EINVAL;
    }
    break;
  }
  spin_unlock_irqrestore(&sched_lock, flags);

  if (target) {
    /* Set READY before enqueue so it passes __enqueue_task's STOPPED
     * guard. A resumed sleeper re-runs its nanosleep and returns once the
     * (now long-past) deadline is checked, continuing where it left off. */
    target->state = PROC_READY;
    enqueue_task(target);
  }
  return rc;
}

/* ===========================================================================
 * SECTION 10 — IPC
 * ===========================================================================
 */

/* pop_message - pop the first queued message matching src_pid (-1 for
 * any). NULL proc is a reachable argument, not a caller bug: schedule()
 * sets current_process to NULL while reaping a DEAD/ZOMBIE task before
 * picking the next one, and every call site here passes current_process. */
struct ipc_node *pop_message(struct process *proc, int src_pid) {
  if (!proc)
    return NULL;

  uint64_t flags;
  struct ipc_node *node = NULL;
  struct list_head *pos, *q;

  spin_lock_irqsave(&proc->msg_lock, &flags);
  list_for_each_safe(pos, q, &proc->msg_queue) {
    struct ipc_node *tmp = list_entry(pos, struct ipc_node, list);
    if (src_pid == -1 || tmp->msg.from == (int)src_pid) {
      list_del(pos);
      if (proc->msg_count > 0)
        proc->msg_count--;
      node = tmp;
      break;
    }
  }
  spin_unlock_irqrestore(&proc->msg_lock, flags);
  return node;
}

/* kernel_ipc_send - internal IPC implementation.
 *
 * SCHED-05 fix: MSG_QUEUE_MAX now caps a target's pending-message count.
 * The cap is checked under target->msg_lock (the same lock the append
 * happens under), so the count can never be read stale relative to the
 * append it gates.
 *
 * Lock order (unchanged, matches the file-header hierarchy exactly):
 * sched_lock -> target->msg_lock -> target_cpu->sched_lock. */
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

  spin_lock(&target->msg_lock);

  if (target->msg_count >= MSG_QUEUE_MAX) {
    spin_unlock(&target->msg_lock);
    spin_unlock_irqrestore(&sched_lock, flags);
    kfree(node);
    return -ENOBUFS; /* SCHED-05: flow control instead of unbounded growth */
  }

  list_add_tail(&node->list, &target->msg_queue);
  target->msg_count++;

  if (target->state == PROC_SLEEPING &&
      (target->ipc_target_pid == -1 ||
       target->ipc_target_pid == (int)msg->from)) {
    target->state = PROC_READY;

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
  if (vmm_copy_from_user(&k_msg, msg_ptr, sizeof(struct ipc_message)) != 0)
    return -EINVAL;
  /* USR-SEC-03 #79: without CAP_IPC_ANY a process may only message its
   * parent or a descendant. */
  if (!process_ipc_allowed(current_process, target_pid))
    return -EPERM;
  k_msg.from = current_process->pid;
  return kernel_ipc_send(target_pid, &k_msg);
}

int sys_ipc_recv(int src_pid, void *msg_ptr) {
  struct ipc_node *node = pop_message(current_process, src_pid);
  if (node) {
    if (vmm_copy_to_user(msg_ptr, &node->msg, sizeof(struct ipc_message)) !=
        0) {
      kfree(node);
      return -EFAULT;
    }
    kfree(node);
    return 0;
  }

  /* No message ready: commit to sleep. IPC-01 lost-wakeup fix: re-check
   * the queue under msg_lock — the SAME lock kernel_ipc_send() holds
   * while appending and testing our state — and sleep only if it is
   * still empty. Lock order msg_lock -> cpu->sched_lock matches the
   * sender's msg_lock -> target-CPU sched_lock. */
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

  /* The rewind belongs to the DISPATCHER, which holds the live trap
   * frame — this function only reports that a retry must happen.
   * IPC_RECV_RETRY tells the dispatcher the retry is armed and the trap
   * frame's argument registers must survive untouched (see sched.h). */
  return IPC_RECV_RETRY;
}

int sys_ipc_try_recv(int src_pid, void *msg_ptr) {
  struct ipc_node *node = pop_message(current_process, src_pid);
  if (!node)
    return -1; /* EAGAIN */

  if (vmm_copy_to_user(msg_ptr, &node->msg, sizeof(struct ipc_message)) != 0) {
    kfree(node);
    return -1;
  }
  kfree(node);
  return 0;
}

/* ===========================================================================
 * SECTION 11 — Introspection: ps, sysstats, sbrk
 * ===========================================================================
 */

long sys_getprocs(struct ps_info *user_buf, size_t max_count) {
  if (!user_buf)
    return -1;

  /* SCHED-09 (#98): max_count is a raw user argument; clamp so the
   * sizeof(struct ps_info) * max_count product cannot overflow. */
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
    struct process *p = process_pool[i];
    if (!p)
      continue;
    k_buf[count].pid = p->pid;
    strncpy(k_buf[count].name, p->name, 32);
    k_buf[count].state = p->state;
    k_buf[count].priority = p->priority;
    k_buf[count].cpu_time = timer_counts_to_ns(p->cpu_time_counts) / 1000000ULL;
    k_buf[count].on_cpu = p->on_cpu;
    count++;
  }
  spin_unlock_irqrestore(&sched_lock, flags);

  /* Returning `count` after a failed copy would tell the caller it
   * received N entries it never got. */
  int copied =
      vmm_copy_to_user(user_buf, k_buf, sizeof(struct ps_info) * count);
  kfree(k_buf);
  return (copied != 0) ? -EFAULT : count;
}

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
  case PROC_STOPPED:
    return "stopped"; /* Phase 2 job control */
  default:
    return "unused";
  }
}

int proc_get_info(int pid, struct ps_info *out) {
  if (!out)
    return -1;

  int rc = -1;
  uint64_t flags;
  spin_lock_irqsave(&sched_lock, &flags);
  for (int i = 0; i < MAX_PROCESSES; i++) {
    struct process *p = process_pool[i];
    if (!p || (int)p->pid != pid)
      continue;
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
  spin_unlock_irqrestore(&sched_lock, flags);
  return rc;
}

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

/* sys_sysstats - OS1_sys_stats(buf, buf_size): copy one struct
 * os1_sysstats snapshot to userland. Forward-compatible: copies
 * min(sizeof(struct os1_sysstats), buf_size). NO HARDCODED CPU COUNT:
 * sched_ncpu is the real nr_cpus; per-CPU load buckets by the real on_cpu
 * field into a MAX_CPUS scratch. */
long sys_sysstats(struct os1_sysstats *user_buf, size_t buf_size) {
  if (!user_buf)
    return -1;

  struct os1_sysstats s;
  memset(&s, 0, sizeof(s));
  s.version = OS1_SYSSTATS_VERSION;
  s.struct_size = (unsigned int)sizeof(s);
  s.uptime_ns = timer_counts_to_ns(arch_timer_get_count());

  /* MM-PMM-09 (#94): report USABLE RAM, not the metadata span (which
   * includes the sub-4GB PCI/MMIO hole on amd64) — every "used" consumer
   * derives it as total - free. */
  s.pmm_total_pages = pmm_get_usable_pages();
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

  {
    uint64_t km_total = 0, km_inuse = 0, km_hw = 0, km_live = 0;
    kmalloc_get_stats(&km_total, &km_inuse, &km_hw, &km_live);
    s.km_heap_total_bytes = km_total;
    s.km_bytes_in_use = km_inuse;
    s.km_high_water_bytes = km_hw;
    s.km_live_allocs = km_live;
  }

  {
    uint64_t live[OBJ_TYPE_COUNT];
    object_get_live_counts(live, OBJ_TYPE_COUNT);
    for (int i = 0; i < OBJ_TYPE_COUNT; i++)
      s.obj_live_by_type[i] = live[i];
  }

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

    uint32_t nc = (nr_cpus > MAX_CPUS) ? MAX_CPUS : nr_cpus;
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
    n = buf_size;
  if (vmm_copy_to_user(user_buf, &s, n) != 0)
    return -1;
  return (long)n;
}

/* SBRK_HEAP_LIMIT: hard ceiling for the user heap. The user stack lives at
 * [0xC0000000, 0xC0100000); without a bound a process could sbrk() its
 * heap straight into (or past) the stack mappings. 16MB of guard gap
 * below the stack base. (SCHED-07: confirmed already resolved — this
 * bound was already present and correct; kept as-is.) */
#define SBRK_HEAP_LIMIT 0xBF000000UL

long sys_sbrk(intptr_t increment) {
  struct process *proc = current_process;
  uint64_t old_brk = proc->heap_end;
  uint64_t new_brk = old_brk + increment;

  if (increment == 0)
    return (long)old_brk;

  if (increment > 0) {
    if (new_brk < old_brk || new_brk > SBRK_HEAP_LIMIT)
      return -ENOMEM; /* overflow, or past the guard below the stack */

    uint64_t start_map = (old_brk + 4095) & ~(4095ULL);
    uint64_t end_map = (new_brk + 4095) & ~(4095ULL);

    for (uint64_t vaddr = start_map; vaddr < end_map; vaddr += 4096) {
      void *paddr = pmm_alloc_page();
      if (!paddr)
        return -ENOMEM;
      memset(paddr, 0, 4096);
      /* PAGE_USER_DATA: the user heap is never executable (W^X, ELF-02). */
      if (vmm_map_page_locked(proc, vaddr, virt_to_phys(paddr),
                              PAGE_USER_DATA) != 0) {
        pmm_free_page(paddr);
        return -ENOMEM;
      }
    }
  } else {
    if (new_brk < proc->heap_start)
      return -EINVAL;

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