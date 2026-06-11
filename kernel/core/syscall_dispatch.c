/*
 * kernel/core/syscall_dispatch.c
 * Architecture-Agnostic Syscall Dispatcher
 *
 * This file is the central syscall switch for OS1/NEXS.  The arch-specific
 * syscall entry stub (aarch64: svc_handler / amd64: syscall entry in cpu.c)
 * calls kernel_syscall_dispatcher() with the saved register frame.  This
 * function reads the syscall number and arguments from the frame via the
 * pt_regs_* accessor macros (arch-agnostic) and dispatches to the appropriate
 * implementation.
 *
 * Role / layering:
 *   userland svc/syscall -> arch entry (context.S / cpu.c)
 *                        -> kernel_syscall_dispatcher()  [this file]
 *                        -> sys_* / process_* / compositor_* / vfs_*
 *   Returns a pt_regs* to restore; may differ from the input frame when
 *   schedule() performs a context switch (IPC block, exit, yield).
 *
 * Key invariants:
 *   - All user pointers (arg0..arg5) must be validated via arch_copy_*_from_user
 *     or arch_copy_string_from_user before being dereferenced in the kernel.
 *   - Syscall implementations must not return directly; they write the return
 *     value via pt_regs_set_return() and fall through to "return frame", OR
 *     they call schedule() and return its result (a different task's frame).
 *   - cpu->syscall_buf (a per-CPU scratch buffer) is used for path/title copies;
 *     only one such copy is in flight per CPU at any time.
 *
 * Known issues:
 *   ABI-01  (W3 WRONG-DESIGN) Incoherent numbering: Linux-aarch64 numbers
 *           (63/64/93/247) mixed with ad-hoc numbers (200-256); IPC is
 *           duplicated at both 30/31/32 and 230/231; 30/31/32 are not in os1.h.
 *   ABI-02  (W3 MISSING) No errno: all failures return bare -1; only
 *           sys_ipc_send is the exception (-EINVAL).  Callers cannot
 *           distinguish error codes.
 *   ABI-03  (W3 WRONG-DESIGN) No per-process fd table: fd 0=stdin(IPC),
 *           1/2=window-by-pid, >=100=window id.  Neither POSIX nor Plan 9.
 *   ABI-04  (W4 SECURITY) No capability/permission checks: any process may
 *           kill any non-system PID (case 221), steal keyboard focus (case 232),
 *           destroy any window (case 215), or write any file (case 251).
 *   ABI-05  (W2 BUG) case 230 (SEND): the self-admitted broken reschedule
 *           logic — comment in code says "pt_regs_arg is read-only … I'll fix
 *           it below" and the post-switch comment at line 296-298 confirms it.
 *   ABI-06  (W2 BUG/PERF) sys_write() silently truncates writes > 1023 bytes
 *           and unconditionally echoes every write to the UART (debug leftover).
 *   ABI-07  (W2 BUG) case 220 (SPAWN): disables IRQs across process_create +
 *           process_load_elf, which may trigger blocking virtio/ext4 disk I/O.
 *   GFX-FONT-01  (W4 SECURITY/BUG) case 253 (SET_FONT): stores a raw user
 *           pointer into kernel globals; dereferenced in IRQ-context rendering
 *           (sys_set_font in graphics/font.c) → UAF / info-leak.
 *   EXT4-02  (W4 SECURITY) case 251 (FILE_WRITE): no access control;
 *           any PID can overwrite any file, including /init.
 */
#include <kernel/types.h>
#include <arch/pt_regs.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <kernel/cpu.h>
#include <kernel/string.h>
#include <kernel/kmalloc.h>
#include <kernel/vfs.h>

/*
 * FIX(EXT4-07): upper bound for kmalloc'd bounce buffers whose size comes
 * straight from a user syscall argument (arg2) in FILE_WRITE/FILE_READ/LIST_DIR
 * (cases 251/252/254).  Without a cap a process can pass size=4 GB and make the
 * kernel attempt an enormous pmm_alloc_pages() — at best a slow contiguous scan
 * that fails (NULL), at worst draining kernel RAM (there is no per-process
 * quota) → OOM/DoS.
 *
 * 16 MiB sits above every legitimate single-syscall transfer: the largest
 * routine read is a ~98 KB font (read whole-file at boot), DOOM reads WAD lumps
 * individually (each well under 1 MB), and the ext4 driver's own single-indirect
 * read ceiling is ~4 MB (double-indirect is unimplemented) — so this cap never
 * truncates a read the driver could actually satisfy, while rejecting absurd
 * allocations.  size==0 (the FILE_READ size-probe) is never > the cap, so it is
 * unaffected.
 */
#define SYSCALL_MAX_IO_BYTES (16u * 1024u * 1024u)  /* 16 MiB */

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
int sys_set_font(void *data, size_t size);
/* Filesystem access goes through the VFS contract only (<kernel/vfs.h>);
 * no direct ext4_* calls (VFS-01 resolved). */

extern int arch_copy_from_user(void *dest, const void *src, size_t n);
extern int arch_copy_to_user(void *dest, const void *src, size_t n);
extern int arch_copy_string_from_user(char *dest, const char *src, size_t max_len);

extern int keyboard_focus_pid;

/*
 * kernel_syscall_dispatcher - dispatch a syscall from the saved register frame.
 *
 * Entry point called by the arch-specific svc/syscall handler immediately
 * after saving all user registers into 'frame'.  Reads syscall_num and up
 * to six arguments from frame via pt_regs_* accessors.
 *
 * Returns: a pt_regs* to restore.  In the common (non-blocking) case this is
 *          'frame' itself with the return value written via pt_regs_set_return().
 *          For blocking operations (EXIT/YIELD/IPC RECV and sometimes IPC SEND)
 *          this is the frame of the next scheduled process.
 *
 * Locking: no locks held on entry; individual cases may acquire
 *          sched_lock / msg_lock / per-CPU sched_lock internally.
 * IRQ context: no — syscalls run in kernel mode with IRQs enabled (normal
 *          exception-level transition on aarch64; ring 3->0 on amd64).
 *
 * NOTE(ABI-01): The switch uses a mix of Linux-aarch64 numbers and ad-hoc
 *          numbers; IPC is duplicated at 30/31/32 and 230/231.
 * NOTE(ABI-04): There are no capability checks at the dispatch boundary;
 *          any user process may invoke any case.
 * NOTE(ABI-07): case 220 (SPAWN) calls arch_local_irq_disable() before
 *          process_create + process_load_elf, which may block on virtio I/O.
 */
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
    /* NOTE(ABI-05): The intent is to yield after a successful send so the
     * receiver can run, but pt_regs_arg() reads the original arg register,
     * not the return value we just wrote.  The reschedule condition is
     * therefore always false (original arg0 is the target PID, not 0).
     * The post-switch comment at the bottom of this function acknowledges the
     * same defect.  The send itself succeeds; the yield does not. */
    pt_regs_set_return(frame, sys_ipc_send((int)arg0, (void *)arg1));
    if (pt_regs_arg(frame, 0) == 0) return schedule(frame); /* pt_regs_arg is read-only. I mean checking the return we just set. */
    break;
  case 231: /* RECV (IPC) */
    /* NOTE(IPC-02): Unlike case 31 (IPC_RECV), this variant unconditionally
     * calls schedule() after sys_ipc_recv(), even if a message was already
     * available.  sys_ipc_recv() returns 0 whether it received or blocked,
     * so the caller cannot distinguish the two outcomes. */
    pt_regs_set_return(frame, sys_ipc_recv((int)arg0, (void *)arg1));
    return schedule(frame);
  case 232: /* SET_FOCUS */
    /* NOTE(ABI-04): No permission check; any user process can redirect the
     * global keyboard focus to any PID, including system processes. */
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
    char resolved_path[128];
    vfs_resolve_path(k_path, resolved_path, 128);
    size_t size = (size_t)arg2;
    if (size > SYSCALL_MAX_IO_BYTES) {  /* FIX(EXT4-07): reject absurd user size */
      pt_regs_set_return(frame, -1);
      break;
    }
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
    pt_regs_set_return(frame, vfs_write_file(resolved_path, k_buf, (uint32_t)size, offset));
    kfree(k_buf);
  } break;
  case 252: /* FILE_READ */
  {
    char k_path[128];
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -1);
      break;
    }
    char resolved_path[128];
    vfs_resolve_path(k_path, resolved_path, 128);
    size_t size = (size_t)arg2;
    if (size > SYSCALL_MAX_IO_BYTES) {  /* FIX(EXT4-07): reject absurd user size */
      pt_regs_set_return(frame, -1);
      break;
    }
    uint32_t offset = (uint32_t)arg3;
    int bytes_read;

    if (size == 0) {
      bytes_read = vfs_read_file(resolved_path, NULL, 0, offset);
    } else {
      uint8_t *k_buf = kmalloc(size);
      if (!k_buf) {
        pt_regs_set_return(frame, -1);
        break;
      }

      bytes_read = vfs_read_file(resolved_path, k_buf, (uint32_t)size, offset);
      if (bytes_read >= 0) {
        if (arch_copy_to_user((void *)arg1, k_buf, bytes_read) != 0) {
          bytes_read = -1;
        }
      }
      kfree(k_buf);
    }
    pt_regs_set_return(frame, bytes_read);
  } break;
  case 253: /* SET_FONT */
    pt_regs_set_return(frame, sys_set_font((void *)arg0, (size_t)arg1));
    break;
  case 254: /* LIST_DIR */
  {
    char k_path[128];
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -1);
      break;
    }
    char resolved_path[128];
    vfs_resolve_path(k_path, resolved_path, 128);
    size_t size = (size_t)arg2;
    if (size > SYSCALL_MAX_IO_BYTES) {  /* FIX(EXT4-07): reject absurd user size */
      pt_regs_set_return(frame, -1);
      break;
    }
    char *k_buf = kmalloc(size);
    if (!k_buf) {
      pt_regs_set_return(frame, -1);
      break;
    }
    int res = vfs_list_dir(resolved_path, k_buf, (uint32_t)size);
    if (res >= 0) {
      if (arch_copy_to_user((void *)arg1, k_buf, res + 1) != 0) {
        res = -1;
      }
    }
    kfree(k_buf);
    pt_regs_set_return(frame, res);
  } break;
  case 255: /* CHDIR */
  {
    char k_path[128];
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -1);
      break;
    }
    char resolved_path[128];
    vfs_resolve_path(k_path, resolved_path, 128);
    
    /* Verify it exists and is a directory. */
    struct vfs_stat st;
    if (vfs_stat(resolved_path, &st) == 0 && st.type == VFS_TYPE_DIR) {
       strncpy(current_process->cwd, resolved_path, 128);
       pt_regs_set_return(frame, 0);
    } else {
       pt_regs_set_return(frame, -1);
    }
  } break;
  case 256: /* GETCWD */
  {
    size_t size = (size_t)arg1;
    if (arch_copy_to_user((void *)arg0, current_process->cwd, size) != 0) {
      pt_regs_set_return(frame, -1);
    } else {
      pt_regs_set_return(frame, 0);
    }
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

/*
 * sys_get_time - return current time in milliseconds.
 *
 * Divides timer_get_us() by 1000.  On aarch64 this is accurate (arch counter).
 * On amd64 timer_get_us() returns jiffies*1000, so the result has 1ms
 * resolution only (ARCH-03).
 *
 * Locking: none.  IRQ context: no.
 */
extern uint64_t timer_get_us(void);
long sys_get_time(void) { return (long)(timer_get_us() / 1000); }

/*
 * sys_get_pid - return the PID of the calling process.
 *
 * Returns 0 if current_process is NULL (should not happen in normal operation).
 * Locking: none (current_process is CPU-local during a syscall).
 * IRQ context: no.
 */
long sys_get_pid(void) {
  return current_process ? (long)current_process->pid : 0;
}

/*
 * sys_read - blocking read implementation (syscall 63, fd=0 stdin only).
 *
 * Drains the calling process's IPC message queue looking for IPC_TYPE_INPUT
 * messages (keyboard events).  Returns the key character of the first pressed
 * or repeated event (data2 != 0); ignores key-release events (data2 == 0).
 *
 * If no ready input message is found, the process is put to sleep
 * (PROC_SLEEPING, ipc_target_pid=-1) with a retry annotation
 * (pt_regs_retry_syscall) so the syscall instruction is re-executed when the
 * process wakes up, and schedule() is called to switch to another task.
 *
 * Non-stdin fds: unhandled; falls through to the sleep path regardless.
 *
 * NOTE(ABI-03): fd 0 is overloaded as IPC channel, not a real file descriptor.
 *
 * Locking: process sleeps after a short IRQ-disable window to set state;
 *          no spinlock held across the sleep.
 * IRQ context: no — called from the syscall dispatcher.
 * Returns: regs (with return value set) on a hit; schedule(regs) on a miss.
 */
struct pt_regs *sys_read(struct pt_regs *regs) {
  int fd = (int)pt_regs_arg(regs, 0);
  char *buf = (char *)pt_regs_arg(regs, 1);
  size_t count = (size_t)pt_regs_arg(regs, 2);
  (void)count;

  if (fd == 0) { /* STDIN */
    while (1) {
      extern struct ipc_node *pop_message(struct process * proc, int src_pid);
      struct ipc_node *node = pop_message(current_process, -1); /* From ANY */

      if (!node)
        break;

      if (node->msg.type == IPC_TYPE_INPUT) {
        /* Only return pressed (1) or repeat (2) events to standard read()
         * Release (0) events are ignored for compatibility with shell/etc.
         */
        if (node->msg.data2 != 0) {
          char c = (char)node->msg.data1;
          if (arch_copy_to_user(buf, &c, 1) != 0) { }
          pt_regs_set_return(regs, 1);
          kfree(node);
          return regs;
        }
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

/*
 * sys_write - write to a file descriptor.
 *
 * Copies up to min(count, 1023) bytes from user space into the per-CPU
 * syscall_buf scratch buffer, then:
 *   fd >= 100: routes directly to compositor_window_write(fd, ...).
 *   fd == 1 or 2: looks up the calling process's compositor window and
 *                 writes there; falls through to UART echo if no window.
 *   other fds: not handled; returns to_copy after UART echo only.
 *
 * NOTE(ABI-06): Silently truncates writes > 1023 bytes with no error.
 *              Unconditionally echoes every write to the UART regardless of
 *              fd — debug behaviour left on a hot code path. [static]
 * NOTE(ABI-03): fd 1/2 are window-by-pid, not stdout/stderr file descriptors.
 *
 * Locking: none (cpu->syscall_buf is per-CPU, safe without a lock during a
 *          syscall because the calling process is pinned to this CPU).
 * IRQ context: no.
 * Returns: number of bytes written (to_copy), or -1 on uaccess failure.
 */
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

/*
 * sys_exit - terminate the calling process.
 *
 * Calls process_terminate(current_process->pid), which marks the process
 * PROC_ZOMBIE and returns immediately (it cannot free its own kernel stack).
 * The caller (case 93 in kernel_syscall_dispatcher) MUST call schedule()
 * after sys_exit() to switch away from this process; that schedule() call
 * auto-reaps the zombie via the per-CPU deferred-free stack.
 *
 * Locking: delegates to process_terminate() which acquires sched_lock.
 * IRQ context: no.
 * NOTE(SCHED-03, mitigated): zombies no longer accumulate — schedule()
 *          reaps them without requiring a process_wait() caller.
 */
void sys_exit(int status) {
  if (current_process) {
    pr_info("PID %d exiting with status %d\n", current_process->pid, status);
    process_terminate(current_process->pid);
  }
}
