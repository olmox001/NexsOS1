#ifndef _KERNEL_SCHED_H
#define _KERNEL_SCHED_H

#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <stdint.h>
#pragma GCC optimize("O2")

#define PROCESS_NAME_MAX 32
#define STACK_SIZE 65536
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

struct pt_regs;
#define DEFAULT_QUANTUM 10

/* Wait Queue */
struct wait_queue_head {
  struct list_head task_list;
  spinlock_t lock;
};

/* Task Context (Saved Registers) */
/* Full Register State (Matching stack layout in exception.S) */
struct pt_regs {
  uint64_t regs[31]; /* x0-x30 */
  uint64_t unused;   /* Padding to align to 16 bytes (32 * 8 = 256) */
  uint64_t elr;      /* 256 */
  uint64_t spsr;     /* 264 */
  uint64_t sp_el0;   /* 272 */
  uint64_t padding;  /* 280 */

  /* FPU/SIMD State (NEON q0-q31 = 32 * 16 bytes = 512 bytes) */
  __uint128_t qregs[32]; /* 288 - 800 */
  uint32_t fpsr;         /* 800 */
  uint32_t fpcr;         /* 804 */
  uint64_t fpu_padding;  /* 808-816 */
};

/* Process Control Block */
struct process {
  uint32_t pid;
  char name[PROCESS_NAME_MAX];

  /* Memory */
  uint64_t *page_table;  /* Physical address of TTBR0_EL1 */
  uint64_t kernel_stack; /* Kernel stack top */
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

  uint32_t permissions;

  /* Scheduler List */
  struct list_head run_list;
  struct process *next; /* Legacy Linked list (remove later?) */

  /* Wait Queue (for sleeping) */
  struct wait_queue_head *wait_queue_ptr;
  struct wait_queue_head wait_queue;

  /* IPC state */
  int ipc_target_pid; /* PID we want to talk to (-1 for ANY) */
  struct ipc_message
      *ipc_msg; /* User-space pointer to message (for synchronous RECV) */
  struct list_head msg_queue; /* Buffered incoming messages */
  spinlock_t msg_lock;        /* Message queue protection */

  /* SMP state */
  int on_cpu; /* CPU ID running this process, -1 if none */
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

/* Process Permissions */
#define PROC_PERM_SYSTEM (1 << 0) /* Cannot be killed, has kernel access */
#define PROC_PERM_ROOT (1 << 1)   /* Can spawn and kill other processes */
#define PROC_PERM_USER (1 << 2)   /* Standard user app permissions */

#define MAX_PROCESSES 32
extern struct process *process_pool[MAX_PROCESSES];

/* API */
#include <kernel/cpu.h>
#define current_process (get_cpu_info()->current_task)

extern struct process *process_create(const char *name, uint8_t priority,
                                      uint32_t permissions);
struct process *process_find_by_pid(int pid);
int process_terminate(int pid);
int process_wait(
    int pid); /* Wait for process, returns status or -1 if active */
extern int process_load_elf(struct process *proc, const char *path);
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
int sys_ipc_recv(int src_pid, void *msg_ptr);
int sys_ipc_try_recv(int src_pid, void *msg_ptr);
int kernel_ipc_send(int target_pid, struct ipc_message *msg);
struct ipc_node *pop_message(struct process *proc, int src_pid);
extern int keyboard_focus_pid;
long sys_getprocs(struct ps_info *user_buf, size_t max_count);

#endif
