/*
 * kernel/core/syscall_dispatch.c
 * Architecture-Agnostic Syscall Dispatcher
 *
 * This file is the central syscall switch for OS1/NEXS.  The arch-specific
 * syscall entry stub (aarch64: svc_handler / amd64: syscall entry in cpu.c)
 * calls kernel_syscall_dispatcher() with the saved register frame.  This
 * function reads the syscall number and arguments from the frame via the
 * hal_frame_* accessor macros (arch-agnostic) and dispatches to the appropriate
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
 * arch_copy_*_from_user or hal_copy_string_from_user before being dereferenced
 * in the kernel.
 *   - Syscall implementations must not return directly; they write the return
 *     value via hal_frame_set_return() and fall through to "return frame", OR
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
 *   ABI-07  creation now keeps path/argv state call-local and leaves IRQs
 *           enabled across ELF I/O; the providers retain their own locks.
 *   GFX-FONT-01  (W4 SECURITY/BUG) SYS_SET_FONT: stores a raw user
 *           pointer into kernel globals; dereferenced in IRQ-context rendering
 *           (sys_set_font in graphics/font.c) → UAF / info-leak.
 */
#include <hal/frame.h>
#include <drivers/input.h>
#include <graphics/gl.h>
#include <kernel/gfx_surface.h>
#include <kernel/cpu.h>
#include <kernel/kmalloc.h>
#include <kernel/graphics.h>
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
static hal_frame_t *sys_nanosleep(hal_frame_t *regs, uint64_t ns);
/* Yield with per-process anti-spin throttle; used by the SYS_YIELD case. */
static hal_frame_t *sys_yield(hal_frame_t *regs);

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

extern int compositor_create_window(int x, int y, int w, int h,
                                    const char *title, struct process *owner);
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

/* User-memory access is a HAL primitive with per-architecture providers, like
 * arch_bus_scan/arch_irq_init (kernel/hal.h).  These used to be hand-rolled
 * `extern`s right here, which is why the two providers had nothing binding them
 * to the same semantics.  The contract now lives in one place. */
#include <kernel/hal_uaccess.h>
#include <os1_compositor.h>
#include <os1_input.h>

extern int keyboard_focus_pid;

/*
 * kernel_syscall_dispatcher - dispatch a syscall from the saved register frame.
 *
 * Entry point called by the arch-specific svc/syscall handler immediately
 * after saving all user registers into 'frame'.  Reads syscall_num and up
 * to six arguments from frame via hal_frame_* accessors.
 *
 * Returns: a pt_regs* to restore.  In the common (non-blocking) case this is
 *          'frame' itself with the return value written via
 * hal_frame_set_return(). For blocking operations (EXIT/YIELD/IPC RECV and
 * sometimes IPC SEND) this is the frame of the next scheduled process.
 *
 * Locking: no locks held on entry; individual cases may acquire
 *          sched_lock / msg_lock / per-CPU sched_lock internally.
 * IRQ context: no — syscalls run in kernel mode with IRQs enabled (normal
 *          exception-level transition on aarch64; ring 3->0 on amd64).
 *
 * Spawn path/argv copies are call-local across preemption during ELF I/O.
 */
hal_frame_t *kernel_syscall_dispatcher(hal_frame_t *frame);

/* argv marshalling limits for SYS_SPAWN.  ELF_MAX_ARGS in elf.c must be >=
 * SPAWN_MAX_ARGS; SPAWN_ARG_LEN bounds each NUL-terminated argument. */
#define SPAWN_MAX_ARGS 16
#define SPAWN_ARG_LEN 128

/* level_for_path - transitional capability preset from the executable VFS
 * location. /sys/services and /sys/bin are ROOT system code; /sbin is MACHINE;
 * /bin is USER.  Service capability manifests replace this path-derived policy
 * in the object/capability migration.  This is a CEILING + default, not an
 * escalation:
 * process_create_caps still clamps the child to no more privileged than its
 * creator, so a USER shell launching a /sys/bin binary does NOT gain root.
 * /sys/bin is also write- protected (object.c handle_create denies non-machine
 * writes under /sys,/bin), so the binaries backing this preset are immutable.
 */
static uint8_t level_for_path(const char *path) {
  if (path && (strncmp(path, "/sbin/", 6) == 0 ||
               strncmp(path, "/krnl/extension/", 16) == 0))
    return PLVL_MACHINE;
  if (path && strncmp(path, "/sys/bin/", 9) == 0)
    return PLVL_ROOT;
  if (path && strncmp(path, "/sys/services/", 14) == 0)
    return PLVL_ROOT;
  return PLVL_USER;
}

/* spawn_path_is_sane - reject a spawn path the preset cannot reason about.
 *
 * SPAWN-LVL-01 (found 2026-07-23, audit programme A): level_for_path() prefix-
 * matches the RAW string, but the VFS resolves ".." (vfs.c).  So
 * "/sys/bin/../../home/evil" prefix-matches /sys/bin/ and would take the ROOT
 * preset while resolving to a user-writable file.  It is NOT an escalation
 * today — the monotonic creator clamp in process_create_caps drags the child
 * back to the (less-privileged) creator's level, so a USER caller gets USER
 * regardless.  But "harmless only because a SECOND, distant check happens to
 * cover it" is exactly the fragility this audit exists to remove: the preset
 * must see the path the VFS will actually open.  No legitimate caller spawns a
 * path containing ".." (verified across the tree), so refusing them costs
 * nothing and makes level_for_path()'s prefix match sound. */
static int spawn_path_is_sane(const char *path) {
  /* The ELF loader resolves VFS paths, not the raw privilege preset string.
   * Only canonical absolute paths can carry a pathname security decision.
   * Only the kernel-created PID1 machine extension may launch /sbin.
   * ROOT/USER/GUEST and unrelated machine processes cannot create a second
   * authority daemon or independently bootstrap init. */
  if (path == NULL || path[0] != '/' || path[1] == '\0' || path[1] == '/')
    return 0;
  /* PID1 is the sole extension authority allowed to bootstrap an /sbin
   * initializer. Other machine processes and all ROOT/USER/GUEST clients
   * are denied before any child object is allocated. */
  if ((strncmp(path, "/sbin/", 6U) == 0 ||
       strncmp(path, "/krnl/extension/", 16U) == 0) &&
      (current_process == NULL || current_process->pid != 1 ||
       !proc_is_machine(current_process)))
    return 0;
  for (uint32_t i = 1U; i < 128U; ++i) {
    char c = path[i];
    if (c == '\0')
      return 1;
    if (c == '/' && (path[i + 1U] == '/' || path[i + 1U] == '\0'))
      return 0;
    if (c == '.' && path[i - 1U] == '/' &&
        (path[i + 1U] == '/' || path[i + 1U] == '\0' ||
         (path[i + 1U] == '.' &&
          (path[i + 2U] == '/' || path[i + 2U] == '\0'))))
      return 0;
  }
  return 0;
}

/* Common creation state is bounded and call-local. A logical owner is an
 * explicit mechanism input; execution policy remains in the requesting service. */
struct spawn_arguments { char *values[SPAWN_MAX_ARGS]; };
struct spawn_options {
  const char *path;
  uint8_t level;
  uint32_t caps, flags;
  int use_caps, argc, nredir, owner_pid;
  struct spawn_arguments arguments;
  const struct spawn_redir *redirections;
};

static int spawn_redirect_one(struct process *child,
                               const struct spawn_redir *redirect) {
  if (redirect->source_pid == 0)
    return process_redirect_child_fd(child, redirect->child_fd,
                                      redirect->parent_fd);
  if (!proc_is_privileged(current_process)) return -EPERM;
  /* Existing PROC-REF-01 obligation: this legacy foreign-table path still
   * needs a process lifetime pin. Do not allocate while holding sched_lock. */
  struct process *source = process_find_by_pid(redirect->source_pid);
  if (source == NULL) return -ESRCH;
  return process_redirect_child_fd_from(source, child, redirect->child_fd,
                                         redirect->parent_fd);
}

static int spawn_prepare(struct process *child,
                           const struct spawn_options *options) {
  if ((options->flags & SPAWN_FLAG_DETACHED) != 0U) child->ctty_win = -1;
  if (options->owner_pid > 0) {
    int assigned = process_set_owner((int)child->pid, options->owner_pid);
    if (assigned != 0) return assigned;
  }
  for (int i = 0; i < SPAWN_MAX_REDIR && i < options->nredir; ++i) {
    int redirected = spawn_redirect_one(child, &options->redirections[i]);
    if (redirected != 0) return redirected;
  }
  return 0;
}

static long spawn_load_and_publish(struct process *child,
                                    const struct spawn_options *options) {
  int prepared = spawn_prepare(child, options);
  if (prepared != 0) {
    process_abort_spawn(child);
    return prepared;
  }
  if (process_load_elf_args(child, options->path, options->argc,
                            options->arguments.values) != 0) {
    process_abort_spawn(child);
    return -ENOENT;
  }
  long pid = (long)child->pid;
  process_finalize_spawn(child);
  return pid;
}

static long spawn_dispatch_options(struct spawn_options *options) {
  if (!spawn_path_is_sane(options->path)) return -EINVAL;
  int entered = process_creation_begin();
  if (entered != 0) return entered;
  uint8_t path_level = level_for_path(options->path);
  if (!options->use_caps || options->level < path_level)
    options->level = path_level;
  /* PROC_CREATED cannot run and kill is deferred until finalize/abort.
   * Creation and providers use their own short locks; ELF I/O must not hold
   * this CPU's interrupts masked for the complete load. */
  struct process *child = options->use_caps ?
      process_create_caps(options->path, PROC_PRIO_USER, options->level,
                            options->caps) :
      process_create(options->path, PROC_PRIO_USER, options->level);
  long result = child == NULL ? -EAGAIN : spawn_load_and_publish(child, options);
  return result;
}

static long dispatch_spawn(const char *path, uint8_t level, uint32_t caps,
                           int use_caps, int argc, char *const kargv[],
                           uint32_t flags, const struct spawn_redir *redir,
                           int nredir) {
  struct spawn_options options = {
      .path = path, .level = level, .caps = caps, .flags = flags,
      .use_caps = use_caps, .argc = argc, .nredir = nredir,
      .redirections = redir
  };
  for (int i = 0; i < SPAWN_MAX_ARGS && i < argc; ++i)
    options.arguments.values[i] = kargv[i];
  return spawn_dispatch_options(&options);
}

struct spawn_user_copy {
  struct os1_spawn_request_v1 request;
  struct spawn_options options;
  char path[128];
  char text[SPAWN_MAX_ARGS][SPAWN_ARG_LEN];
  uint64_t pointers[SPAWN_MAX_ARGS];
  struct spawn_redir redirections[SPAWN_MAX_REDIR];
};

static int spawn_copy_header(struct spawn_user_copy *copy,
                              const struct os1_spawn_request_v1 *user) {
  if (hal_copy_from_user(&copy->request, user, sizeof(copy->request)) != 0)
    return -EFAULT;
  const struct os1_spawn_request_v1 *request = &copy->request;
  if (request->abi_version != OS1_SPAWN_REQUEST_VERSION ||
      request->struct_size != sizeof(*request) || request->reserved != 0U ||
      request->argc > SPAWN_MAX_ARGS || request->nredir > SPAWN_MAX_REDIR ||
      (request->flags & ~(uint32_t)SPAWN_FLAGS_ALL) != 0U) return -EINVAL;
  if (hal_copy_string_from_user(copy->path,
      (const char *)(uintptr_t)request->path, sizeof(copy->path)) != 0)
    return -EFAULT;
  if (request->argc != 0U && hal_copy_from_user(copy->pointers,
      (const void *)(uintptr_t)request->argv,
      request->argc * sizeof(copy->pointers[0])) != 0) return -EFAULT;
  if (request->nredir != 0U && hal_copy_from_user(copy->redirections,
      (const void *)(uintptr_t)request->redirections,
      request->nredir * sizeof(copy->redirections[0])) != 0) return -EFAULT;
  copy->options.path = copy->path;
  copy->options.level = PLVL_USER;
  copy->options.argc = (int)request->argc;
  copy->options.flags = request->flags;
  copy->options.nredir = (int)request->nredir;
  copy->options.redirections = copy->redirections;
  return 0;
}

long kernel_spawn_owned(int owner_pid,
    const struct os1_spawn_request_v1 *urequest) {
  if (owner_pid <= 0 || !proc_is_privileged(current_process) ||
      !proc_has_cap(current_process, CAP_SPAWN)) return -EPERM;
  struct spawn_user_copy copy = {0};
  int status = spawn_copy_header(&copy, urequest);
  if (status != 0) return status;
  for (uint32_t i = 0U; i < SPAWN_MAX_ARGS && i < copy.request.argc; ++i) {
    if (hal_copy_string_from_user(copy.text[i],
        (const char *)(uintptr_t)copy.pointers[i], SPAWN_ARG_LEN) != 0)
      return -EFAULT;
    copy.options.arguments.values[i] = copy.text[i];
  }
  copy.options.owner_pid = owner_pid;
  return spawn_dispatch_options(&copy.options);
}

/* window_text_write - copy a user text buffer into a kmalloc bounce (no
 * truncation; capped at SYSCALL_MAX_IO_BYTES), mirror it to the UART serial
 * log, and append it to compositor window win_id.  Shared by the FD_WIN
 * stdout sink and SYS_WINDOW_WRITE (#123).  Replaces the old 1023-byte
 * syscall_buf truncation (retires ABI-06 on the window path). */
extern void uart_puts(const char *str);
/* Non-static: also the OBJ_TYPE_CONSOLE stdout/stderr backend, called from
 * kernel/core/object.c (sys_object_write).  Shared by SYS_WINDOW_WRITE. */
/*
 * GFX-WIN-WRITE-01 (found 2026-07-23, audit programme A) — may 'current' put
 * text into window 'win_id'?
 *
 * SYS_WINDOW_WRITE was gated by CAP_WINDOW and nothing else, while every
 * sibling verb had already been closed: DRAW and BLIT pass the caller's pid so
 * the compositor can refuse, RESIZE / SET_FLAGS / DESTROY_WINDOW check
 * compositor_window_owner() right in the dispatcher.  This one was missed, and
 * CAP_WINDOW is the WEAKEST capability there is — PLVL_GUEST holds it and
 * nothing else (level_ceiling[], process.c).  So the least privileged process
 * on the machine could write arbitrary text into ANY window, and it did not
 * even have to guess an id: SYS_WINDOW_ENUM is deliberately ungated.  That is a
 * UI-spoofing primitive against the shell, the dock and the notification panel.
 *
 * Three writers are legitimate and all three are expressible as authority the
 * process already holds:
 *   - the window's OWNER;
 *   - a process writing to its CONTROLLING TERMINAL.  This is the one that
 *     makes an owner-only check wrong: a windowless child's stdout goes to the
 *     shell's window (sys_object_write resolves ctty_win), and it does not own
 *     it.  ctty_win is set by the kernel at spawn and is not user-writable, so
 *     honouring it grants nothing the process was not already given;
 *   - a MACHINE process, exactly as SET_FLAGS and DESTROY_WINDOW allow.
 *
 * Locking: called with NO lock held (pin_handle releases object_lock before
 * sys_object_write reaches here, and the dispatcher holds nothing), so taking
 * compositor_lock inside compositor_window_owner() is safe.  Do not move this
 * check under a lock without re-reading PROCESS-KILL-MODEL §4, Pitfall A.
 */
static int window_write_allowed(int win_id) {
  extern int compositor_window_owner(int window_id);
  if (!current_process)
    return 1; /* kernel-internal writer */
  if (proc_is_machine(current_process))
    return 1;
  if (win_id == current_process->ctty_win)
    return 1;
  int owner = compositor_window_owner(win_id);
  return (owner < 0) || (owner == (int)current_process->pid);
}

long window_text_write(int win_id, const char *ubuf, size_t count) {
  if (count == 0)
    return 0;
  if (count > SYSCALL_MAX_IO_BYTES)
    return -EINVAL;
  if (win_id > 0 && !window_write_allowed(win_id))
    return -EPERM;
  char *k = kmalloc(count + 1);
  if (!k)
    return -ENOMEM;
  if (hal_copy_from_user(k, ubuf, count) != 0) {
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

hal_frame_t *kernel_syscall_dispatcher(hal_frame_t *frame) {
  uint64_t syscall_num = hal_frame_syscall_num(frame);
  uint64_t arg0 = hal_frame_arg(frame, 0);
  uint64_t arg1 = hal_frame_arg(frame, 1);
  uint64_t arg2 = hal_frame_arg(frame, 2);
  uint64_t arg3 = hal_frame_arg(frame, 3);
  uint64_t arg4 = hal_frame_arg(frame, 4);
  uint64_t arg5 = hal_frame_arg(frame, 5);

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
     * used for WRITE capability acquisition (S-ALIGN F6), so the POSIX
     * personality logic in
     * libc keeps only flag/mode translation (ASTRA layering).  Any OTHER flag
     * is still an explicit -EINVAL, never silently ignored.  sys_handle_create
     * then does the resolve, the write-ACL re-check for a WRITE handle, and the
     * VFS open. */
    if (!current_process) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    int flags = (int)arg1;
    if (flags & ~(O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND)) {
      hal_frame_set_return(frame, -EINVAL);
      break;
    }
    if (flags & (O_CREAT | O_TRUNC)) {
      char kpath[128];
      if (hal_copy_string_from_user(kpath, (const char *)arg0, sizeof(kpath)) !=
          0) {
        hal_frame_set_return(frame, -EFAULT);
        break;
      }
      char resolved[128];
      vfs_resolve_path(kpath, resolved, sizeof(resolved));
      struct vfs_stat vst;
      int exists = (vfs_stat(resolved, &vst) == 0);
      long wperm;
      if (!exists && (flags & O_CREAT)) {
        if ((wperm = vfs_write_allowed(resolved)) != 0) {
          hal_frame_set_return(frame, wperm);
          break;
        }
        if (vfs_create(resolved, VFS_TYPE_FILE) != 0) {
          hal_frame_set_return(frame, -EIO);
          break;
        }
      } else if (exists && (flags & O_TRUNC)) {
        if ((wperm = vfs_write_allowed(resolved)) != 0) {
          hal_frame_set_return(frame, wperm);
          break;
        }
        /* Preserve inode identity and existing open handles. A length change
         * is a provider operation, never unlink followed by recreation. */
        int truncated = vfs_truncate(resolved, 0U);
        if (truncated != 0) {
          hal_frame_set_return(frame, truncated);
          return frame;
        }
      }
      /* O_TRUNC on a missing file without O_CREAT: fall through — the open
       * below resolves nothing and returns -ENOENT, matching POSIX. */
    }
    uint32_t rights = ((flags & O_ACCMODE) == O_RDONLY) ? OS1_RIGHT_READ
                      : ((flags & O_ACCMODE) == O_WRONLY)
                          ? OS1_RIGHT_WRITE
                          : (OS1_RIGHT_READ | OS1_RIGHT_WRITE);
    hal_frame_set_return(frame, sys_handle_create(OS1_NS_FS, (const char *)arg0,
                                                rights, OBJ_TYPE_FILE));
  } break;
  case SYS_CLOSE:
    /* close(fd) ≡ drop the handle; frees the object at its last reference. */
    hal_frame_set_return(frame, sys_handle_close((int)arg0));
    break;
  case SYS_LSEEK:
    /* lseek(fd, off, whence) ≡ reposition a FILE handle's shared offset; a
     * CONSOLE/stream handle returns -ESPIPE (handled in sys_object_lseek). */
    hal_frame_set_return(frame,
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
      hal_irq_disable();
      current_process->ipc_target_pid = -1; /* wake on ANY message */
      current_process->state = PROC_SLEEPING;
      hal_irq_enable();
      hal_frame_retry_syscall(frame);
      return schedule(frame);
    }
    hal_frame_set_return(frame, (r == -EPERM) ? -EBADF : r);
  } break;
  case SYS_WRITE: {
    /* write(fd, buf, n) ≡ OS1_object_write on the handle.  -EPERM (write of a
     * read-only handle, e.g. stdin) maps to the POSIX -EBADF. */
    long r = sys_object_write((int)arg0, (const void *)arg1, (size_t)arg2);
    hal_frame_set_return(frame, (r == -EPERM) ? -EBADF : r);
  } break;
  case SYS_EXIT:
    sys_exit((int)arg0);
    return schedule(frame);
  case SYS_GET_TIME:
    hal_frame_set_return(frame, sys_get_time());
    break;
  case SYS_CLOCK_GETTIME:
    hal_frame_set_return(frame, sys_clock_gettime((int)arg0));
    break;
  case SYS_GETPID:
    hal_frame_set_return(frame, sys_get_pid());
    break;
  case SYS_DRAW:
    /* Direct draw to the root framebuffer is a display effect: gated by
     * CAP_WINDOW (the level that "may draw" — guests included).  Previously
     * ungated.  A capability-stripped worker without CAP_WINDOW cannot scribble
     * on the screen; machine bypasses. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    if (arg0 > UINT32_MAX || arg1 > UINT32_MAX ||
        arg2 == 0U || arg2 > UINT32_MAX ||
        arg3 == 0U || arg3 > UINT32_MAX) {
      hal_frame_set_return(frame, -EINVAL);
      break;
    }
    graphics_draw_rect((uint32_t)arg0, (uint32_t)arg1,
                       (uint32_t)arg2, (uint32_t)arg3, (uint32_t)arg4);
    hal_frame_set_return(frame, 0);
    break;
  case SYS_WINDOW_ENUM: {
    /* Read-only window enumeration → struct window_info[] (ASTRA §6.7); ungated
     * like SYS_GETPROCS.  The dock /sys/bin/nxui lays out its app list from it.
     */
    extern long sys_window_enum(struct window_info * ubuf, size_t max);
    hal_frame_set_return(
        frame, sys_window_enum((struct window_info *)arg0, (size_t)arg1));
    break;
  }
  case SYS_CREATE_WINDOW: {
    /* USR-SEC-03 #79: drawing a window needs CAP_WINDOW. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    /* SCHED-UAF: a process being torn down must not create a NEW window after
     * process_terminate already destroyed its windows — it would be orphaned
     * (owner dead -> un-closeable).  The killer sets ->dying before the
     * teardown. */
    if (__atomic_load_n(&current_process->dying, __ATOMIC_ACQUIRE)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    struct cpu_info *cpu = get_cpu_info();
    char *k_title = cpu->syscall_buf;
    if (hal_copy_string_from_user(k_title, (const char *)arg4, 64) != 0) {
      hal_frame_set_return(frame, -EFAULT);
      break;
    }
    hal_frame_set_return(frame, compositor_create_window(
                                  (int)arg0, (int)arg1, (int)arg2, (int)arg3,
                                  k_title, current_process));
  } break;
  case SYS_WINDOW_DRAW:
    compositor_draw_rect((int)arg0, (int)arg1, (int)arg2, (int)arg3, (int)arg4,
                         (uint32_t)arg5, (int)current_process->pid);
    hal_frame_set_return(frame, 0);
    break;
  case SYS_WINDOW_WRITE:
    /* write text to a window by id (#123) — needs CAP_WINDOW.  Replaces the
     * old fd>=100 overload on write(). */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    hal_frame_set_return(
        frame, window_text_write((int)arg0, (const char *)arg1, (size_t)arg2));
    break;
  case SYS_WINDOW_OF_PID: {
    /* Read-only: the compositor window id of a pid, or 0 if it has none.
     * The shell uses it to tell a windowless (run-in-shell) program from one
     * that opened its own window (#123).  No capability needed. */
    extern int compositor_get_window_by_pid(int pid);
    int w = compositor_get_window_by_pid((int)arg0);
    hal_frame_set_return(frame, w > 0 ? w : 0);
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
      hal_frame_set_return(frame, -EINVAL);
    else
      hal_frame_set_return(frame,
                         ((long)(cols & 0xFFFF) << 16) | (rows & 0xFFFF));
    break;
  }
  case SYS_DISPLAY_INFO: {
    /* Read-only: current desktop size, packed (w<<16)|h (GFX-DYN-01). */
    extern void compositor_get_size(int *w, int *h);
    int w = 0, h = 0;
    compositor_get_size(&w, &h);
    hal_frame_set_return(frame, ((long)(w & 0xFFFF) << 16) | (h & 0xFFFF));
    break;
  }
  case SYS_SET_DISPLAY_MODE: {
    /* GFX-DYN-01: change resolution at runtime; the desktop adapts.  A
     * system-level action → needs CAP_WINDOW. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      hal_frame_set_return(frame, -EPERM);
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
    hal_frame_set_return(frame, r);
    break;
  }
  case SYS_WINDOW_RESIZE: {
    /* Resize a window's logical surface.  Needs CAP_WINDOW and ownership
     * (owner or machine), mirroring SYS_DESTROY_WINDOW's ABI-04 check. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    extern int compositor_window_owner(int window_id);
    extern int compositor_resize_window(int window_id, int w, int h);
    int owner = compositor_window_owner((int)arg0);
    if (owner >= 0 && owner != (int)current_process->pid &&
        !proc_has_cap(current_process, CAP_WINDOW_MANAGE)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    hal_frame_set_return(
        frame, compositor_resize_window((int)arg0, (int)arg1, (int)arg2));
    break;
  }
  case SYS_DISPLAY_POLL: {
    /* Apply a pending host display-change in process context (init's supervisor
     * loop polls this so the heavy set_mode/realloc never runs in the IRQ
     * tick). Machine-level only.  Returns 1 if the desktop was resized, else 0.
     */
    if (!proc_is_machine(current_process)) {
      hal_frame_set_return(frame, -EPERM);
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
      hal_frame_set_return(frame, 1);
    } else {
      hal_frame_set_return(frame, 0);
    }
    break;
  }
  case SYS_SET_STYLE: {
    /* Switch the compositor look (Style/Theme/Background).  Needs CAP_WINDOW.
     * arg0 = style id, arg1 = theme id, arg2 = bg id; pass -1 to keep. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      hal_frame_set_return(frame, -EPERM);
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
        if (registry_set("style.name", os1_style_names[(int)arg0], 0) != 0)
        rc |= -EIO; /* applied but NOT persisted: it survives until
                     * reboot and then silently reverts, which is
                     * worse than failing now */
    }
    if ((int)arg1 >= 0) {
      int r = compositor_set_theme((int)arg1);
      rc |= r;
      if (r == 0 && (int)arg1 < OS1_THEME_COUNT)
        if (registry_set("theme.color", os1_theme_names[(int)arg1], 0) != 0)
        rc |= -EIO; /* applied but NOT persisted: it survives until
                     * reboot and then silently reverts, which is
                     * worse than failing now */
    }
    if ((int)arg2 >= 0) {
      int r = compositor_set_background((int)arg2);
      rc |= r;
      if (r == 0 && (int)arg2 < OS1_BG_COUNT)
        if (registry_set("background.name", os1_bg_names[(int)arg2], 0) != 0)
        rc |= -EIO; /* applied but NOT persisted: it survives until
                     * reboot and then silently reverts, which is
                     * worse than failing now */
    }
    hal_frame_set_return(frame, rc);
    break;
  }
  case SYS_SET_ZOOM: {
    /* Desktop zoom percent (HiDPI/zoom, F2).  Needs CAP_WINDOW. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    extern int compositor_set_zoom(int percent);
    hal_frame_set_return(frame, compositor_set_zoom((int)arg0));
    break;
  }
  case SYS_COMPOSITOR_RENDER:
    /* Single compositor-push syscall (the duplicate SYS_FLUSH was retired and
     * folded here). CAP_WINDOW authorizes a process to request presentation;
     * PID 1 is the sole renderer/input pump below. Other apps' flush() calls
     * return promptly and the supervisor presents their damage on its next
     * frame. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    /* Legacy flush is a compatibility hint; rendering is resource I/O. */
    hal_frame_set_return(frame, 0);
    break;
  case SYS_WINDOW_SNAPSHOT: {
    /* Legacy ABI acquisition gate. Native SDK clients use resource handles. */
    if (!proc_has_cap(current_process, CAP_WINDOW) ||
        !proc_is_privileged(current_process)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    hal_frame_set_return(
        frame, compositor_window_snapshot(
                   (struct os1_window_snapshot_v1 *)arg0));
    break;
  }
  case SYS_DISPLAY_PRESENT: {
    if (!proc_has_cap(current_process, CAP_WINDOW) ||
        !proc_is_privileged(current_process)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    struct os1_compositor_present_v1 user_present;
    if (hal_copy_from_user(&user_present, (const void *)arg0,
                            sizeof(user_present)) != 0) {
      hal_frame_set_return(frame, -EFAULT);
      break;
    }
    if (user_present.abi_version != OS1_COMPOSITOR_ABI_V1 ||
        user_present.struct_size < sizeof(user_present) ||
        user_present.width <= 0 || user_present.height <= 0 ||
        user_present.stride != user_present.width ||
        !user_present.pixels) {
      hal_frame_set_return(frame, -EINVAL);
      break;
    }
    size_t pixel_count = (size_t)user_present.width *
                         (size_t)user_present.height;
    if (pixel_count / (size_t)user_present.width !=
            (size_t)user_present.height ||
        pixel_count > 16u * 1024u * 1024u ||
        user_present.pixel_capacity < pixel_count) {
      hal_frame_set_return(frame, -EINVAL);
      break;
    }
    size_t bytes = pixel_count * sizeof(uint32_t);
    uint32_t *pixels = (uint32_t *)kmalloc(bytes);
    if (!pixels) {
      hal_frame_set_return(frame, -ENOMEM);
      break;
    }
    if (hal_copy_from_user(pixels, user_present.pixels, bytes) != 0) {
      kfree(pixels);
      hal_frame_set_return(frame, -EFAULT);
      break;
    }
    int rc = graphics_present_argb8888(
        pixels, user_present.width, user_present.height,
        user_present.stride, user_present.pixel_capacity);
    kfree(pixels);
    hal_frame_set_return(frame, rc);
    break;
  }
  case SYS_INPUT_RAW_READ: {
    struct os1_raw_input_event_v1 event;
    long result;
    if (!proc_has_cap(current_process, CAP_WINDOW) ||
        !proc_is_privileged(current_process)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    result = OS1libkernel_input_read_raw(&event, (unsigned)arg1,
                                         (unsigned)arg2);
    if (result > 0 &&
        hal_copy_to_user((void *)arg0, &event, sizeof(event)) != 0)
      result = -EFAULT;
    hal_frame_set_return(frame, result);
    break;
  }
  case SYS_WINDOW_MOVE:
    if (!proc_has_cap(current_process, CAP_WINDOW) ||
        !proc_has_cap(current_process, CAP_WINDOW_MANAGE)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    compositor_move_window((int)arg0, (int)arg1, (int)arg2);
    hal_frame_set_return(frame, 0);
    break;
  case SYS_WINDOW_BLIT:
    compositor_blit((int)arg0, (int)arg1, (int)arg2, (int)arg3, (int)arg4,
                    (const uint32_t *)arg5, (int)current_process->pid);
    /* This process now owns custom pixel content in a window's buffer —
     * see struct process.self_rendered (sched.h) for why the own-window-
     * first stdout path (window_text_write below) must stop drawing text
     * into it from here on.  compositor_blit() already refused the call
     * above if the window is not this process's own (or init's), so
     * reaching here means the blit legitimately landed on OUR window. */
    current_process->self_rendered = 1;
    hal_frame_set_return(frame, 0);
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
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    compositor_set_window_flags((int)arg0, (int)arg1);
    hal_frame_set_return(frame, 0);
  } break;
  case SYS_DESTROY_WINDOW: {
    /* ABI-04: only the window's owner (or a system process) may destroy it.
     * Kernel-internal teardown (close button, process exit) calls
     * compositor_destroy_window() directly and is unaffected. */
    extern int compositor_window_owner(int window_id);
    int owner = compositor_window_owner((int)arg0);
    if (owner >= 0 && owner != (int)current_process->pid &&
        !proc_is_machine(current_process)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    compositor_destroy_window((int)arg0);
    hal_frame_set_return(frame, 0);
  } break;
  case SYS_SBRK:
    hal_frame_set_return(frame, sys_sbrk((intptr_t)arg0));
    break;
  case SYS_SPAWN: {
    /* USR-SEC-03 #79: spawning needs CAP_SPAWN.  A plain spawn yields a full
     * PLVL_USER child (clamped to the creator), preserving today's behaviour.
     */
    if (!proc_has_cap(current_process, CAP_SPAWN)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    /* #193 hardening: unknown spawn-flag bits are rejected, not ignored —
     * the spawn surface must fail closed on semantics it does not know. */
    if (((uint32_t)arg3 & ~(uint32_t)SPAWN_FLAGS_ALL) != 0) {
      hal_frame_set_return(frame, -EINVAL);
      break;
    }
    char k_path[128];
    if (hal_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      hal_frame_set_return(frame, -EFAULT);
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
    char *kargv[SPAWN_MAX_ARGS] = {0};
    char *argv_store = NULL;
    if (argc > 0) {
      void *uptrs[SPAWN_MAX_ARGS];
      if (hal_copy_from_user(uptrs, (const void *)arg2,
                              (size_t)argc * sizeof(void *)) != 0) {
        hal_frame_set_return(frame, -EFAULT);
        break;
      }
      argv_store = kmalloc((size_t)argc * SPAWN_ARG_LEN);
      if (!argv_store) {
        hal_frame_set_return(frame, -ENOMEM);
        break;
      }
      int bad = 0;
      for (int i = 0; i < argc; i++) {
        kargv[i] = argv_store + (size_t)i * SPAWN_ARG_LEN;
        if (hal_copy_string_from_user(kargv[i], (const char *)uptrs[i],
                                       SPAWN_ARG_LEN) != 0) {
          bad = 1;
          break;
        }
      }
      if (bad) {
        kfree(argv_store);
        hal_frame_set_return(frame, -EFAULT);
        break;
      }
    }
    /* Optional fd redirections (Phase 4 shell `<`/`>`/`>>`/`2>`): arg4 = user
     * array of struct spawn_redir, arg5 = count.  Copy them into kernel memory
     * so dispatch_spawn can dup the spawner's handles into the child.  Absent
     * (count 0) → today's behaviour exactly. */
    struct spawn_redir kredir[SPAWN_MAX_REDIR] = {0};
    int nredir = (int)arg5;
    if (nredir < 0)
      nredir = 0;
    if (nredir > SPAWN_MAX_REDIR) {
      if (argv_store)
        kfree(argv_store);
      hal_frame_set_return(frame, -EINVAL);
      break;
    }
    if (nredir > 0 &&
        hal_copy_from_user(kredir, (const void *)arg4,
                            (size_t)nredir * sizeof(struct spawn_redir)) != 0) {
      if (argv_store)
        kfree(argv_store);
      hal_frame_set_return(frame, -EFAULT);
      break;
    }
    /* arg3 = spawn-mode flags (SPAWN_FLAG_*, caps.h) — nxexec model #193. */
    long sret = dispatch_spawn(k_path, PLVL_USER, 0, 0, argc, kargv,
                               (uint32_t)arg3, kredir, nredir);
    if (argv_store)
      kfree(argv_store);
    hal_frame_set_return(frame, sret);
  } break;
  case SYS_SPAWN_CAPS: {
    /* spawn_caps(path, level, caps) — restricted spawn.  The requested level
     * and caps are clamped monotonically in process_create_caps (never more
     * privileged than the creator, never above the level ceiling, never more
     * than the creator holds). */
    if (!proc_has_cap(current_process, CAP_SPAWN)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    /* #193 hardening: same strict flag validation as SYS_SPAWN. */
    if (((uint32_t)arg3 & ~(uint32_t)SPAWN_FLAGS_ALL) != 0) {
      hal_frame_set_return(frame, -EINVAL);
      break;
    }
    char k_path[128];
    if (hal_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      hal_frame_set_return(frame, -EFAULT);
      break;
    }
    hal_frame_set_return(frame,
                       dispatch_spawn(k_path, (uint8_t)arg1, (uint32_t)arg2, 1,
                                      0, NULL, (uint32_t)arg3, NULL, 0));
  } break;
  case SYS_KILL:
    /* ABI-04: a process may kill itself or its descendants (orphans are
     * re-homed to a live ancestor at reap time, SCHED-DOS-02); SYSTEM/ROOT
     * may kill anything (process_terminate still protects SYSTEM targets). */
    if (!process_kill_allowed(current_process, (int)arg0)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    /* External kill of ANOTHER process is window-aware (kill the target + its
     * windowless/in-shell descendants, SPARE windowed apps —
     * PROCESS-KILL-MODEL). Killing SELF stays single-process, like exit. */
    if (current_process && (int)arg0 == (int)current_process->pid) {
      hal_frame_set_return(frame, process_terminate((int)arg0));
    } else {
      hal_frame_set_return(frame, process_kill_subtree((int)arg0));
    }
    break;
  case SYS_GETPROCS:
    hal_frame_set_return(frame, sys_getprocs((void *)arg0, (size_t)arg1));
    break;
  case SYS_SYSSTATS:
    /* OS1_sys_stats(buf, buf_size): one os1_sysstats snapshot (perf §1).
     * PRIVILEGED introspection: only ROOT/machine NX services (e.g.
     * /sys/bin/nxmemstat) may read the raw stats; USER apps go through the
     * service's exposed interface, never this syscall directly. */
    if (!current_process || current_process->level > PLVL_ROOT) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    hal_frame_set_return(frame, sys_sysstats((void *)arg0, (size_t)arg1));
    break;
  case SYS_GET_IDENTITY: {
    /* Read-only self-introspection: the caller's own privilege LEVEL and
     * capability mask, packed (level<<16)|caps.  Ungated — a process may always
     * learn its OWN identity.  Backs OS1_identity()/nxperm and lets userland
     * present a level-based view (machine/root/user/guest) without exposing the
     * raw capability bits to applications. */
    int lvl = current_process ? (int)current_process->level : PLVL_GUEST;
    unsigned int cps = current_process ? current_process->caps : 0u;
    hal_frame_set_return(frame,
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
    hal_frame_set_return(frame, rc);
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
    /* No current task means the scheduler is between reaping the previous one
     * and picking the next; sys_ipc_recv walks current_process->msg_queue
     * unguarded, so it must not be entered in that window.  aarch64's EL0
     * vector already refuses to continue in this state (it panics by name);
     * amd64 has no such guard, which is why this check lives here, in the
     * shared dispatcher, rather than in one arch's entry path. */
    if (!current_process) {
      hal_frame_set_return(frame, -ESRCH);
      return schedule(frame);
    }
    long rc = sys_ipc_recv((int)arg0, (void *)arg1);
    if (rc == IPC_RECV_RETRY)
      hal_frame_retry_syscall(frame); /* the LIVE frame — see sys_ipc_recv */
    else
      hal_frame_set_return(frame, rc);
    return schedule(frame);
  }
  case SYS_TRY_RECV:
    hal_frame_set_return(frame, sys_ipc_try_recv((int)arg0, (void *)arg1));
    break;
  case SYS_SET_FOCUS:
    /* ABI-04 / USR-SEC-03 #79: claiming focus needs CAP_WINDOW; a process may
     * only claim focus for ITSELF (every userland caller does
     * set_focus(get_pid())); redirecting input to/from another PID — i.e.
     * keystroke stealing — needs machine level. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    if ((int)arg0 != (int)current_process->pid &&
        !proc_is_machine(current_process)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    sched_set_focus_pid(
        (int)arg0); /* push the focus hint to the scheduler (#67) */
    /* Caret follows the input window: clear it off whoever just lost focus. */
    {
      extern void compositor_focus_changed(int new_pid);
      compositor_focus_changed((int)arg0);
    }
    hal_frame_set_return(frame, 0);
    break;
  case SYS_WAIT: {
    /* Ambient wait, arg1 = OPTIONAL user int* for the exit status.
     *
     * It used to always pass NULL, on the reasoning that the capability path
     * (SYS_OBJECT_WAIT) carries the status.  Phase 9b invalidated that: once a
     * status OUTLIVES its process, the capability can no longer be acquired for
     * a dead pid (acquisition requires a live process), so the caller ALWAYS
     * lands here — and the status was discarded exactly in the case it was
     * finally available.  Passing NULL stays legal, so existing callers that
     * only test the return value are unaffected. */
    int wcode = 0;
    long wr = process_wait((int)arg0, &wcode);
    /* The (void) cast that used to be here did not suppress the attribute, it
     * only made the omission look deliberate.  A bad status pointer is -EFAULT
     * under POSIX, not a successful wait whose status silently never arrived. */
    if (arg1 && wr >= 0 &&
        hal_copy_to_user((void *)arg1, &wcode, sizeof(wcode)) != 0)
      wr = -EFAULT;
    hal_frame_set_return(frame, wr);
  } break;
  case SYS_REGISTRY:
    hal_frame_set_return(frame, sys_registry((int)arg0, (const char *)arg1,
                                           (char *)arg2, (size_t)arg3));
    break;
  case SYS_STAT: {
    /*
     * Path metadata in ONE round trip: type + size.
     *
     * Why this exists (R1-fix).  R1 removed the ambient FS verbs and composed
     * their users over the object layer, which was right for the DATA path but
     * had two costs the composition could not absorb:
     *
     *  - CORRECTNESS.  opendir() and stat() both relied on the invariant "the
     *    list primitive succeeds ONLY on a directory".  Once a READ handle could
     *    be acquired on any path, object_read on a FILE returned its CONTENT, so
     *    opendir() succeeded on regular files — the file manager then marked
     *    every file a directory and double-clicking one tried to chdir into it
     *    ("Cannot open directory").  The type distinction has to come from the
     *    kernel, which is the only place that knows it.
     *  - COST.  The size probe became handle_create + OBJ_CTL_STAT + close:
     *    three syscalls and two path resolutions for one number, on the path
     *    every fopen() takes.
     *
     * This is a NET REDUCTION even though it adds a number: it replaces three
     * retired verbs and the three-call dance, and it duplicates nothing — it
     * asks the same VFS the object layer asks.
     */
    /* Path copies go in the per-CPU scratch buffer, per this file's own rule
     * ("cpu->syscall_buf is used for path/title copies; only one such copy is
     * in flight per CPU"), not on the kernel stack. */
    char *k_path = get_cpu_info()->syscall_buf;
    if (hal_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      hal_frame_set_return(frame, -EFAULT);
      break;
    }
    char *resolved_path = k_path + 128; /* same 2 KiB scratch, second slot */
    vfs_resolve_path(k_path, resolved_path, 128);
    struct vfs_stat vst;
    if (vfs_stat(resolved_path, &vst) != 0) {
      hal_frame_set_return(frame, -ENOENT);
      break;
    }
    struct abi_stat ast;
    memset(&ast, 0, sizeof(ast));
    ast.size = vst.size;
    ast.type = vst.type; /* ABI_S_TYPE_* match VFS_TYPE_* by value */
    if (hal_copy_to_user((void *)arg1, &ast, sizeof(ast)) != 0) {
      hal_frame_set_return(frame, -EFAULT);
      break;
    }
    hal_frame_set_return(frame, 0);
  } break;
  case SYS_SET_FONT:
    /* Replacing the GLOBAL system font is a desktop-wide display change (it
     * affects every window): same capability gate as set_style/set_zoom
     * (CAP_WINDOW).  Previously ungated — any process could restyle the
     * desktop. */
    if (!proc_has_cap(current_process, CAP_WINDOW)) {
      hal_frame_set_return(frame, -EPERM);
      break;
    }
    hal_frame_set_return(frame, sys_set_font((void *)arg0, (size_t)arg1));
    break;
  case SYS_CHDIR: {
    char k_path[128];
    if (hal_copy_string_from_user(k_path, (const char *)arg0, 128) != 0) {
      hal_frame_set_return(frame, -EFAULT);
      break;
    }
    char resolved_path[128];
    vfs_resolve_path(k_path, resolved_path, 128);

    /* Verify it exists and is a directory. */
    struct vfs_stat st;
    if (vfs_stat(resolved_path, &st) != 0) {
      hal_frame_set_return(frame, -ENOENT);
    } else if (st.type != VFS_TYPE_DIR) {
      hal_frame_set_return(frame, -ENOTDIR);
    } else {
      strncpy(current_process->cwd, resolved_path, 128);
      hal_frame_set_return(frame, 0);
    }
  } break;
  case SYS_GETCWD: {
    /* Capacity is not a copy length: copying it leaked adjacent process
     * fields for large buffers and falsely succeeded without a terminator
     * for small ones. Export only the validated string including its NUL. */
    size_t length = strnlen(current_process->cwd, sizeof(current_process->cwd));
    long result = -EIO;
    if (arg0 == 0U || arg1 == 0U) result = -EINVAL;
    else if (length == sizeof(current_process->cwd)) result = -EIO;
    else if ((size_t)arg1 <= length) result = -ERANGE;
    else result = hal_copy_to_user((void *)arg0, current_process->cwd,
                                   length + 1U) != 0 ? -EFAULT : 0;
    hal_frame_set_return(frame, result);
    return frame;
  }
  case SYS_PORT_SEND_CAPS:
    /* port send carrying capabilities; see sys_port_send_caps() for why a
     * service found by NAME must not need its pid to receive rights. */
    hal_frame_set_return(frame,
                       sys_port_send_caps((int)arg0, (const void *)arg1,
                                          (const int *)arg2, (int)arg3));
    break;
  case SYS_PIPE:
    /* pipe(int fds[2]) → OBJ_TYPE_PIPE; installs read+write ends in the caller's
     * handle table (fds[0]=read, fds[1]=write).  The shell wires `cmd | cmd` by
     * dup'ing the ends into the children's fd 0/1 (the same spawn-redir path). */
    hal_frame_set_return(frame, sys_pipe((int *)arg0));
    break;
  /* --- Object / capability ABI (ASTRA §6.1/6.2/6.5, kernel/object.h) ---
   * The real capability layer: unforgeable per-process handles to refcounted
   * kernel objects with separable/attenuable rights.  User pointers (path,
   * buf) are validated inside each backend via arch_copy_*_user. */
  case SYS_HANDLE_CREATE:
    hal_frame_set_return(frame, sys_handle_create((int)arg0, (const char *)arg1,
                                                (uint32_t)arg2, (int)arg3));
    break;
  case SYS_HANDLE_DUP:
    hal_frame_set_return(frame, sys_handle_dup((int)arg0, (uint32_t)arg1));
    break;
  case SYS_HANDLE_CLOSE:
    hal_frame_set_return(frame, sys_handle_close((int)arg0));
    break;
  case SYS_CAP_QUERY:
    hal_frame_set_return(frame, sys_cap_query((int)arg0));
    break;
  case SYS_CAP_GRANT:
    hal_frame_set_return(frame,
                       sys_cap_grant((int)arg0, (int)arg1, (uint32_t)arg2));
    break;
  case SYS_OBJECT_READ:
    hal_frame_set_return(frame,
                       sys_object_read((int)arg0, (void *)arg1, (size_t)arg2));
    break;
  case SYS_OBJECT_WRITE:
    hal_frame_set_return(
        frame, sys_object_write((int)arg0, (const void *)arg1, (size_t)arg2));
    break;
  case SYS_OBJECT_WAIT:
    hal_frame_set_return(frame, sys_object_wait((int)arg0, (long)arg1));
    break;
  case SYS_OBJECT_CTL:
    hal_frame_set_return(frame, sys_object_ctl((int)arg0, (int)arg1, (long)arg2));
    break;
  default:
    pr_warn("Unknown syscall: %ld\n", syscall_num);
    hal_frame_set_return(frame, -ENOSYS);
    break;
  }

  return process_creation_finish(frame);
}

/*
 * SYS_GET_TIME remains the compatibility millisecond view. New code uses the
 * object/time ABI or SYS_CLOCK_GETTIME nanoseconds.
 */
long sys_get_time(void) {
  return (long)(OS1libkernel_time_monotonic_ns() / UINT64_C(1000000));
}

long sys_clock_gettime(int clock_id) {
  if (clock_id == 1) {
    struct process *process = current_process;
    return process
        ? (long)OS1libkernel_time_counts_to_ns(process->cpu_time_counts)
        : 0;
  }
  return (long)OS1libkernel_time_monotonic_ns();
}

static void proc_sleep_wake(void *data) {
  OS1libkernel_process_sleep_timer_wake((struct process *)data);
}

static hal_frame_t *sys_nanosleep(hal_frame_t *frame, uint64_t ns) {
  struct process *process = current_process;
  uint64_t now;
  int arm_status;

  if (process == NULL || ns == 0U) {
    hal_frame_set_return(frame, 0);
    return frame;
  }
  now = OS1libkernel_time_monotonic_ns();
  if (process->wake_ns == 0U) {
    if (ns > UINT64_MAX - now) {
      hal_frame_set_return(frame, -EINVAL);
      return frame;
    }
    process->wake_ns = now + ns;
  }
  if (now >= process->wake_ns) {
    process->wake_ns = 0U;
    OS1libkernel_timer_cancel(&process->sleep_timer);
    hal_frame_set_return(frame, 0);
    return frame;
  }
  arm_status = OS1libkernel_process_sleep_timer_arm(
      process, &process->sleep_timer, proc_sleep_wake, process->wake_ns);
  if (arm_status < 0) {
    hal_frame_set_return(frame, arm_status);
    return frame;
  }
  hal_frame_retry_syscall(frame);
  return schedule(frame);
}

#define YIELD_SPIN_BUDGET 64U

static hal_frame_t *sys_yield(hal_frame_t *frame) {
  struct process *process = current_process;
  uint64_t now;
  uint64_t window;

  if (process == NULL)
    return schedule(frame);
  now = OS1libkernel_time_monotonic_ns();
  window = now - (now % OS1LIBKERNEL_NSEC_PER_TICK);
  if (process->yield_window_ns != window) {
    process->yield_window_ns = window;
    process->yield_count = 0U;
  }
  ++process->yield_count;
  if (process->yield_count > YIELD_SPIN_BUDGET) {
    uint64_t deadline = window + OS1LIBKERNEL_NSEC_PER_TICK;
    process->yield_count = 0U;
    if (deadline > window)
      (void)OS1libkernel_process_sleep_timer_arm(
          process, &process->sleep_timer, proc_sleep_wake, deadline);
  }
  return schedule(frame);
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
    current_process->exit_code = status & 0xff; /* Phase 2: collectable by a waiter */
    current_process->exited = 1;                 /* voluntary exit (not killed) */
    /* Nothing to propagate to: the caller is exiting.  Reported so a failure
     * to tear down is visible instead of leaving a process that believes it
     * exited still in the pool. */
    if (process_terminate((int)current_process->pid) != 0)
      pr_err("exit: process_terminate(%d) failed\n", (int)current_process->pid);
  }
}
