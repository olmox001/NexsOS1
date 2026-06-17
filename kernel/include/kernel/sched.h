#ifndef _KERNEL_SCHED_H
#define _KERNEL_SCHED_H

#include <drivers/timer.h>
#include <kernel/fd.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <stdint.h>
#pragma GCC optimize("O2")

#define PROCESS_NAME_MAX 32
#define STACK_SIZE 131072
#define MAX_PRIO 32

/* Process Info for Diagnostics */
struct ps_info {
  int pid;
  char name[32];
  int state;
  int priority;
  uint64_t cpu_time;
  int on_cpu;
};

struct ipc_node {
  struct ipc_message msg;
  struct list_head list;
};

/* Architecture-specific register frame layout */
#include <arch/pt_regs.h>

#define DEFAULT_QUANTUM 10

/* Wait Queue */
struct wait_queue_head {
  struct list_head task_list;
  spinlock_t lock;
};

/* Process Control Block */
struct process {
  uint32_t pid;
  char name[PROCESS_NAME_MAX];

  /* Memory */
  uint64_t *page_table;  /* Physical address of TTBR0_EL1 */
  uint64_t kernel_stack; /* Kernel stack top */
  uint64_t heap_start;   /* Base address of user heap */
  uint64_t heap_end;     /* Current end of user heap */
  spinlock_t mm_lock;    /* Protects page table modifications */

  /* Context for switching */
  struct pt_regs *context;

  /* User Context (Initial) */
  uint64_t user_entry;
  uint64_t user_stack;

  /* State, Priority and Permissions */
  int state;
  uint8_t first_run; /* 1 if never scheduled before (ELF context intact) */
  int priority;      /* 0 (High) - 31 (Low) */
  int time_slice;    /* Ticks remaining */
  int quantum_reset; /* Reset value */

  uint8_t level;  /* privilege level (PLVL_*) — see the capability model below */
  uint32_t caps;  /* capability mask (CAP_*); machine level bypasses checks */
  /* ctty_win: controlling-terminal window (USR-TTY-01 #123).  Inherited from
   * the spawner at process_create: the launching shell's window.  A process
   * with NO window of its own (a POSIX-like CLI tool) writes stdout here, so
   * it runs "in the shell"; a process that opens its own window renders there
   * instead (own-window-first in sys_write).  -1 = none. */
  int ctty_win;
  /* parent_pid: PID of the process that spawned this one (0 = kernel/boot).
   * Set by process_create() from current_process; used by the SYS_KILL
   * capability check (ABI-04): a process may kill itself or any descendant,
   * and a privileged (machine/root) process may kill anything. */
  int parent_pid;
  /* child_count: live (not yet reaped) children of this process
   * (SCHED-DOS-01 #122).  Incremented by process_create() on the creator,
   * decremented when a child's pool slot is released (terminate immediate
   * free / scheduler reap).  Protected by sched_lock. */
  int child_count;

  /* Scheduler List */
  struct list_head run_list;
  struct process *next; /* Legacy Linked list (remove later?) */

  /* Wait Queue (for sleeping) */
  struct wait_queue_head *wait_queue_ptr;
  struct wait_queue_head wait_queue;

  /* Tier 3 timed sleep (docs/TIMER-MODEL.md §4): a per-process software timer,
   * armed on the running core, wakes the process at the ABSOLUTE real-time
   * deadline wake_ns (mono_ns base). The wheel fires on a coarse jiffies edge;
   * the fine wake condition is mono_ns() >= wake_ns, so the process wakes at the
   * right wall-clock instant even if ticks were dropped. 0 = not sleeping.
   * cpu_time_counts: CPU time consumed by this process, accumulated in RAW
   * hardware-counter units across context switches (a cheap subtraction in the
   * scheduler hot path — no divide). Converted to ns only on read via
   * timer_counts_to_ns() for os1_cpu_ns()/ps_info. */
  uint64_t wake_ns;
  uint64_t cpu_time_counts;
  struct timer sleep_timer;

  /* Yield-throttle anti-spin (per-process, docs/TIMER-MODEL.md §4): yield()
   * calls within the current tick are counted in yield_count (for the tick
   * yield_jiffy). Beyond YIELD_SPIN_BUDGET the process is busy-spinning on
   * yield() rather than doing work and is put to sleep until the next tick via
   * sleep_timer, so an unoptimised program cannot keep a core at 100% and freeze
   * the system. */
  uint64_t yield_jiffy;
  uint32_t yield_count;

  /* IPC state */
  int ipc_target_pid; /* PID we want to talk to (-1 for ANY) */
  struct ipc_message
      *ipc_msg; /* User-space pointer to message (for synchronous RECV) */
  struct list_head msg_queue; /* Buffered incoming messages */
  spinlock_t msg_lock;        /* Message queue protection */

  /* SMP state */
  int on_cpu; /* CPU ID running this process, -1 if none */

  /* Filesystem state */
  char cwd[128]; /* Current Working Directory */

  /* File-descriptor table (ABI-03, kernel/fd.h).  0/1/2 pre-opened by
   * process_create(); entries hold no kernel-owned resources, so teardown
   * needs no cleanup pass.  Touched only by the owning process from
   * syscall context — no lock. */
  struct fd_entry fds[NPROC_FDS];
};

/* Process States */
#define PROC_UNUSED 0
#define PROC_CREATED 1
#define PROC_RUNNING 2
#define PROC_SLEEPING 3
#define PROC_ZOMBIE 4
#define PROC_DEAD 5
#define PROC_READY 6

/* Process Priorities */
#define PROC_PRIO_SYSTEM 0 /* Kernel-level service */
#define PROC_PRIO_ROOT 1   /* Root shells/services */
#define PROC_PRIO_USER 2   /* User applications */
#define PROC_PRIO_IDLE 31  /* Idle task */

/* Capability & privilege-level model (B3, USR-SEC-03 #79).  Levels (PLVL_*)
 * and capability bits (CAP_*) live in the shared api header so the kernel and
 * userland cannot drift. */
#include <caps.h>

/* Capability helpers.  A NULL process is the kernel-internal context and is
 * treated as fully privileged, matching the historical bypass. */
static inline int proc_is_machine(const struct process *p) {
  return !p || p->level == PLVL_MACHINE;
}
static inline int proc_is_privileged(const struct process *p) {
  return !p || p->level <= PLVL_ROOT;
}
static inline int proc_has_cap(const struct process *p, uint32_t cap) {
  return !p || p->level == PLVL_MACHINE || (p->caps & cap) == cap;
}

#define MAX_PROCESSES 128
extern struct process *process_pool[MAX_PROCESSES];

/* SCHED-DOS-01 (#122): anti fork-bomb quotas.  MAX_PROCESSES is only the
 * pool ARRAY bound; the effective limit is derived from usable memory at
 * process_init() (see proc_limit in process.c) and these gates keep a
 * single unprivileged process from exhausting it:
 *   MAX_PROCS_PER_PARENT  live-children quota for non-SYSTEM/ROOT creators;
 *   RESERVED_PROC_SLOTS   slots only SYSTEM/ROOT creators may dig into, so
 *                         kill/respawn recovery stays possible at saturation. */
#define MAX_PROCS_PER_PARENT 32
#define RESERVED_PROC_SLOTS 8

/* API */
#include <kernel/cpu.h>
#define current_process (get_cpu_info()->current_task)

/* process_create: spawn at 'level' with that level's default capability
 * preset, clamped monotonically against the creator (see process.c).
 * process_create_caps: same, with an explicit requested capability mask
 * (used by SYS_SPAWN_CAPS) — also clamped to the level ceiling and the
 * creator's caps. */
extern struct process *process_create(const char *name, uint8_t priority,
                                      uint8_t level);
extern struct process *process_create_caps(const char *name, uint8_t priority,
                                           uint8_t level, uint32_t req_caps);
struct process *process_find_by_pid(int pid);
struct process *__process_find_by_pid(int pid);
/* process_kill_allowed: ABI-04 capability check for SYS_KILL.  Returns
 * non-zero if 'caller' may terminate 'target_pid': itself, any descendant,
 * or anything when it is privileged (machine/root).  Kernel-internal
 * terminate paths (compositor close, init supervision) bypass this and call
 * process_terminate() directly. */
int process_kill_allowed(struct process *caller, int target_pid);
/* process_ipc_allowed: may 'caller' send IPC to target_pid?  True if the
 * caller holds CAP_IPC_ANY, or target is the caller's parent or a
 * descendant.  Acquires sched_lock internally. */
int process_ipc_allowed(struct process *caller, int target_pid);
int process_terminate(int pid);
int process_wait(
    int pid); /* Wait for process, returns status or -1 if active */
extern int process_load_elf(struct process *proc, const char *path);
/* Like process_load_elf(), but marshals an argv vector onto the new task's
 * stack and seeds main()'s argc/argv (kargv holds kernel-side string copies;
 * pass argc=0/kargv=NULL for no arguments). */
extern int process_load_elf_args(struct process *proc, const char *path,
                                 int argc, char *const kargv[]);
void start_user_process(struct process *proc);
void process_init(void);
struct pt_regs *schedule(struct pt_regs *regs);

/* Exception Handlers */
struct pt_regs *syscall_handler(struct pt_regs *frame);
struct pt_regs *irq_handler(struct pt_regs *frame);
struct pt_regs *sync_handler(struct pt_regs *frame);

/* API Declarations */
void enqueue_task(struct process *p);
void sleep_on(struct wait_queue_head *wq);
void wake_up(struct wait_queue_head *wq);

/* Core Tasks */
void idle_task_entry(void);

/* Syscalls */
int sys_ipc_send(int target_pid, void *msg_ptr);
/* sys_ipc_recv returns IPC_RECV_RETRY when it annotated a syscall retry
 * (blocked, or a message slipped in during the sleep window).  The
 * dispatcher must NOT write a return value in that case: on aarch64 x0 is
 * both the return register and arg0, so writing it clobbers src_pid for
 * the re-executed SVC (the receiver re-armed with src_pid=0 and slept
 * forever on a non-empty queue — the visible half of IPC-01). */
#define IPC_RECV_RETRY 1
int sys_ipc_recv(int src_pid, void *msg_ptr);
int sys_ipc_try_recv(int src_pid, void *msg_ptr);
int kernel_ipc_send(int target_pid, struct ipc_message *msg);
struct ipc_node *pop_message(struct process *proc, int src_pid);
/* keyboard_focus_pid: scheduler-owned focus HINT (which PID keystrokes route to
 * and the schedule() focus boost favours). MUTATE ONLY via sched_set_focus_pid()
 * — the compositor and SYS_SET_FOCUS PUSH changes down to the scheduler
 * (GFX-COMP-01 #67); the compositor no longer writes this global directly. It is
 * a single int, so accesses are atomic and reads are lockless: it is a hint,
 * never load-bearing for correctness. Read via sched_get_focus_pid() (snapshot)
 * outside the compositor's own render path. */
extern int keyboard_focus_pid;
void sched_set_focus_pid(int pid);
int sched_get_focus_pid(void);
long sys_getprocs(struct ps_info *user_buf, size_t max_count);
long sys_sbrk(intptr_t increment);

#endif
