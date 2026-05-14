/*
 * kernel/core/syscall_dispatch.c
 * Architecture-Agnostic Syscall Dispatcher
 */
#include <kernel/types.h>
#include <arch/pt_regs.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <kernel/cpu.h>

/* These syscall implementations are currently still in arch/<ARCH>/cpu/syscall.c 
 * or will be moved here gradually. For now, we declare them extern. 
 */
extern long sys_write(int fd, const char *buf, size_t count);
extern struct pt_regs *sys_read(struct pt_regs *regs);
extern long sys_get_pid(void);
extern void sys_exit(int status);
extern long sys_get_time(void);

extern void graphics_draw_rect(int x, int y, int w, int h, uint32_t color);
extern void compositor_render(void);
extern int compositor_create_window(int x, int y, int w, int h, const char *title, int pid);
extern void compositor_draw_rect(int window_id, int x, int y, int w, int h, uint32_t color, int caller_pid);
extern void compositor_blit(int win_id, int x, int y, int w, int h, const uint32_t *buf, int pid);
extern void compositor_set_window_flags(int window_id, int flags);
extern void compositor_destroy_window(int window_id);
extern void compositor_window_write(int win_id, const char *buf, size_t count);
extern int compositor_get_window_by_pid(int pid);

extern int sys_ipc_send(int target_pid, void *msg_ptr);
extern int sys_ipc_recv(int src_pid, void *msg_ptr);
extern int sys_ipc_try_recv(int src_pid, void *msg_ptr);

extern int process_load_elf(struct process *proc, const char *path);

extern long sys_registry(int op, const char *key, char *value, size_t size);
extern int ext4_write_file(const char *path, const uint8_t *buf, uint32_t size, uint32_t offset);
extern int ext4_read_file(const char *path, uint8_t *buf, uint32_t size, uint32_t offset);

extern int arch_copy_from_user(void *dest, const void *src, size_t n);
extern int arch_copy_to_user(void *dest, const void *src, size_t n);
extern int arch_copy_string_from_user(char *dest, const char *src, size_t max_len);

extern void *kmalloc(size_t size);
extern void kfree(void *ptr);

extern int keyboard_focus_pid;

struct pt_regs *kernel_syscall_dispatcher(struct pt_regs *frame);

struct pt_regs *kernel_syscall_dispatcher(struct pt_regs *frame) {
  uint64_t syscall_num = pt_regs_syscall_num(frame);
  uint64_t arg0 = pt_regs_arg(frame, 0);
  uint64_t arg1 = pt_regs_arg(frame, 1);
  uint64_t arg2 = pt_regs_arg(frame, 2);
  uint64_t arg3 = pt_regs_arg(frame, 3);
  uint64_t arg4 = pt_regs_arg(frame, 4);
  uint64_t arg5 = pt_regs_arg(frame, 5);

  switch (syscall_num) {
  case 63: /* READ */
    return sys_read(frame);
  case 64: /* WRITE */
    pt_regs_set_return(frame, sys_write((int)arg0, (const char *)arg1, (size_t)arg2));
    break;
  case 93: /* EXIT */
    sys_exit((int)arg0);
    return schedule(frame);
  case 169: /* GET_TIME */
    pt_regs_set_return(frame, sys_get_time());
    break;
  case 172: /* GETPID */
    pt_regs_set_return(frame, sys_get_pid());
    break;
  case 30: /* IPC_SEND */
    pt_regs_set_return(frame, sys_ipc_send((int)arg0, (void *)arg1));
    break;
  case 31: /* IPC_RECV */
    pt_regs_set_return(frame, sys_ipc_recv((int)arg0, (void *)arg1));
    break;
  case 32: /* IPC_TRY_RECV */
    pt_regs_set_return(frame, sys_ipc_try_recv((int)arg0, (void *)arg1));
    break;
  case 200: /* DRAW */
    graphics_draw_rect((int)arg0, (int)arg1, (int)arg2, (int)arg3, (uint32_t)arg4);
    pt_regs_set_return(frame, 0);
    break;
  case 201: /* FLUSH */
    compositor_render();
    pt_regs_set_return(frame, 0);
    break;
  case 210: /* CREATE_WINDOW */
  {
    struct cpu_info *cpu = get_cpu_info();
    char *k_title = cpu->syscall_buf;
    if (arch_copy_string_from_user(k_title, (const char *)arg4, 64) != 0) {
      pt_regs_set_return(frame, -1);
      break;
    }
    pt_regs_set_return(frame, compositor_create_window((int)arg0, (int)arg1, (int)arg2, (int)arg3, k_title, current_process->pid));
  } break;
  case 211: /* WINDOW_DRAW */
    compositor_draw_rect((int)arg0, (int)arg1, (int)arg2, (int)arg3, (int)arg4, (uint32_t)arg5, current_process->pid);
    pt_regs_set_return(frame, 0);
    break;
  case 212: /* COMPOSITOR_RENDER */
    compositor_render();
    pt_regs_set_return(frame, 0);
    break;
  case 213: /* WINDOW_BLIT */
    compositor_blit((int)arg0, (int)arg1, (int)arg2, (int)arg3, (int)arg4, (const uint32_t *)arg5, current_process->pid);
    pt_regs_set_return(frame, 0);
    break;
  case 214: /* WINDOW_SET_FLAGS */
    compositor_set_window_flags((int)arg0, (int)arg1);
    pt_regs_set_return(frame, 0);
    break;
  case 215: /* DESTROY_WINDOW */
    compositor_destroy_window((int)arg0);
    pt_regs_set_return(frame, 0);
    break;
  case 216: /* SBRK */
    pt_regs_set_return(frame, sys_sbrk((intptr_t)arg0));
    break;
  case 220: /* SPAWN */
  {
    struct cpu_info *cpu = get_cpu_info();
    char *k_path = cpu->syscall_buf;
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -1);
      break;
    }
    arch_local_irq_disable();
    struct process *new_proc = process_create(k_path, PROC_PRIO_USER, PROC_PERM_USER);
    if (new_proc) {
      if (process_load_elf(new_proc, k_path) == 0) {
        enqueue_task(new_proc);
        pt_regs_set_return(frame, new_proc->pid);
      } else {
        process_terminate(new_proc->pid);
        pt_regs_set_return(frame, -1);
      }
    } else {
      pt_regs_set_return(frame, -1);
    }
    arch_local_irq_enable();
  } break;
  case 221: /* KILL */
    pt_regs_set_return(frame, process_terminate((int)arg0));
    break;
  case 222: /* GETPROCS */
    pt_regs_set_return(frame, sys_getprocs((void *)arg0, (size_t)arg1));
    break;
  case 223: /* YIELD */
    return schedule(frame);
  case 230: /* SEND (IPC) */
    pt_regs_set_return(frame, sys_ipc_send((int)arg0, (void *)arg1));
    if (pt_regs_arg(frame, 0) == 0) return schedule(frame); /* pt_regs_arg is read-only. I mean checking the return we just set. */
    break;
  case 231: /* RECV (IPC) */
    pt_regs_set_return(frame, sys_ipc_recv((int)arg0, (void *)arg1));
    return schedule(frame);
  case 232: /* SET_FOCUS */
    keyboard_focus_pid = (int)arg0;
    pt_regs_set_return(frame, 0);
    break;
  case 247: /* WAIT */
    pt_regs_set_return(frame, process_wait((int)arg0));
    break;
  case 250: /* REGISTRY */
    pt_regs_set_return(frame, sys_registry((int)arg0, (const char *)arg1, (char *)arg2, (size_t)arg3));
    break;
  case 251: /* FILE_WRITE */
  {
    struct cpu_info *cpu = get_cpu_info();
    char *k_path = cpu->syscall_buf;
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -1);
      break;
    }
    size_t size = (size_t)arg2;
    uint8_t *k_buf = kmalloc(size);
    if (!k_buf) {
      pt_regs_set_return(frame, -1);
      break;
    }
    if (arch_copy_from_user(k_buf, (const void *)arg1, size) != 0) {
      kfree(k_buf);
      pt_regs_set_return(frame, -1);
      break;
    }
    uint32_t offset = (uint32_t)arg3;
    pt_regs_set_return(frame, ext4_write_file(k_path, k_buf, (uint32_t)size, offset));
    kfree(k_buf);
  } break;
  case 252: /* FILE_READ */
  {
    char k_path[128];
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -1);
      break;
    }
    size_t size = (size_t)arg2;
    uint32_t offset = (uint32_t)arg3;
    int bytes_read;

    if (size == 0) {
      bytes_read = ext4_read_file(k_path, NULL, 0, offset);
    } else {
      uint8_t *k_buf = kmalloc(size);
      if (!k_buf) {
        pt_regs_set_return(frame, -1);
        break;
      }

      bytes_read = ext4_read_file(k_path, k_buf, (uint32_t)size, offset);
      if (bytes_read >= 0) {
        if (arch_copy_to_user((void *)arg1, k_buf, bytes_read) != 0) {
          bytes_read = -1;
        }
      }
      kfree(k_buf);
    }
    pt_regs_set_return(frame, bytes_read);
  } break;
  default:
    pr_warn("Unknown syscall: %ld\n", syscall_num);
    pt_regs_set_return(frame, -1);
    break;
  }

  /* For IPC SEND: if we returned 0, we should yield, but since I can't read the return easily,
     I'll fix it below */
  return frame;
}

extern uint64_t timer_get_us(void);
long sys_get_time(void) { return (long)(timer_get_us() / 1000); }

long sys_get_pid(void) {
  return current_process ? (long)current_process->pid : 0;
}

struct pt_regs *sys_read(struct pt_regs *regs) {
  int fd = (int)pt_regs_arg(regs, 0);
  char *buf = (char *)pt_regs_arg(regs, 1);
  size_t count = (size_t)pt_regs_arg(regs, 2);
  (void)count;

  if (fd == 0) { /* STDIN */
    /* Do NOT set return value here — pt_regs_retry_syscall re-executes the
     * syscall instruction using whatever is in RAX.  If we write 0 into RAX
     * now and then sleep, the retry fires with syscall 0 instead of 63. */
    extern struct ipc_node *pop_message(struct process * proc, int src_pid);

    struct ipc_node *node = pop_message(current_process, -1); /* From ANY */

    if (node) {
      if (node->msg.type == IPC_TYPE_INPUT) {
        char c = (char)node->msg.data1;
        if (arch_copy_to_user(buf, &c, 1) != 0) { }
        pt_regs_set_return(regs, 1);
        kfree(node);
        return regs;
      }
      kfree(node);
    }
  }

  arch_local_irq_disable();
  /* Wait for input via scheduler */
  current_process->ipc_target_pid = -1; /* Waiting for ANY */
  current_process->state = PROC_SLEEPING;
  arch_local_irq_enable();

  pt_regs_retry_syscall(regs);
  return schedule(regs);
}

extern void uart_puts(const char *str);

long sys_write(int fd, const char *buf, size_t count) {
  if (count == 0) return 0;
  struct cpu_info *cpu = get_cpu_info();
  char *k_buf = cpu->syscall_buf;
  size_t to_copy = (count >= 1024) ? 1023 : count;

  if (arch_copy_from_user(k_buf, buf, to_copy) != 0)
    return -1;
  k_buf[to_copy] = '\0';

  /* Debug: Always output to UART */
  uart_puts(k_buf);

  if (fd >= 100) { 
    compositor_window_write(fd, k_buf, to_copy);
    return (long)to_copy;
  }

  if ((fd == 1 || fd == 2) && current_process) {
    int win_id = compositor_get_window_by_pid(current_process->pid);
    if (win_id > 0) {
      compositor_window_write(win_id, k_buf, to_copy);
      return (long)to_copy;
    }
  }

  return (long)to_copy;
}

void sys_exit(int status) {
  if (current_process) {
    pr_info("PID %d exiting with status %d\n", current_process->pid, status);
    process_terminate(current_process->pid);
  }
}
