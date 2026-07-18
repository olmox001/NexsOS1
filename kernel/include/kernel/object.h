/*
 * kernel/include/kernel/object.h
 * Kernel object manager + per-process capability handle table.
 *
 * This is the REAL capability layer (ASTRA §6.1/6.2/6.5): the kernel-side
 * implementation of the unforgeable-handle model whose ABI lives in
 * include/api/object.h.  A kernel resource is a refcounted `struct kobject`;
 * a process names it only through a HANDLE — a small integer index into its
 * private handle table.  Each handle carries a RIGHTS subset (separable,
 * attenuable).  There is no way to name a kobject without holding a handle to
 * it: a handle integer with no installed slot is -EBADF, and a handle's rights
 * can only shrink across duplicate/grant, so privilege escalation is
 * impossible by construction (the same invariant as the caps.h spawn cut).
 *
 * This ABSORBED the B3 per-process fd table (ASTRA's "seed", §6.2): there is no
 * separate fd array any more — a POSIX descriptor IS a handle (fd N == handle N).
 * open() = handle_create(FILE); read/write/lseek/close operate on the handle
 * table; stdin/stdout/stderr are pre-installed CONSOLE handles 0/1/2.  The object
 * syscalls (235..242) and the POSIX file syscalls are the same mechanism.
 *
 * Concurrency: a single global `object_lock` serialises refcount changes and
 * handle-table slot install/close/grant (short, no-I/O critical sections).
 * Object I/O (read/write/wait) uses a pin/use/unpin pattern so blocking VFS
 * I/O never runs under the lock.
 */
#ifndef _KERNEL_OBJECT_H
#define _KERNEL_OBJECT_H

#include <kernel/types.h>
#include <kernel/vfs.h>
#include <object.h> /* shared ABI: OBJ_TYPE_*, OS1_RIGHT_*, OS1_NS_* */

#define NPROC_HANDLES 32
#define OBJ_FILE_PATH_MAX 128

/* A kernel object: a refcounted, typed resource named only through handles.
 * Payloads for every type are kept inline (a kobject is small and kmalloc'd);
 * `type` selects which fields are live. */
struct kobject {
  uint8_t type; /* OBJ_TYPE_* */
  int refcount; /* live handles referencing it across ALL processes */

  /* OBJ_TYPE_FILE */
  struct vfs_node node;
  uint64_t offset;                /* shared file position */
  char path[OBJ_FILE_PATH_MAX];   /* resolved path (VFS write contract is path-based) */

  /* OBJ_TYPE_PROCESS (also the owning pid for OBJ_TYPE_WINDOW) */
  int pid;

  /* OBJ_TYPE_WINDOW */
  int window_id;

  /* OBJ_TYPE_PIPE — anonymous kernel byte pipe (ASTRA §6.2).  Heap-allocated
   * (struct kpipe, kernel/core/object.c) so the ring buffer + reader wait queue
   * don't bloat every kobject; NULL for non-pipe objects. */
  struct kpipe *pipe;

  /* OBJ_TYPE_PORT — Mach-style message port (ASTRA §6.5): a named mailbox that
   * IS a capability.  Heap-allocated (struct kport, kernel/core/object.c) for
   * the same reason as the pipe; NULL for non-port objects. */
  struct kport *port;
};

/* A handle-table slot: a capability = (object, held rights). */
struct handle_entry {
  struct kobject *obj; /* NULL = free slot */
  uint32_t rights;     /* OS1_RIGHT_* held via THIS handle */
};

struct process;

/* Close every handle a dying process holds and free its handle table.
 * NULL-safe (a process that never used a handle has p->handles == NULL).
 * Called from BOTH process free sites (immediate teardown + deferred reap). */
void process_handles_destroy(struct process *p);

/* Instrumentation (perf brief §1/§2.6; surfaced via OS1_sys_stats): copy the
 * per-type live-kobject counts into out[0..min(max,OBJ_TYPE_COUNT)). */
void object_get_live_counts(uint64_t *out, int max);

/* Syscall backends (dispatched from kernel/core/syscall_dispatch.c).  The
 * caller is current_process; user pointers are copied with arch_copy_*_user
 * inside.  Return >= 0 on success, negative errno on failure. */
long sys_handle_create(int ns, const char *upath, uint32_t rights, int type);
long sys_handle_dup(int handle, uint32_t new_rights);
long sys_handle_close(int handle);
long sys_cap_query(int handle);
long sys_cap_grant(int target_pid, int handle, uint32_t rights);
long sys_object_read(int handle, void *ubuf, size_t n);
long sys_object_write(int handle, const void *ubuf, size_t n);
long sys_object_wait(int handle, long arg);
long sys_object_ctl(int handle, int cmd, long arg);
/* POSIX lseek(2) on a handle: FILE repositions its byte offset per 'whence'
 * (SEEK_SET/_CUR/_END); a non-seekable object (CONSOLE, …) returns -ESPIPE. */
long sys_object_lseek(int handle, long off, int whence);

/* window_text_write - copy a user buffer to window win_id (UART mirror +
 * compositor append); defined in kernel/core/syscall_dispatch.c.  The shared
 * backend of SYS_WINDOW_WRITE and the OBJ_TYPE_CONSOLE stdout/stderr handle
 * (declared here so kernel/core/object.c's console write can reach it). */
long window_text_write(int win_id, const char *ubuf, size_t count);

/* process_install_stdio - eagerly allocate p's handle table and pre-install the
 * standard trio: handles 0/1/2 share one CONSOLE object (0 = read/stdin, 1/2 =
 * write/stdout+stderr).  Called by process_create() so every process is born
 * with stdin/stdout/stderr as capability handles.  Returns 0, or -ENOMEM. */
int process_install_stdio(struct process *p);

/* process_redirect_child_fd - dup the spawner's open handle 'parent_fd' into a
 * freshly-created child's 'child_slot', overwriting the console there (Phase 4
 * shell `<`/`>`/`>>`/`2>`).  Called from dispatch_spawn with the spawner as
 * current_process, before the child runs.  Returns 0 or -EINVAL/-EBADF/-ENOMEM. */
int process_redirect_child_fd(struct process *child, int child_slot,
                              int parent_fd);
/* process_redirect_child_fd_from - same, with the SOURCE process explicit.  The
 * spawner and the fd owner are the same process today, but diverge once an
 * execution service spawns on a client's behalf (Q2): the fds are the CLIENT's.
 * Parameterising the source keeps that a caller change, not a rewrite. */
int process_redirect_child_fd_from(struct process *owner, struct process *child,
                                   int child_slot, int parent_fd);

/* sys_pipe - SYS_PIPE: create an anonymous OBJ_TYPE_PIPE and install its read +
 * write ends in the caller's table; writes both fds to the user int[2].  0 or
 * -ENOMEM/-EMFILE/-EFAULT. */
long sys_pipe(int *ufds);
/* sys_port_send_caps - SYS_PORT_SEND_CAPS: send through a PORT while
 * TRANSFERRING handles to the receiver, rewriting the payload with the indices
 * as the RECEIVER sees them (ASTRA 6.5 Mach rights-in-a-message).  Removes the
 * need to cap_grant by pid to a service discovered by NAME. */
long sys_port_send_caps(int handle, const void *umsg, const int *ufds, int nfds);

#endif /* _KERNEL_OBJECT_H */
