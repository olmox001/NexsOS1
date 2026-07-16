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
 *   - All user pointers (arg0..arg5) must be validated via
 * arch_copy_*_from_user or arch_copy_string_from_user before being dereferenced
 * in the kernel.
 *   - Syscall implementations must not return directly; they write the return
 *     value via pt_regs_set_return() and fall through to "return frame", OR
 *     they call schedule() and return its result (a different task's frame).
 *   - cpu->syscall_buf (a per-CPU scratch buffer) is used for path/title
 * copies; only one such copy is in flight per CPU at any time.
 *
 * ABI (Phase B3):
 *   Numbering (ABI-01/ABI-SYS-01 RESOLVED): the switch uses the SYS_*
 *   macros from include/api/syscall_nums.h — the same header the userland
 *   stubs assemble against, so the two sides cannot drift.  The legacy
 *   duplicate IPC numbers (30/31/32) are gone (SEND/RECV/TRY_RECV =
 *   230/231/233).
 *   Error model (ABI-02 RESOLVED): failures return negative errno values
 *   from posix_types.h (-EFAULT for bad user pointers, -ENOMEM, -EINVAL,
 *   -ENOSYS for unknown numbers...); >= 0 means success.  VFS-layer calls
 *   still return their own negatives (mapped to -EIO/-ENOENT where the
 *   cause is unambiguous).
 *
 * Capability checks (ABI-04 batch 2 + USR-SEC-03 #79 batch 6):
 *   Privilege levels: machine (bypasses all checks, unkillable) > root >
 *   user > guest.  Fine-grained caps (CAP_*) gate each surface; the cut at
 *   spawn is monotonic (a child is never more privileged than its creator).
 *   SYS_SPAWN / SYS_SPAWN_CAPS  need CAP_SPAWN — else -EPERM.
 *   SYS_KILL         caller must be privileged, the target itself, or an
 *                    ancestor of it (process_kill_allowed) — else -EPERM.
 *   SYS_CREATE_WINDOW / SYS_SET_FOCUS  need CAP_WINDOW — else -EPERM;
 *                    cross-PID focus still needs machine level.
 *   SYS_DESTROY_WINDOW  owner or machine only — else -EPERM.
 *   SYS_OPEN(write) / SYS_FILE_WRITE  need CAP_FS_WRITE; the /bin and /sys
 *                    trees stay machine-only (EXT4-02) — else -EPERM/-EACCES.
 *   SYS_SEND         need CAP_IPC_ANY for non-relatives (process_ipc_allowed);
 *                    parent/descendants always allowed — else -EPERM.
 *   SYS_REGISTRY     write needs CAP_REG_WRITE; ownership enforced in
 *                    registry_set (LIB-REG-02/USR-SEC-01) — else
 * -EPERM/-EACCES. Kernel-internal paths (compositor close button, init
 * supervision, process teardown) call the underlying functions directly and
 * bypass these checks by design.
 *
 * Descriptor model (ASTRA §6.2 — the fd table folded into the object table):
 *   a POSIX descriptor IS a capability handle (fd N == handle N).  Every
 *   process is born with handles 0/1/2 as one shared OBJ_TYPE_CONSOLE object
 *   (0 = stdin keyboard, 1/2 = stdout+stderr window); open() ≡
 *   handle_create(FILE) and hands out handles >= 3; read/write/lseek/close
 *   (63/64/62/57) route through the object layer (sys_object_read/_write/
 *   _lseek + sys_handle_close).  There is no separate fd array.
 *
 * Known issues:
 *   ABI-07  (W2 BUG) SYS_SPAWN disables IRQs across process_create +
 *           process_load_elf, which may trigger blocking virtio/ext4 disk I/O.
 *   GFX-FONT-01  (W4 SECURITY/BUG) SYS_SET_FONT: stores a raw user
 *           pointer into kernel globals; dereferenced in IRQ-context rendering
 *           (sys_set_font in graphics/font.c) → UAF / info-leak.
 */
#include <arch/pt_regs.h>
#include <kernel/cpu.h>
#include <kernel/kmalloc.h>
#include <kernel/object.h>
#include <kernel/printk.h>
#include <kernel/registry.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vfs.h>
#include <style_names.h>
#include <syscall_nums.h>

/* Defined below (after sys_get_time); used by the SYS_NANOSLEEP dispatch case.
 */
static struct pt_regs *sys_nanosleep(struct pt_regs *regs, uint64_t ns);
/* Yield with per-process anti-spin throttle; used by the SYS_YIELD case. */
static struct pt_regs *sys_yield(struct pt_regs *regs);

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
 * individually (each well under 1 MB), and the ext4 driver's own
 * single-indirect read ceiling is ~4 MB (double-indirect is unimplemented) — so
 * this cap never truncates a read the driver could actually satisfy, while
 * rejecting absurd allocations.  size==0 (the FILE_READ size-probe) is never >
 * the cap, so it is unaffected.
 */
#define SYSCALL_MAX_IO_BYTES (16u * 1024u * 1024u) /* 16 MiB */

extern long sys_get_pid(void);
extern void sys_exit(int status);
extern long sys_get_time(void);
long sys_clock_gettime(int clk);

extern void graphics_draw_rect(int x, int y, int w, int h, uint32_t color);
extern void compositor_render(void);
extern int compositor_create_window(int x, int y, int w, int h,
                                    const char *title, int pid);
extern void compositor_draw_rect(int window_id, int x, int y, int w, int h,
                                 uint32_t color, int caller_pid);
extern void compositor_blit(int win_id, int x, int y, int w, int h,
                            const uint32_t *buf, int pid);
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
extern int arch_copy_string_from_user(char *dest, const char *src,
                                      size_t max_len);

extern int keyboard_focus_pid;

/*
 * kernel_syscall_dispatcher - dispatch a syscall from the saved register frame.
 *
 * Entry point called by the arch-specific svc/syscall handler immediately
 * after saving all user registers into 'frame'.  Reads syscall_num and up
 * to six arguments from frame via pt_regs_* accessors.
 *
 * Returns: a pt_regs* to restore.  In the common (non-blocking) case this is
 *          'frame' itself with the return value written via
 * pt_regs_set_return(). For blocking operations (EXIT/YIELD/IPC RECV and
 * sometimes IPC SEND) this is the frame of the next scheduled process.
 *
 * Locking: no locks held on entry; individual cases may acquire
 *          sched_lock / msg_lock / per-CPU sched_lock internally.
 * IRQ context: no — syscalls run in kernel mode with IRQs enabled (normal
 *          exception-level transition on aarch64; ring 3->0 on amd64).
 *
 * NOTE(ABI-07): case 220 (SPAWN) calls arch_local_irq_disable() before
 *          process_create + process_load_elf, which may block on virtio I/O.
 */
struct pt_regs *kernel_syscall_dispatcher(struct pt_regs *frame);

/* argv marshalling limits for SYS_SPAWN.  ELF_MAX_ARGS in elf.c must be >=
 * SPAWN_MAX_ARGS; SPAWN_ARG_LEN bounds each NUL-terminated argument. */
#define SPAWN_MAX_ARGS 16
#define SPAWN_ARG_LEN 128

/* level_for_path - the capability PRESET a binary gets from its location in the
 * VFS (ASTRA per-path stratification, F1): a /sys/bin service runs at ROOT
 * (system authority — refined per service later), everything else (notably
 * /bin) at USER.  This is a CEILING + default, NOT an escalation:
 * process_create_caps still clamps the child to no more privileged than its
 * creator, so a USER shell launching a /sys/bin binary does NOT gain root.
 * /sys/bin is also write- protected (object.c handle_create denies non-machine
 * writes under /sys,/bin), so the binaries backing this preset are immutable.
 */
static uint8_t level_for_path(const char *path) {
  if (path && strncmp(path, "/sys/bin/", 9) == 0)
    return PLVL_ROOT;
  return PLVL_USER;
}

/* dispatch_spawn - shared body for SYS_SPAWN and SYS_SPAWN_CAPS.
 *
 * NOTE(ABI-07): runs process_create + process_load_elf with IRQs disabled
 * across blocking virtio/ext4 disk I/O.  Pre-existing; kept verbatim so the
 * new capability path does not widen the critical section. */
static long dispatch_spawn(const char *path, uint8_t level, uint32_t caps,
                           int use_caps, int argc, char *const kargv[],
                           uint32_t flags) {
  /* ASTRA per-path preset (F1): plain spawn() takes the path's level;
   * spawn_caps may only DROP privilege below it (a request more privileged than
   * the path is capped to the path).  The creator-clamp in process_create_caps
   * then forbids any escalation regardless. */
  uint8_t path_lvl = level_for_path(path);
  if (!use_caps || level < path_lvl)
    level = path_lvl;

  arch_local_irq_disable();
  struct process *p =
      use_caps ? process_create_caps(path, PROC_PRIO_USER, level, caps)
               : process_create(path, PROC_PRIO_USER, level);
  long ret;
  if (p) {
    /* nxexec model (#193): a DETACHED spawn declines being the child's
     * controlling terminal — process_create inherited the spawner's window
     * as ctty unconditionally; the flag opts out (launcher-style spawns).
     * Safe here: the child is not yet enqueued/visible. */
    if (flags & SPAWN_FLAG_DETACHED)
      p->ctty_win = -1;
    if (process_load_elf_args(p, path, argc, kargv) == 0) {
      /* SCHED-UAF Pitfall B: commit the child atomically against a concurrent
       * kill.  Capture the pid first — process_finalize_spawn may RELEASE p (if
       * a kill was deferred while the ELF loaded), so p must not be read after.
       */
      long pid = (long)p->pid;
      process_finalize_spawn(p);
      ret = pid;
    } else {
      process_abort_spawn(p); /* load failed: release the half-built child */
      ret = -ENOENT;          /* path missing or unloadable ELF */
    }
  } else {
    ret = -EAGAIN; /* quota hit or process table exhausted */
  }
  arch_local_irq_enable();
  return ret;
}

/* window_text_write - copy a user text buffer into a kmalloc bounce (no
 * truncation; capped at SYSCALL_MAX_IO_BYTES), mirror it to the UART serial
 * log, and append it to compositor window win_id.  Shared by the FD_WIN
 * stdout sink and SYS_WINDOW_WRITE (#123).  Replaces the old 1023-byte
 * syscall_buf truncation (retires ABI-06 on the window path). */
extern void uart_puts(const char *str);
/* Non-static: also the OBJ_TYPE_CONSOLE stdout/stderr backend, called from
 * kernel/core/object.c (sys_object_write).  Shared by SYS_WINDOW_WRITE. */
long window_text_write(int win_id, const char *ubuf, size_t count) {
  if (count == 0)
    return 0;
  if (count > SYSCALL_MAX_IO_BYTES)
    return -EINVAL;
  char *k = kmalloc(count + 1);
  if (!k)
    return -ENOMEM;
  if (arch_copy_from_user(k, ubuf, count) != 0) {
    kfree(k);
    return -EFAULT;
  }
  k[count] = '\0';
  uart_puts(k);
  if (win_id > 0)
    compositor_window_write(win_id, k, count);
  kfree(k);
  return (long)count;
}

struct pt_regs *kernel_syscall_dispatcher(struct pt_regs *frame) {
  uint64_t syscall_num = pt_regs_syscall_num(frame);
  uint64_t arg0 = pt_regs_arg(frame, 0);
  uint64_t arg1 = pt_regs_arg(frame, 1);
  uint64_t arg2 = pt_regs_arg(frame, 2);
  uint64_t arg3 = pt_regs_arg(frame, 3);
  uint64_t arg4 = pt_regs_arg(frame, 4);
  uint64_t arg5 = pt_regs_arg(frame, 5);

  switch (syscall_num) {
  case SYS_OPEN: {
    /* open(path, flags) -> descriptor.  A descriptor IS a capability handle
     * (ASTRA §6.8: open ≡ handle_create(FS), the fd table folded into the
     * object table): this installs an OBJ_TYPE_FILE handle and returns its
     * index — the first free slot >= 3, since 0/1/2 are the pre-installed
     * stdin/stdout/stderr console handles, so the first open is fd 3 exactly as
     * before.  O_ACCMODE selects the handle's rights.
     *
     * O_CREAT/O_TRUNC/O_APPEND are honoured here (issue #126): the create /
     * truncate is applied through the SAME vfs_write_allowed() authority seam
     * SYS_FILE_WRITE and SYS_UNLINK use (S-ALIGN F6), so path-based and
     * handle-based writes cannot drift, and the POSIX personality logic in
     * libc keeps only flag/mode translation (ASTRA layering).  Any OTHER flag
     * is still an explicit -EINVAL, never silently ignored.  sys_handle_create
     * then does the resolve, the write-ACL re-check for a WRITE handle, and the
     * VFS open. */
    if (!current_process) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    int flags = (int)arg1;
    if (flags & ~(O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND)) {
      pt_regs_set_return(frame, -EINVAL);
      break;
    }
    if (flags & (O_CREAT | O_TRUNC)) {
      char kpath[128];
      if (arch_copy_string_from_user(kpath, (const char *)arg0, sizeof(kpath)) !=
          0) {
        pt_regs_set_return(frame, -EFAULT);
        break;
      }
      char resolved[128];
      vfs_resolve_path(kpath, resolved, sizeof(resolved));
      struct vfs_stat vst;
      int exists = (vfs_stat(resolved, &vst) == 0);
      long wperm;
      if (!exists && (flags & O_CREAT)) {
        if ((wperm = vfs_write_allowed(resolved)) != 0) {
          pt_regs_set_return(frame, wperm);
          break;
        }
        if (vfs_create(resolved, VFS_TYPE_FILE) != 0) {
          pt_regs_set_return(frame, -EIO);
          break;
        }
      } else if (exists && (flags & O_TRUNC)) {
        if ((wperm = vfs_write_allowed(resolved)) != 0) {
          pt_regs_set_return(frame, wperm);
          break;
        }
        /* No vfs_truncate primitive: drop and recreate empty — same net effect
         * as truncate-to-zero, reusing the create-on-write model. */
        vfs_unlink(resolved);
        if (vfs_create(resolved, VFS_TYPE_FILE) != 0) {
          pt_regs_set_return(frame, -EIO);
          break;
        }
      }
      /* O_TRUNC on a missing file without O_CREAT: fall through — the open
       * below resolves nothing and returns -ENOENT, matching POSIX. */
    }
    uint32_t rights = ((flags & O_ACCMODE) == O_RDONLY) ? OS1_RIGHT_READ
                      : ((flags & O_ACCMODE) == O_WRONLY)
                          ? OS1_RIGHT_WRITE
                          : (OS1_RIGHT_READ | OS1_RIGHT_WRITE);
    pt_regs_set_return(frame, sys_handle_create(OS1_NS_FS, (const char *)arg0,
                                                rights, OBJ_TYPE_FILE));
  } break;
  case SYS_CLOSE:
    /* close(fd) ≡ drop the handle; frees the object at its last reference. */
    pt_regs_set_return(frame, sys_handle_close((int)arg0));
    break;
  case SYS_LSEEK:
    /* lseek(fd, off, whence) ≡ reposition a FILE handle's shared offset; a
     * CONSOLE/stream handle returns -ESPIPE (handled in sys_object_lseek). */
    pt_regs_set_return(frame,
                       sys_object_lseek((int)arg0, (long)arg1, (int)arg2));
    break;
  case SYS_READ: {
    /* read(fd, buf, n) ≡ OS1_object_read on the handle.  A CONSOLE stdin with
     * no key pending returns -EAGAIN; that is NOT surfaced to userland — we
     * block the caller exactly like the folded-in FD_KBD path (PROC_SLEEPING,
     * wake on ANY message i.e. a keystroke, retry the syscall on wake;
     * rescheduling needs the trap frame, which is why this stays in the
     * dispatcher).  -EPERM (read of a write-only handle, e.g. stdout) maps to
     * the POSIX -EBADF. */
    long r = sys_object_read((int)arg0, (void *)arg1, (size_t)arg2);
    if (r == -EAGAIN && current_process) {
      arch_local_irq_disable();
      current_process->ipc_target_pid = -1; /* wake on ANY message */
      current_process->state = PROC_SLEEPING;
      arch_local_irq_enable();
      pt_regs_retry_syscall(frame);
      return schedule(frame);
    }
    pt_regs_set_return(frame, (r == -EPERM) ? -EBADF : r);
  } break;
  case SYS_WRITE: {
    /* write(fd, buf, n) ≡ OS1_object_write on the handle.  -EPERM (write of a
     * read-only handle, e.g. stdin) maps to the POSIX -EBADF. */
    long r = sys_object_write((int)arg0, (const void *)arg1, (size_t)arg2);
    pt_regs_set_return(frame, (r == -EPERM) ? -EBADF : r);
  } break;
  case SYS_EXIT:
    sys_exit((int)arg0);
    return schedule(frame);
  case SYS_GET_TIME:
    pt_regs_set_return(frame, sys_get_time());
    break;
  case SYS_CLOCK_GETTIME:
    pt_regs_set_return(frame, sys_clock_gettime((int)arg0));
    break;
  case SYS_GETPID:
    pt_regs_set_return(frame, sys_get_pid());
    break;
  case SYS_DRAW:
    /* Direct draw to the root framebuffer is a display effect: gated by
     * CAP_WINDOW (the level that "may draw" — guests included).  Previously
     * ungated.  A capability-stripped worker without CAP_WINDOW cannot scribble
     * on the screen; machine bypasses. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    graphics_draw_rect((int)arg0, (int)arg1, (int)arg2, (int)arg3,
                       (uint32_t)arg4);
    pt_regs_set_return(frame, 0);
    break;
  case SYS_WINDOW_ENUM: {
    /* Read-only window enumeration → struct window_info[] (ASTRA §6.7); ungated
     * like SYS_GETPROCS.  The dock /sys/bin/nxui lays out its app list from it.
     */
    extern long sys_window_enum(struct window_info * ubuf, size_t max);
    pt_regs_set_return(
        frame, sys_window_enum((struct window_info *)arg0, (size_t)arg1));
    break;
  }
  case SYS_CREATE_WINDOW: {
    /* USR-SEC-03 #79: drawing a window needs CAP_WINDOW. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    /* SCHED-UAF: a process being torn down must not create a NEW window after
     * process_terminate already destroyed its windows — it would be orphaned
     * (owner dead -> un-closeable).  The killer sets ->dying before the
     * teardown. */
    if (current_process->dying) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    struct cpu_info *cpu = get_cpu_info();
    char *k_title = cpu->syscall_buf;
    if (arch_copy_string_from_user(k_title, (const char *)arg4, 64) != 0) {
      pt_regs_set_return(frame, -EFAULT);
      break;
    }
    pt_regs_set_return(frame, compositor_create_window(
                                  (int)arg0, (int)arg1, (int)arg2, (int)arg3,
                                  k_title, current_process->pid));
  } break;
  case SYS_WINDOW_DRAW:
    compositor_draw_rect((int)arg0, (int)arg1, (int)arg2, (int)arg3, (int)arg4,
                         (uint32_t)arg5, current_process->pid);
    pt_regs_set_return(frame, 0);
    break;
  case SYS_WINDOW_WRITE:
    /* write text to a window by id (#123) — needs CAP_WINDOW.  Replaces the
     * old fd>=100 overload on write(). */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    pt_regs_set_return(
        frame, window_text_write((int)arg0, (const char *)arg1, (size_t)arg2));
    break;
  case SYS_WINDOW_OF_PID: {
    /* Read-only: the compositor window id of a pid, or 0 if it has none.
     * The shell uses it to tell a windowless (run-in-shell) program from one
     * that opened its own window (#123).  No capability needed. */
    extern int compositor_get_window_by_pid(int pid);
    int w = compositor_get_window_by_pid((int)arg0);
    pt_regs_set_return(frame, w > 0 ? w : 0);
    break;
  }
  case SYS_WINDOW_GRID: {
    /* Read-only: terminal character grid of a window, packed (cols<<16)|rows.
     * A windowed TTY app (kilo) queries this to size itself to the compositor
     * font cell instead of assuming a fixed 80x25.  -EINVAL if no such window.
     */
    extern int compositor_window_grid(int win_id, int *cols, int *rows);
    int cols = 0, rows = 0;
    if (compositor_window_grid((int)arg0, &cols, &rows) != 0)
      pt_regs_set_return(frame, -EINVAL);
    else
      pt_regs_set_return(frame,
                         ((long)(cols & 0xFFFF) << 16) | (rows & 0xFFFF));
    break;
  }
  case SYS_DISPLAY_INFO: {
    /* Read-only: current desktop size, packed (w<<16)|h (GFX-DYN-01). */
    extern void compositor_get_size(int *w, int *h);
    int w = 0, h = 0;
    compositor_get_size(&w, &h);
    pt_regs_set_return(frame, ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF));
    break;
  }
  case SYS_SET_DISPLAY_MODE: {
    /* GFX-DYN-01: change resolution at runtime; the desktop adapts.  A
     * system-level action → needs CAP_WINDOW. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    extern int gpu_set_mode(int w, int h);
    extern void compositor_resize(int w, int h);
    extern void compositor_set_native_mode(int w, int h);
    int r = gpu_set_mode((int)arg0, (int)arg1);
    if (r == 0) {
      compositor_set_native_mode((int)arg0,
                                 (int)arg1); /* new zoom-100 reference */
      compositor_resize((int)arg0, (int)arg1);
    }
    pt_regs_set_return(frame, r);
    break;
  }
  case SYS_WINDOW_RESIZE: {
    /* Resize a window's logical surface.  Needs CAP_WINDOW and ownership
     * (owner or machine), mirroring SYS_DESTROY_WINDOW's ABI-04 check. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    extern int compositor_window_owner(int window_id);
    extern int compositor_resize_window(int window_id, int w, int h);
    int owner = compositor_window_owner((int)arg0);
    if (owner >= 0 && owner != (int)current_process->pid &&
        !proc_is_machine(current_process)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    pt_regs_set_return(
        frame, compositor_resize_window((int)arg0, (int)arg1, (int)arg2));
    break;
  }
  case SYS_DISPLAY_POLL: {
    /* Apply a pending host display-change in process context (init's supervisor
     * loop polls this so the heavy set_mode/realloc never runs in the IRQ
     * tick). Machine-level only.  Returns 1 if the desktop was resized, else 0.
     */
    if (!proc_is_machine(current_process)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    extern int gpu_poll_events(int *w, int *h);
    extern int gpu_set_mode(int w, int h);
    extern void compositor_resize(int w, int h);
    extern void compositor_set_native_mode(int w, int h);
    int w = 0, h = 0;
    if (gpu_poll_events(&w, &h) == 1) {
      if (gpu_set_mode(w, h) == 0) {
        compositor_set_native_mode(w, h);
        compositor_resize(w, h);
      }
      pt_regs_set_return(frame, 1);
    } else {
      pt_regs_set_return(frame, 0);
    }
    break;
  }
  case SYS_SET_STYLE: {
    /* Switch the compositor look (Style/Theme/Background).  Needs CAP_WINDOW.
     * arg0 = style id, arg1 = theme id, arg2 = bg id; pass -1 to keep. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    extern int compositor_set_style(int style_id);
    extern int compositor_set_theme(int theme_id);
    extern int compositor_set_background(int bg_id);
    int rc = 0;
    /* Publish the change into the registry ON SUCCESS, right here, so
     * style.name/theme.color/background.name are the single coherent
     * getter for the whole desktop (S-ALIGN): no caller-side "set the
     * compositor, then remember to also write the registry" step to
     * forget (nxsettings used to skip it, leaving every OTHER app showing
     * the stale theme after a GUI change).  owner_pid 0 = system: the
     * kernel is the canonical writer of these three keys regardless of
     * which process asked for the switch. */
    if ((int)arg0 >= 0) {
      int r = compositor_set_style((int)arg0);
      rc |= r;
      if (r == 0 && (int)arg0 < OS1_STYLE_COUNT)
        registry_set("style.name", os1_style_names[(int)arg0], 0);
    }
    if ((int)arg1 >= 0) {
      int r = compositor_set_theme((int)arg1);
      rc |= r;
      if (r == 0 && (int)arg1 < OS1_THEME_COUNT)
        registry_set("theme.color", os1_theme_names[(int)arg1], 0);
    }
    if ((int)arg2 >= 0) {
      int r = compositor_set_background((int)arg2);
      rc |= r;
      if (r == 0 && (int)arg2 < OS1_BG_COUNT)
        registry_set("background.name", os1_bg_names[(int)arg2], 0);
    }
    pt_regs_set_return(frame, rc);
    break;
  }
  case SYS_SET_ZOOM: {
    /* Desktop zoom percent (HiDPI/zoom, F2).  Needs CAP_WINDOW. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    extern int compositor_set_zoom(int percent);
    pt_regs_set_return(frame, compositor_set_zoom((int)arg0));
    break;
  }
  case SYS_COMPOSITOR_RENDER:
    /* The single compositor-push syscall (the duplicate SYS_FLUSH was retired
     * and folded here).  Forcing a global re-render is a display effect: gated
     * by CAP_WINDOW, so a capability-stripped worker (spawned without
     * CAP_WINDOW) cannot drive the compositor.  Every default preset (incl.
     * GUEST) holds CAP_WINDOW, and machine processes bypass — so nothing
     * legitimate breaks. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    compositor_render();
    pt_regs_set_return(frame, 0);
    break;
  case SYS_WINDOW_BLIT:
    compositor_blit((int)arg0, (int)arg1, (int)arg2, (int)arg3, (int)arg4,
                    (const uint32_t *)arg5, current_process->pid);
    /* This process now owns custom pixel content in a window's buffer —
     * see struct process.self_rendered (sched.h) for why the own-window-
     * first stdout path (window_text_write below) must stop drawing text
     * into it from here on.  compositor_blit() already refused the call
     * above if the window is not this process's own (or init's), so
     * reaching here means the blit legitimately landed on OUR window. */
    current_process->self_rendered = 1;
    pt_regs_set_return(frame, 0);
    break;
  case SYS_WINDOW_SET_FLAGS: {
    /* Window flags (top-most / hide / click-through) are a property of the
     * window: only its owner — or a machine process — may set them, mirroring
     * SYS_DESTROY_WINDOW.  Previously ungated, so any process could top-most or
     * hide any window.  All legitimate callers (nxui/nxlauncher/notify_srv) set
     * flags on their OWN window, so the owner check breaks nothing. */
    extern int compositor_window_owner(int window_id);
    int fowner = compositor_window_owner((int)arg0);
    if (fowner >= 0 && fowner != (int)current_process->pid &&
        !proc_is_machine(current_process)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    compositor_set_window_flags((int)arg0, (int)arg1);
    pt_regs_set_return(frame, 0);
  } break;
  case SYS_DESTROY_WINDOW: {
    /* ABI-04: only the window's owner (or a system process) may destroy it.
     * Kernel-internal teardown (close button, process exit) calls
     * compositor_destroy_window() directly and is unaffected. */
    extern int compositor_window_owner(int window_id);
    int owner = compositor_window_owner((int)arg0);
    if (owner >= 0 && owner != (int)current_process->pid &&
        !proc_is_machine(current_process)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    compositor_destroy_window((int)arg0);
    pt_regs_set_return(frame, 0);
  } break;
  case SYS_SBRK:
    pt_regs_set_return(frame, sys_sbrk((intptr_t)arg0));
    break;
  case SYS_SPAWN: {
    /* USR-SEC-03 #79: spawning needs CAP_SPAWN.  A plain spawn yields a full
     * PLVL_USER child (clamped to the creator), preserving today's behaviour.
     */
    if (!proc_has_cap(current_process, CAP_SPAWN)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    /* #193 hardening: unknown spawn-flag bits are rejected, not ignored —
     * the spawn surface must fail closed on semantics it does not know. */
    if (((uint32_t)arg3 & ~(uint32_t)SPAWN_FLAGS_ALL) != 0) {
      pt_regs_set_return(frame, -EINVAL);
      break;
    }
    struct cpu_info *cpu = get_cpu_info();
    char *k_path = cpu->syscall_buf;
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -EFAULT);
      break;
    }
    /* Optional argv vector: arg1 = argc, arg2 = user array of char* (#kilo).
     * Copy the pointer array and each string into kernel memory before the
     * spawn so process_load_elf_args() can place them on the child's stack. */
    int argc = (int)arg1;
    if (argc < 0)
      argc = 0;
    if (argc > SPAWN_MAX_ARGS)
      argc = SPAWN_MAX_ARGS;
    char *kargv[SPAWN_MAX_ARGS];
    char *argv_store = NULL;
    if (argc > 0) {
      void *uptrs[SPAWN_MAX_ARGS];
      if (arch_copy_from_user(uptrs, (const void *)arg2,
                              (size_t)argc * sizeof(void *)) != 0) {
        pt_regs_set_return(frame, -EFAULT);
        break;
      }
      argv_store = kmalloc((size_t)argc * SPAWN_ARG_LEN);
      if (!argv_store) {
        pt_regs_set_return(frame, -ENOMEM);
        break;
      }
      int bad = 0;
      for (int i = 0; i < argc; i++) {
        kargv[i] = argv_store + (size_t)i * SPAWN_ARG_LEN;
        if (arch_copy_string_from_user(kargv[i], (const char *)uptrs[i],
                                       SPAWN_ARG_LEN) != 0) {
          bad = 1;
          break;
        }
      }
      if (bad) {
        kfree(argv_store);
        pt_regs_set_return(frame, -EFAULT);
        break;
      }
    }
    /* arg3 = spawn-mode flags (SPAWN_FLAG_*, caps.h) — nxexec model #193. */
    long sret =
        dispatch_spawn(k_path, PLVL_USER, 0, 0, argc, kargv, (uint32_t)arg3);
    if (argv_store)
      kfree(argv_store);
    pt_regs_set_return(frame, sret);
  } break;
  case SYS_SPAWN_CAPS: {
    /* spawn_caps(path, level, caps) — restricted spawn.  The requested level
     * and caps are clamped monotonically in process_create_caps (never more
     * privileged than the creator, never above the level ceiling, never more
     * than the creator holds). */
    if (!proc_has_cap(current_process, CAP_SPAWN)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    /* #193 hardening: same strict flag validation as SYS_SPAWN. */
    if (((uint32_t)arg3 & ~(uint32_t)SPAWN_FLAGS_ALL) != 0) {
      pt_regs_set_return(frame, -EINVAL);
      break;
    }
    struct cpu_info *cpu = get_cpu_info();
    char *k_path = cpu->syscall_buf;
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -EFAULT);
      break;
    }
    pt_regs_set_return(frame,
                       dispatch_spawn(k_path, (uint8_t)arg1, (uint32_t)arg2, 1,
                                      0, NULL, (uint32_t)arg3));
  } break;
  case SYS_KILL:
    /* ABI-04: a process may kill itself or its descendants (orphans are
     * re-homed to a live ancestor at reap time, SCHED-DOS-02); SYSTEM/ROOT
     * may kill anything (process_terminate still protects SYSTEM targets). */
    if (!process_kill_allowed(current_process, (int)arg0)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    /* External kill of ANOTHER process is window-aware (kill the target + its
     * windowless/in-shell descendants, SPARE windowed apps —
     * PROCESS-KILL-MODEL). Killing SELF stays single-process, like exit. */
    if (current_process && (int)arg0 == (int)current_process->pid) {
      pt_regs_set_return(frame, process_terminate((int)arg0));
    } else {
      process_kill_subtree((int)arg0);
      pt_regs_set_return(frame, 0);
    }
    break;
  case SYS_GETPROCS:
    pt_regs_set_return(frame, sys_getprocs((void *)arg0, (size_t)arg1));
    break;
  case SYS_SYSSTATS:
    /* OS1_sys_stats(buf, buf_size): one os1_sysstats snapshot (perf §1).
     * PRIVILEGED introspection: only ROOT/machine NX services (e.g.
     * /sys/bin/nxmemstat) may read the raw stats; USER apps go through the
     * service's exposed interface, never this syscall directly. */
    if (!current_process || current_process->level > PLVL_ROOT) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    pt_regs_set_return(frame, sys_sysstats((void *)arg0, (size_t)arg1));
    break;
  case SYS_GET_IDENTITY: {
    /* Read-only self-introspection: the caller's own privilege LEVEL and
     * capability mask, packed (level<<16)|caps.  Ungated — a process may always
     * learn its OWN identity.  Backs OS1_identity()/nxperm and lets userland
     * present a level-based view (machine/root/user/guest) without exposing the
     * raw capability bits to applications. */
    int lvl = current_process ? (int)current_process->level : PLVL_GUEST;
    unsigned int cps = current_process ? current_process->caps : 0u;
    pt_regs_set_return(frame,
                       ((long)(lvl & 0xFF) << 16) | (long)(cps & 0xFFFF));
    break;
  }
  case SYS_YIELD:
    return sys_yield(frame);
  case SYS_NANOSLEEP:
    return sys_nanosleep(frame, arg0);
  case SYS_SEND: {
    /* ABI-05 RESOLVED: capture the result in a local instead of trying to
     * re-read it through the (read-only) argument accessors, so the
     * yield-after-successful-send actually happens and the receiver gets
     * a chance to run immediately. */
    long rc = sys_ipc_send((int)arg0, (void *)arg1);
    pt_regs_set_return(frame, rc);
    if (rc == 0)
      return schedule(frame);
    break;
  }
  case SYS_RECV: {
    /* IPC-01: when sys_ipc_recv() blocks it arms a syscall retry (PC rewound
     * to the SVC/SYSCALL).  The return value must NOT be written then — on
     * aarch64 x0 is both the return register and arg0, so writing it would
     * clobber src_pid for the re-executed syscall (the receiver re-armed
     * with src_pid=0 and slept forever on a non-empty queue).
     * NOTE(IPC-02): still unconditionally schedules — a delivered message
     * costs an extra yield. */
    long rc = sys_ipc_recv((int)arg0, (void *)arg1);
    if (rc != IPC_RECV_RETRY)
      pt_regs_set_return(frame, rc);
    return schedule(frame);
  }
  case SYS_TRY_RECV:
    pt_regs_set_return(frame, sys_ipc_try_recv((int)arg0, (void *)arg1));
    break;
  case SYS_SET_FOCUS:
    /* ABI-04 / USR-SEC-03 #79: claiming focus needs CAP_WINDOW; a process may
     * only claim focus for ITSELF (every userland caller does
     * set_focus(get_pid())); redirecting input to/from another PID — i.e.
     * keystroke stealing — needs machine level. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    if ((int)arg0 != (int)current_process->pid &&
        !proc_is_machine(current_process)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    sched_set_focus_pid(
        (int)arg0); /* push the focus hint to the scheduler (#67) */
    /* Caret follows the input window: clear it off whoever just lost focus. */
    {
      extern void compositor_focus_changed(int new_pid);
      compositor_focus_changed((int)arg0);
    }
    pt_regs_set_return(frame, 0);
    break;
  case SYS_WAIT:
    pt_regs_set_return(frame, process_wait((int)arg0));
    break;
  case SYS_REGISTRY:
    pt_regs_set_return(frame, sys_registry((int)arg0, (const char *)arg1,
                                           (char *)arg2, (size_t)arg3));
    break;
  case SYS_FILE_WRITE: {
    struct cpu_info *cpu = get_cpu_info();
    char *k_path = cpu->syscall_buf;
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -EFAULT);
      break;
    }
    char resolved_path[128];
    vfs_resolve_path(k_path, resolved_path, 128);
    /* Single write-authority seam (S-ALIGN F6): CAP_FS_WRITE + /sys,/bin ACL
     * live in vfs_write_allowed(), shared with SYS_UNLINK and open-for-write.
     */
    long wperm = vfs_write_allowed(resolved_path);
    if (wperm != 0) {
      pt_regs_set_return(frame, wperm);
      break;
    }
    /* File CREATION (issue #126, NOTE(M4.5-FS-WRITE)): creation authority
     * IS write authority for the path — vfs_write_allowed above already
     * applied the whole tree ACL (/home open to users, guest confined to
     * /home/shared, /sys/bin machine-only, every other tree root/machine).
     * The old extra machine-only gate predates /home and kept kilo/doom
     * from ever creating a file; the tree policy is the single seam now. */
    struct vfs_stat wst;
    if (vfs_stat(resolved_path, &wst) != 0) {
      if (vfs_create(resolved_path, VFS_TYPE_FILE) != 0) {
        pt_regs_set_return(frame, -EIO);
        break;
      }
    }
    size_t size = (size_t)arg2;
    if (size >
        SYSCALL_MAX_IO_BYTES) { /* FIX(EXT4-07): reject absurd user size */
      pt_regs_set_return(frame, -EINVAL);
      break;
    }
    if (size == 0) {
      /* Zero-length write: nothing to copy (falling through to kmalloc(0)
       * returns NULL and would fail a valid POSIX zero-write with -ENOMEM).
       * The file was created/already exists above.  A from-start (offset 0)
       * zero write still truncates the file to empty through the provider —
       * the create/truncate-empty idiom (fopen("w"), nxfilem, an editor saving
       * a now-empty buffer); a non-zero offset zero write is a pure no-op. */
      if ((uint32_t)arg3 == 0)
        (void)vfs_write_file(resolved_path, "", 0, 0);
      pt_regs_set_return(frame, 0);
      break;
    }
    uint8_t *k_buf = kmalloc(size);
    if (!k_buf) {
      pt_regs_set_return(frame, -ENOMEM);
      break;
    }
    if (arch_copy_from_user(k_buf, (const void *)arg1, size) != 0) {
      kfree(k_buf);
      pt_regs_set_return(frame, -EFAULT);
      break;
    }
    uint32_t offset = (uint32_t)arg3;
    int wr = vfs_write_file(resolved_path, k_buf, (uint32_t)size, offset);
    pt_regs_set_return(frame, wr < 0 ? -EIO : wr);
    kfree(k_buf);
  } break;
  case SYS_FILE_READ: {
    char k_path[128];
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -EFAULT);
      break;
    }
    char resolved_path[128];
    vfs_resolve_path(k_path, resolved_path, 128);
    size_t size = (size_t)arg2;
    if (size >
        SYSCALL_MAX_IO_BYTES) { /* FIX(EXT4-07): reject absurd user size */
      pt_regs_set_return(frame, -EINVAL);
      break;
    }
    uint32_t offset = (uint32_t)arg3;
    long ret;

    if (size == 0) {
      int probed = vfs_read_file(resolved_path, NULL, 0, offset);
      ret = probed < 0 ? -ENOENT : probed;
    } else {
      uint8_t *k_buf = kmalloc(size);
      if (!k_buf) {
        pt_regs_set_return(frame, -ENOMEM);
        break;
      }

      int bytes_read =
          vfs_read_file(resolved_path, k_buf, (uint32_t)size, offset);
      if (bytes_read < 0) {
        ret = -ENOENT; /* missing path is by far the dominant failure */
      } else if (arch_copy_to_user((void *)arg1, k_buf, bytes_read) != 0) {
        ret = -EFAULT;
      } else {
        ret = bytes_read;
      }
      kfree(k_buf);
    }
    pt_regs_set_return(frame, ret);
  } break;
  case SYS_SET_FONT:
    /* Replacing the GLOBAL system font is a desktop-wide display change (it
     * affects every window): same capability gate as set_style/set_zoom
     * (CAP_WINDOW).  Previously ungated — any process could restyle the
     * desktop. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      pt_regs_set_return(frame, -EPERM);
      break;
    }
    pt_regs_set_return(frame, sys_set_font((void *)arg0, (size_t)arg1));
    break;
  case SYS_LIST_DIR: {
    char k_path[128];
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -EFAULT);
      break;
    }
    char resolved_path[128];
    vfs_resolve_path(k_path, resolved_path, 128);
    size_t size = (size_t)arg2;
    if (size >
        SYSCALL_MAX_IO_BYTES) { /* FIX(EXT4-07): reject absurd user size */
      pt_regs_set_return(frame, -EINVAL);
      break;
    }
    char *k_buf = kmalloc(size);
    if (!k_buf) {
      pt_regs_set_return(frame, -ENOMEM);
      break;
    }
    long ret;
    int res = vfs_list_dir(resolved_path, k_buf, (uint32_t)size);
    if (res < 0) {
      ret = -ENOENT;
    } else if (arch_copy_to_user((void *)arg1, k_buf, res + 1) != 0) {
      ret = -EFAULT;
    } else {
      ret = res;
    }
    kfree(k_buf);
    pt_regs_set_return(frame, ret);
  } break;
  case SYS_CHDIR: {
    char k_path[128];
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -EFAULT);
      break;
    }
    char resolved_path[128];
    vfs_resolve_path(k_path, resolved_path, 128);

    /* Verify it exists and is a directory. */
    struct vfs_stat st;
    if (vfs_stat(resolved_path, &st) != 0) {
      pt_regs_set_return(frame, -ENOENT);
    } else if (st.type != VFS_TYPE_DIR) {
      pt_regs_set_return(frame, -ENOTDIR);
    } else {
      strncpy(current_process->cwd, resolved_path, 128);
      pt_regs_set_return(frame, 0);
    }
  } break;
  case SYS_GETCWD: {
    size_t size = (size_t)arg1;
    if (arch_copy_to_user((void *)arg0, current_process->cwd, size) != 0) {
      pt_regs_set_return(frame, -EFAULT);
    } else {
      pt_regs_set_return(frame, 0);
    }
  } break;
  case SYS_UNLINK: {
    char k_path[128];
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -EFAULT);
      break;
    }
    char resolved_path[128];
    vfs_resolve_path(k_path, resolved_path, 128);
    /* unlink is a write-class modification: same single authority seam as
     * SYS_FILE_WRITE / open(write) (vfs_write_allowed, S-ALIGN F6).  Closes
     * the asymmetry (USR-SEC) where any process could unlink a path it was
     * not allowed to write. */
    long uperm = vfs_write_allowed(resolved_path);
    if (uperm != 0) {
      pt_regs_set_return(frame, uperm);
      break;
    }
    pt_regs_set_return(frame, vfs_unlink(resolved_path));
  } break;
  case SYS_MKDIR: {
    char k_path[128];
    if (arch_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      pt_regs_set_return(frame, -EFAULT);
      break;
    }
    char resolved_path[128];
    vfs_resolve_path(k_path, resolved_path, 128);
    /* mkdir is a write-class modification: same single authority seam as
     * SYS_FILE_WRITE / SYS_UNLINK (vfs_write_allowed, S-ALIGN F6), so the
     * three entry points cannot drift. */
    long mperm = vfs_write_allowed(resolved_path);
    if (mperm != 0) {
      pt_regs_set_return(frame, mperm);
      break;
    }
    pt_regs_set_return(frame, vfs_create(resolved_path, VFS_TYPE_DIR));
  } break;
  /* --- Object / capability ABI (ASTRA §6.1/6.2/6.5, kernel/object.h) ---
   * The real capability layer: unforgeable per-process handles to refcounted
   * kernel objects with separable/attenuable rights.  User pointers (path,
   * buf) are validated inside each backend via arch_copy_*_user. */
  case SYS_HANDLE_CREATE:
    pt_regs_set_return(frame, sys_handle_create((int)arg0, (const char *)arg1,
                                                (uint32_t)arg2, (int)arg3));
    break;
  case SYS_HANDLE_DUP:
    pt_regs_set_return(frame, sys_handle_dup((int)arg0, (uint32_t)arg1));
    break;
  case SYS_HANDLE_CLOSE:
    pt_regs_set_return(frame, sys_handle_close((int)arg0));
    break;
  case SYS_CAP_QUERY:
    pt_regs_set_return(frame, sys_cap_query((int)arg0));
    break;
  case SYS_CAP_GRANT:
    pt_regs_set_return(frame,
                       sys_cap_grant((int)arg0, (int)arg1, (uint32_t)arg2));
    break;
  case SYS_OBJECT_READ:
    pt_regs_set_return(frame,
                       sys_object_read((int)arg0, (void *)arg1, (size_t)arg2));
    break;
  case SYS_OBJECT_WRITE:
    pt_regs_set_return(
        frame, sys_object_write((int)arg0, (const void *)arg1, (size_t)arg2));
    break;
  case SYS_OBJECT_WAIT:
    pt_regs_set_return(frame, sys_object_wait((int)arg0, (long)arg1));
    break;
  case SYS_OBJECT_CTL:
    pt_regs_set_return(frame, sys_object_ctl((int)arg0, (int)arg1, (long)arg2));
    break;
  default:
    pr_warn("Unknown syscall: %ld\n", syscall_num);
    pt_regs_set_return(frame, -ENOSYS);
    break;
  }

  return frame;
}

/*
 * sys_get_time - return monotonic time in milliseconds.
 *
 * Derived from mono_ns() (the real-time hardware-counter clock, docs/
 * TIMER-MODEL.md), so it is accurate on BOTH arches — amd64 no longer returns
 * the jiffies*1000 stub (ARCH-03 retired for the real-time path).
 *
 * Locking: none.  IRQ context: no.
 */
long sys_get_time(void) { return (long)(mono_ns() / 1000000ULL); }

/*
 * sys_clock_gettime - Tier 3 userland clock (SYS_CLOCK_GETTIME).
 *
 * Returns a 64-bit nanosecond value directly (both arches return 64 bits):
 *   clk == 0 (MONOTONIC)  : mono_ns() — monotonic ns since boot.
 *   clk == 1 (PROCESS_CPU): the caller's consumed CPU time in real ns.
 * Any other clock id falls back to MONOTONIC. This is the minimal seL4-style
 * mechanism; POSIX clock_gettime() is built on top in <time.h>. Capability
 * timer OBJECTS (arm-and-notify) are issue #135.
 */
long sys_clock_gettime(int clk) {
  if (clk == 1) {
    struct process *p = current_process;
    return p ? (long)timer_counts_to_ns(p->cpu_time_counts) : 0;
  }
  return (long)mono_ns();
}

/* Global monotonic tick counter (drivers/timer.c / platform.c). */
extern volatile uint64_t jiffies;

/*
 * proc_sleep_wake - per-process sleep timer callback (fired by the owning
 * core's tick when the coarse deadline is reached). Makes the sleeper runnable
 * again; it will re-run sys_nanosleep (retry), test mono_ns() >= wake_ns, and
 * either return (deadline reached) or re-arm and sleep again (woken a tick
 * early). Runs under that CPU's timer_lock; enqueue_task takes only a per-CPU
 * sched_lock (lock order timer_lock > per-CPU sched_lock — no inversion).
 */
static void proc_sleep_wake(void *data) {
  struct process *p = (struct process *)data;
  if (p && p->state == PROC_SLEEPING)
    enqueue_task(p);
}

/*
 * sys_nanosleep - block the caller for `ns` nanoseconds (POSIX nanosleep core,
 * Tier 3 of docs/TIMER-MODEL.md).
 *
 * The deadline is stored as an ABSOLUTE real-time instant (p->wake_ns, mono_ns
 * base) on first entry, so the sleep recovers lost time: the per-CPU software
 * timer is only a coarse jiffies-edge trigger, and the precise wake condition
 * is mono_ns() >= wake_ns. Retry-based like the blocking read — re-entered on
 * each wakeup. If the coarse timer fires a tick early, mono_ns() is still short
 * of wake_ns, so we re-arm and sleep again; a genuine deadline returns 0.
 */
static struct pt_regs *sys_nanosleep(struct pt_regs *regs, uint64_t ns) {
  struct process *p = current_process;
  if (!p || ns == 0) {
    pt_regs_set_return(regs, 0);
    return regs;
  }
  if (p->wake_ns == 0)
    p->wake_ns = mono_ns() + ns; /* absolute real-time deadline */

  uint64_t now = mono_ns();
  if (now >= p->wake_ns) {
    p->wake_ns = 0;
    timer_del(&p->sleep_timer); /* cancel if somehow still pending */
    pt_regs_set_return(regs, 0);
    return regs;
  }

  /* (Re)arm the coarse wheel trigger at the deadline's tick. Rounding up by one
   * tick keeps us from busy-spinning if mono_ns() is just shy of wake_ns. */
  if (!timer_pending(&p->sleep_timer)) {
    uint64_t ticks = (p->wake_ns - now + (NS_PER_TICK - 1)) / NS_PER_TICK;
    if (ticks == 0)
      ticks = 1;
    timer_setup(&p->sleep_timer, proc_sleep_wake, p);
    timer_add(&p->sleep_timer, jiffies + ticks);
  }

  arch_local_irq_disable();
  p->state = PROC_SLEEPING;
  arch_local_irq_enable();
  pt_regs_retry_syscall(regs);
  return schedule(regs);
}

/* YIELD_SPIN_BUDGET: how many yield() calls a process may make within a single
 * tick (10 ms at HZ=100) before it is treated as busy-spinning. A program doing
 * real work between yields never approaches this; a `while(1) yield()` spinner
 * (an unoptimised app, or an old-style poll loop) trips it and is descheduled
 * to the next tick, capping its duty cycle so it cannot keep a core at 100% and
 * freeze the system. The bounded focus boost (process.c) stops a focused
 * CPU-hog from STARVING others; this stops a yield-spinner from SATURATING a
 * core. */
#define YIELD_SPIN_BUDGET 64

/*
 * sys_yield - cooperative yield with a per-process anti-spin throttle
 * (SYS_YIELD, docs/TIMER-MODEL.md §4).
 *
 * Normally just reschedules (gives up the rest of the slice). But it counts
 * yields within the current tick; once a process exceeds YIELD_SPIN_BUDGET it
 * is busy-spinning on yield(), so instead of an immediate reschedule it is put
 * to sleep until the next tick via its per-process timer (sleep_timer) — the
 * same mechanism nanosleep uses. The spinner therefore relinquishes the core
 * for a full tick, dropping its CPU duty cycle to near zero. The counter resets
 * each tick (and after a throttle), so legitimate cooperative yielding is
 * untouched.
 *
 * Note: this does NOT touch wake_ns (the nanosleep deadline) — a process cannot
 * be both yielding and nanosleeping. On wake the yield() simply returns.
 */
static struct pt_regs *sys_yield(struct pt_regs *regs) {
  struct process *p = current_process;
  if (!p)
    return schedule(regs);

  if (p->yield_jiffy != jiffies) {
    p->yield_jiffy = jiffies;
    p->yield_count = 0;
  }

  if (++p->yield_count > YIELD_SPIN_BUDGET) {
    /* Busy-spinning on yield() this tick — throttle to a 1-tick per-process
     * timed sleep. Reset the budget; it refills next tick on wake. */
    p->yield_count = 0;
    /* TIMER-UAF-01: only (re)arm when the per-process timer is NOT already
     * queued — exactly like sys_nanosleep. The sleeper can have been
     * force-woken by a non-timer path (kernel_ipc_send flips
     * PROC_SLEEPING->READY WITHOUT cancelling sleep_timer, process.c) with its
     * timer still linked; calling timer_setup()/INIT_LIST_HEAD on that
     * still-linked node re-initialises a live list entry and corrupts the
     * per-CPU timer_list, later faulting in kernel_timer_tick() as a double
     * list_del. If it is already pending, the existing 1-tick trigger will wake
     * us, so we just re-sleep. */
    if (!timer_pending(&p->sleep_timer)) {
      timer_setup(&p->sleep_timer, proc_sleep_wake, p);
      timer_add(&p->sleep_timer, jiffies + 1);
    }
    arch_local_irq_disable();
    p->state = PROC_SLEEPING;
    arch_local_irq_enable();
  }

  return schedule(regs);
}

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
    pr_debug("PID %d exiting with status %d\n", current_process->pid,
             status); /* hot path: demoted (perf §1) */
    process_terminate(current_process->pid);
  }
}
