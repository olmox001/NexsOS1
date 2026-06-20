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
 * This generalizes the B3 per-process fd table (kernel/fd.h, ASTRA's "seed").
 * The fd table stays for the POSIX open/read/write path; the handle table is
 * the new general mechanism the object syscalls (235..242) operate on.
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

  /* OBJ_TYPE_PROCESS */
  int pid;
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

#endif /* _KERNEL_OBJECT_H */
