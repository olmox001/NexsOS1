/*
 * kernel/arch/aarch64/cpu/syscall.c
 * System Call Handler
 */
#include <drivers/uart.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/ext4.h>
#include <kernel/graphics.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>
#include <kernel/registry.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <stdint.h>

extern volatile uint64_t jiffies;
extern struct pt_regs *schedule(struct pt_regs *regs);
extern int process_terminate(int pid);

extern int compositor_get_window_by_pid(int pid);
extern void compositor_window_write(int win_id, const char *buf, size_t count);
extern void compositor_blit(int win_id, int x, int y, int w, int h,
                            const uint32_t *buf, int pid);
extern int compositor_create_window(int x, int y, int w, int h,
                                    const char *title, int pid);
extern void compositor_draw_rect(int window_id, int x, int y, int w, int h,
                                 uint32_t color, int caller_pid);
extern void compositor_render(void);
extern void compositor_set_window_flags(int window_id, int flags);

/* Secure memory access helpers with Page Table Switching */
int copy_from_user(void *dest, const void *src, size_t n) {
  if (!vmm_is_user_addr((uint64_t)src) || !vmm_is_user_addr((uint64_t)src + n))
    return -1;

  if (!current_process || !current_process->page_table)
    return -1;

  /* Save and disable interrupts to prevent scheduler preemption */
  uint64_t flagsptr = local_irq_save();

  /* Lock the address space for this process */
  spin_lock(&current_process->mm_lock);

  /* Save kernel TTBR0 (usually 0 or points to identity map initially) */
  uint64_t old_ttbr0 = arch_get_ttbr0();

  /* Switch to user's page table (must use physical address) */
  arch_set_ttbr0(virt_to_phys(current_process->page_table));
  arch_tlb_flush_all();
  arch_isb();

  /* Perform copy while user space is mapped at TTBR0 */
  memcpy(dest, src, n);

  /* Restore kernel/previous TTBR0 */
  arch_set_ttbr0(old_ttbr0);
  arch_tlb_flush_all();
  arch_isb();

  spin_unlock(&current_process->mm_lock);
  local_irq_restore(flagsptr);

  return 0;
}

int copy_to_user(void *dest, const void *src, size_t n) {
  if (!vmm_is_user_addr((uint64_t)dest) ||
      !vmm_is_user_addr((uint64_t)dest + n))
    return -1;

  if (!current_process || !current_process->page_table)
    return -1;

  uint64_t flagsptr = local_irq_save();
  spin_lock(&current_process->mm_lock);

  uint64_t old_ttbr0 = arch_get_ttbr0();
  arch_set_ttbr0(virt_to_phys(current_process->page_table));
  arch_tlb_flush_all();
  arch_isb();

  memcpy(dest, src, n);

  arch_set_ttbr0(old_ttbr0);
  arch_tlb_flush_all();
  arch_isb();

  spin_unlock(&current_process->mm_lock);
  local_irq_restore(flagsptr);

  return 0;
}

/* Copy null-terminated string from user space safely with Page Table Switching
 */
int copy_string_from_user(char *dest, const char *src, size_t max_len) {
  if (!vmm_is_user_addr((uint64_t)src))
    return -1;

  if (!current_process || !current_process->page_table)
    return -1;

  uint64_t flagsptr = local_irq_save();
  spin_lock(&current_process->mm_lock);

  uint64_t old_ttbr0 = arch_get_ttbr0();
  arch_set_ttbr0(virt_to_phys(current_process->page_table));
  arch_tlb_flush_all();
  arch_isb();

  int ret = 0;
  size_t i;
  for (i = 0; i < max_len - 1; i++) {
    /* Potential optimization: check page boundaries if max_len is large */
    dest[i] = src[i];
    if (src[i] == '\0')
      goto out;
  }
  dest[max_len - 1] = '\0';

out:
  arch_set_ttbr0(old_ttbr0);
  arch_tlb_flush_all();
  arch_isb();
  spin_unlock(&current_process->mm_lock);
  local_irq_restore(flagsptr);
  return ret;
}

extern uint64_t timer_get_us(void);
static long sys_get_time(void) { return (long)(timer_get_us() / 1000); }

/* Syscall Implementations */
long sys_write(int fd, const char *buf, size_t count);
struct pt_regs *sys_read(struct pt_regs *regs);
long sys_get_pid(void);
void sys_exit(int status);

extern int keyboard_read_char_nonblock(void);

long sys_get_pid(void) {
  return current_process ? (long)current_process->pid : 0;
}

struct pt_regs *sys_read(struct pt_regs *regs) {
  int fd = (int)regs->regs[0];
  char *buf = (char *)regs->regs[1];
  size_t count = (size_t)regs->regs[2];

  /* pr_err("sys_read: fd=%d count=%lu\n", fd, count); */

  if (fd != 0 || count == 0) {
    regs->regs[0] = 0;
    return regs;
  }

  /*
   * Microkernel/Tanenbaum Style: Redirect stdin to IPC queue
   * Standard reads from fd 0 now look for IPC_TYPE_INPUT messages.
   */
  extern struct ipc_node *pop_message(struct process * proc, int src_pid);

  /* Try to pop an input message */
  struct ipc_node *node = NULL;
  uint64_t flags;
  struct cpu_info *cpu = get_cpu_info();

  /* Atomic check for message */
  node = pop_message(current_process, -1); /* From ANY */

  if (node) {
    if (node->msg.type == IPC_TYPE_INPUT) {
      char c = (char)node->msg.data1;
      if (copy_to_user(buf, &c, 1) != 0) {
        /* pr_err("%s", "sys_read: copy_to_user failed\n"); */
      }
      regs->regs[0] = 1;
      kfree(node);
      return regs;
    }
    /* If it wasn't input, put it back or drop?
     * Ideally we should filter in pop_message or use a separate queue.
     * For now, if it's not input, we drop it (should not happen for fd 0).
     */
    kfree(node);
  }

  /* Block until input available via IPC wake-up logic already in
   * kernel_ipc_send */
  spin_lock_irqsave(&cpu->sched_lock, &flags);
  current_process->ipc_target_pid = -1; /* Waiting for ANY (input) */
  current_process->state = PROC_SLEEPING;
  spin_unlock_irqrestore(&cpu->sched_lock, flags);

  regs->elr -= 4; /* Re-execute SVC */
  return schedule(regs);
}

long sys_write(int fd, const char *buf, size_t count) {
  if (count == 0)
    return 0;
  struct cpu_info *cpu = get_cpu_info();
  char *k_buf = cpu->syscall_buf;
  size_t to_copy = (count > 1024) ? 1024 : count;
  /* pr_err("sys_write: fd=%d count=%lu\n", fd, count); */

  if (copy_from_user(k_buf, buf, to_copy) != 0)
    return -1;
  k_buf[to_copy] = '\0';

  if (fd >= 100) { /* Handle window ID terminal output */
    compositor_window_write(fd, k_buf, to_copy);
    return (long)to_copy;
  }

  /* Standard output redirection */
  if ((fd == 1 || fd == 2) && current_process) {
    int win_id = compositor_get_window_by_pid(current_process->pid);
    if (win_id > 0) {
      compositor_window_write(win_id, k_buf, to_copy);
      return (long)to_copy;
    }
  }

  /* Default to UART for now (Legacy/Debug logs) */
  uart_puts(k_buf);
  return (long)to_copy;
}

void sys_exit(int status) {
  if (current_process) {
    pr_info("PID %d exiting with status %d\n", current_process->pid, status);
    process_terminate(current_process->pid);

    /* We are now ZOMBIE (if terminate worked). Switch away immediately. */
    /* Note: We use current_process->context which points to our kernel stack
       frame. schedule() will save our state there (not strictly needed since we
       are dead) and switch to next task. */
    schedule(current_process->context);

    /* Should never reach here */
    panic("sys_exit returned");
  }
}

struct pt_regs *syscall_handler(struct pt_regs *frame) {
  uint64_t syscall_num = frame->regs[8];
  uint64_t arg0 = frame->regs[0];
  uint64_t arg1 = frame->regs[1];
  uint64_t arg2 = frame->regs[2];
  uint64_t arg3 = frame->regs[3];
  uint64_t arg4 = frame->regs[4];
  uint64_t arg5 = frame->regs[5];

  /* Check Exception Syndrome to distinguish SVC from Aborts */
  uint64_t esr = arch_get_esr();
  uint64_t ec = (esr >> 26) & 0x3F;

  /* EC 0x15 = SVC from AArch64 */
  if (ec != 0x15) {
    uint64_t far = arch_get_far();
    uint64_t iss = esr & 0x1FFFFFF;

    pr_err(
        "PID %d EXCEPTION: EC=0x%lx ESR=0x%lx FAR=0x%lx ELR=0x%lx ISS=0x%lx\n",
        current_process ? (int)current_process->pid : -1, ec, esr, far,
        frame->elr, iss);

    /* Decode exception class */
    const char *ec_name = "Unknown";
    switch (ec) {
    case 0x00:
      ec_name = "Unknown/Uncategorized";
      break;
    case 0x01:
      ec_name = "WFI/WFE";
      break;
    case 0x20:
      ec_name = "Instruction Abort (Lower EL)";
      break;
    case 0x21:
      ec_name = "Instruction Abort (Same EL)";
      break;
    case 0x24:
      ec_name = "Data Abort (Lower EL)";
      break;
    case 0x25:
      ec_name = "Data Abort (Same EL)";
      break;
    default:
      break;
    }
    pr_err("Exception Class: %s (0x%lx)\n", ec_name, ec);

    if (current_process) {
      pr_err("Terminating PID %d due to fatal exception\n",
             current_process->pid);
      process_terminate(current_process->pid);
      return schedule(frame);
    } else {
      /* Kernel Fault? This function is only for Lower EL Sync though... */
      /* But if VBAR points here for EL1 Sync? No, distinct vectors. */
      /* If we are here, it SHOULD be User Fault. */
      panic("Fatal Exception in Kernel Thread Context");
    }
  }

  switch (syscall_num) {
  case 63: /* READ */
    return sys_read(frame);
  case 64: /* WRITE */
  {
    /* pr_info("Syscall: WRITE from PID %d, fd=%ld\n", current_process->pid,
     * arg0); */
    frame->regs[0] = sys_write((int)arg0, (const char *)arg1, (size_t)arg2);
    break;
  }
  case 93: /* EXIT */
    sys_exit((int)arg0);
    return schedule(frame);
  case 169: /* GET_TIME */
    frame->regs[0] = sys_get_time();
    break;
  case 172: /* GETPID */
    frame->regs[0] = sys_get_pid();
    break;
  case 30: /* IPC_SEND */
    frame->regs[0] = sys_ipc_send((int)arg0, (void *)arg1);
    break;
  case 31: /* IPC_RECV */
    frame->regs[0] = sys_ipc_recv((int)arg0, (void *)arg1);
    break;
  case 32: /* IPC_TRY_RECV */
  {
    extern int sys_ipc_try_recv(int src_pid,
                                void *msg_ptr); /* Ensure proto visible */
    frame->regs[0] = sys_ipc_try_recv((int)arg0, (void *)arg1);
  } break;
  case 200: /* DRAW */
    graphics_draw_rect((int)arg0, (int)arg1, (int)arg2, (int)arg3,
                       (uint32_t)arg4);
    frame->regs[0] = 0;
    break;
  case 201: /* FLUSH */
    compositor_render();
    pr_info("%s", "SYSCALL: FLUSH done, returning to user\n");
    frame->regs[0] = 0;
    break;
  case 210: /* CREATE_WINDOW */
  {
    struct cpu_info *cpu = get_cpu_info();
    char *k_title = cpu->syscall_buf;
    if (copy_string_from_user(k_title, (const char *)arg4, 64) != 0) {
      frame->regs[0] = -1;
      break;
    }
    frame->regs[0] =
        compositor_create_window((int)arg0, (int)arg1, (int)arg2, (int)arg3,
                                 k_title, current_process->pid);
  } break;
  case 211: /* WINDOW_DRAW */
    compositor_draw_rect((int)arg0, (int)arg1, (int)arg2, (int)arg3, (int)arg4,
                         (uint32_t)arg5, current_process->pid);
    frame->regs[0] = 0;
    break;
  case 212: /* COMPOSITOR_RENDER */
    compositor_render();
    frame->regs[0] = 0;
    break;
  case 213: /* WINDOW_BLIT */
    compositor_blit((int)arg0, (int)arg1, (int)arg2, (int)arg3, (int)arg4,
                    (const uint32_t *)arg5, current_process->pid);
    frame->regs[0] = 0;
    break;
  case 214: /* WINDOW_SET_FLAGS */
    compositor_set_window_flags((int)arg0, (int)arg1);
    frame->regs[0] = 0;
    break;
  case 215: /* DESTROY_WINDOW */
    compositor_destroy_window((int)arg0);
    frame->regs[0] = 0;
    break;
  case 220: /* SPAWN */
  {
    struct cpu_info *cpu = get_cpu_info();
    char *k_path = cpu->syscall_buf;
    if (copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      frame->regs[0] = -1;
      break;
    }
    arch_local_irq_disable();
    struct process *new_proc =
        process_create(k_path, PROC_PRIO_USER, PROC_PERM_USER);
    if (new_proc) {
      if (process_load_elf(new_proc, k_path) == 0) {
        enqueue_task(new_proc);
        frame->regs[0] = new_proc->pid;
      } else {
        process_terminate(new_proc->pid);
        frame->regs[0] = -1;
      }
    } else {
      frame->regs[0] = -1;
    }
    arch_local_irq_enable();
  } break;
  case 221: /* KILL */
    frame->regs[0] = process_terminate((int)arg0);
    break;
  case 222: /* GETPROCS */
  {
    extern long sys_getprocs(struct ps_info * user_buf, size_t max_count);
    frame->regs[0] = sys_getprocs((struct ps_info *)arg0, (size_t)arg1);
  } break;
  case 223: /* YIELD */
    return schedule(frame);
  case 230: /* SEND (IPC) */
  {
    extern int sys_ipc_send(int target_pid, void *msg_ptr);
    frame->regs[0] = sys_ipc_send((int)arg0, (void *)arg1);
    /* Yield after successful send to allow target to process message
     * immediately */
    if (frame->regs[0] == 0)
      return schedule(frame);
  } break;
  case 231: /* RECV (IPC) */
  {
    extern int sys_ipc_recv(int src_pid, void *msg_ptr);
    frame->regs[0] = sys_ipc_recv((int)arg0, (void *)arg1);
    /* RECV blocks, so yield */
    return schedule(frame);
  } break;
  case 232: /* SET_FOCUS */
  {
    extern int keyboard_focus_pid;
    keyboard_focus_pid = (int)arg0;
    frame->regs[0] = 0;
    break;
  }
  case 247: /* WAIT */
    frame->regs[0] = process_wait((int)arg0);
    break;
  case 250: /* REGISTRY */
    frame->regs[0] =
        sys_registry((int)arg0, (const char *)arg1, (char *)arg2, (size_t)arg3);
    break;
  case 251: /* FILE_WRITE */
  {
    struct cpu_info *cpu = get_cpu_info();
    char *k_path = cpu->syscall_buf;
    if (copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      frame->regs[0] = -1;
      break;
    }

    size_t size = (size_t)arg2;
    uint8_t *k_buf = kmalloc(size);
    if (!k_buf) {
      frame->regs[0] = -1;
      break;
    }

    if (copy_from_user(k_buf, (const void *)arg1, size) != 0) {
      kfree(k_buf);
      frame->regs[0] = -1;
      break;
    }

    uint32_t offset = (uint32_t)arg3;
    frame->regs[0] = ext4_write_file(k_path, k_buf, (uint32_t)size, offset);
    kfree(k_buf);
  } break;
  case 252: /* FILE_READ */
  {
    char k_path[128];
    if (copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      frame->regs[0] = -1;
      break;
    }
    size_t size = (size_t)arg2;
    uint32_t offset = (uint32_t)arg3;

    uint8_t *k_buf = kmalloc(size);
    if (!k_buf) {
      frame->regs[0] = -1;
      break;
    }

    int bytes_read = ext4_read_file(k_path, k_buf, (uint32_t)size, offset);

    if (bytes_read >= 0) {
      if (copy_to_user((void *)arg1, k_buf, bytes_read) != 0) {
        bytes_read = -1;
      }
    }

    kfree(k_buf);
    frame->regs[0] = bytes_read;
  } break;
  default:
    pr_warn("Unknown syscall: %ld\n", syscall_num);
    frame->regs[0] = -1;
    break;
  }
  return frame;
}
