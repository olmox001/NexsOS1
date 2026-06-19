/*
 * kernel/core/object.c
 * Kernel object manager + capability handle table — the REAL capability layer
 * (ASTRA §6.1/6.2/6.5).  See kernel/include/kernel/object.h and
 * include/api/object.h for the model.
 *
 * Invariants enforced here:
 *   - Unforgeability: a handle is an index into the caller's PRIVATE table; a
 *     value with no installed slot returns -EBADF.  A process can only act on
 *     objects it was handed.
 *   - Attenuation only: duplicate()/grant() compute new_rights & current_rights,
 *     so rights can never grow (no escalation), mirroring the caps.h spawn cut.
 *   - Acquisition is ambient-gated: getting a write FILE capability still needs
 *     CAP_FS_WRITE + the /bin,/sys ACL; getting a PROCESS capability needs the
 *     same authority as kill (self/descendant/privileged).  Once held, the
 *     handle is the only thing that matters.
 *
 * Locking: one global object_lock guards refcounts and slot install/close/grant
 * (short, allocation-free critical sections).  Blocking VFS I/O runs OUTSIDE
 * the lock via a pin (refcount++) / use / unpin pattern, so a concurrent
 * close() cannot free an object mid-I/O.  Lock order object_lock -> kmalloc_lock
 * is consistent (kmalloc/kfree never take object_lock), so kfree() under the
 * lock cannot deadlock.
 */
#include <kernel/types.h>
#include <kernel/object.h>
#include <kernel/sched.h>
#include <kernel/cpu.h>
#include <kernel/spinlock.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/vfs.h>

/* Upper bound for a single object read/write bounce buffer (mirrors the
 * dispatcher's SYSCALL_MAX_IO_BYTES so a user size argument cannot ask the
 * kernel for an absurd allocation). */
#define OBJ_MAX_IO_BYTES (16u * 1024u * 1024u)

extern int arch_copy_from_user(void *dest, const void *src, size_t n);
extern int arch_copy_to_user(void *dest, const void *src, size_t n);
extern int arch_copy_string_from_user(char *dest, const char *src, size_t max_len);

/* Serialises refcount changes and handle-table slot mutations. */
static DEFINE_SPINLOCK(object_lock);

/* kobj_alloc - allocate a zeroed kobject of 'type'.  refcount starts at 0; the
 * first handle_install_locked() that succeeds takes it to 1. */
static struct kobject *kobj_alloc(uint8_t type) {
  struct kobject *o = kmalloc(sizeof(struct kobject));
  if (!o)
    return NULL;
  memset(o, 0, sizeof(*o));
  o->type = type;
  o->refcount = 0;
  return o;
}

/* handles_ensure - lazily allocate the caller's (or a target's) handle table.
 * NULL until a process first touches the object ABI, so processes that never
 * use it pay nothing.  Allocation happens with the lock released; publication
 * is double-checked under the lock. */
static int handles_ensure(struct process *p) {
  if (p->handles)
    return 0;
  struct handle_entry *tbl =
      kmalloc(sizeof(struct handle_entry) * NPROC_HANDLES);
  if (!tbl)
    return -ENOMEM;
  memset(tbl, 0, sizeof(struct handle_entry) * NPROC_HANDLES);
  uint64_t flags;
  spin_lock_irqsave(&object_lock, &flags);
  if (p->handles) {
    spin_unlock_irqrestore(&object_lock, flags);
    kfree(tbl);
    return 0;
  }
  p->handles = tbl;
  spin_unlock_irqrestore(&object_lock, flags);
  return 0;
}

/* handle_install_locked - place a reference to 'o' in p's first free slot and
 * bump the object refcount.  Caller MUST hold object_lock.  Returns the handle
 * index, -EBADF if the table vanished (target died), or -EMFILE if full. */
static int handle_install_locked(struct process *p, struct kobject *o,
                                 uint32_t rights) {
  if (!p->handles)
    return -EBADF;
  for (int i = 0; i < NPROC_HANDLES; i++) {
    if (p->handles[i].obj == NULL) {
      p->handles[i].obj = o;
      p->handles[i].rights = rights;
      o->refcount++;
      return i;
    }
  }
  return -EMFILE;
}

/* obj_unref - drop one reference taken by a pin; free at zero. */
static void obj_unref(struct kobject *o) {
  struct kobject *to_free = NULL;
  uint64_t flags;
  spin_lock_irqsave(&object_lock, &flags);
  if (--o->refcount <= 0)
    to_free = o;
  spin_unlock_irqrestore(&object_lock, flags);
  if (to_free)
    kfree(to_free);
}

/*
 * sys_handle_create - OS1low_handle_create(ns, path, rights, type).
 * Acquire a fresh capability (handle) to a named object.
 */
long sys_handle_create(int ns, const char *upath, uint32_t rights, int type) {
  struct process *cur = current_process;
  if (!cur)
    return -EPERM;
  rights &= OS1_RIGHT_ALL;

  char kpath[OBJ_FILE_PATH_MAX];
  if (arch_copy_string_from_user(kpath, upath, OBJ_FILE_PATH_MAX) != 0)
    return -EFAULT;

  if (handles_ensure(cur) != 0)
    return -ENOMEM;

  if (ns == OS1_NS_FS && type == OBJ_TYPE_FILE) {
    char resolved[OBJ_FILE_PATH_MAX];
    vfs_resolve_path(kpath, resolved, OBJ_FILE_PATH_MAX);
    /* Ambient gate to ACQUIRE a write capability: identical to SYS_OPEN. */
    if (rights & OS1_RIGHT_WRITE) {
      if (!proc_has_cap(cur, CAP_FS_WRITE))
        return -EPERM;
      if (!proc_is_machine(cur) &&
          (strncmp(resolved, "/sys/", 5) == 0 ||
           strncmp(resolved, "/bin/", 5) == 0))
        return -EACCES;
    }
    struct vfs_node node;
    if (vfs_open(resolved, &node) != 0)
      return -ENOENT;
    if (node.type != VFS_TYPE_FILE)
      return -EISDIR;

    struct kobject *o = kobj_alloc(OBJ_TYPE_FILE);
    if (!o)
      return -ENOMEM;
    o->node = node;
    o->offset = 0;
    strncpy(o->path, resolved, OBJ_FILE_PATH_MAX - 1);
    o->path[OBJ_FILE_PATH_MAX - 1] = '\0';

    uint64_t flags;
    spin_lock_irqsave(&object_lock, &flags);
    int h = handle_install_locked(cur, o, rights);
    spin_unlock_irqrestore(&object_lock, flags);
    if (h < 0) {
      kfree(o); /* never installed: refcount stayed 0 */
      return h;
    }
    return h;
  }

  if (ns == OS1_NS_PROC && type == OBJ_TYPE_PROCESS) {
    int pid = atoi(kpath); /* libkernel parser (kernel/lib/string.c) */
    if (pid <= 0)
      return -EINVAL;
    /* Acquisition policy: name a process you could act on (kill semantics). */
    if (!process_kill_allowed(cur, pid))
      return -EPERM;
    if (!process_find_by_pid(pid))
      return -ESRCH;

    struct kobject *o = kobj_alloc(OBJ_TYPE_PROCESS);
    if (!o)
      return -ENOMEM;
    o->pid = pid;

    uint64_t flags;
    spin_lock_irqsave(&object_lock, &flags);
    int h = handle_install_locked(cur, o, rights);
    spin_unlock_irqrestore(&object_lock, flags);
    if (h < 0) {
      kfree(o);
      return h;
    }
    return h;
  }

  return -EINVAL;
}

/*
 * sys_handle_dup - OS1low_handle_duplicate(handle, new_rights).
 * A second handle to the SAME object, with rights attenuated to a subset of the
 * source's.  Needs OS1_RIGHT_DUPLICATE on the source.
 */
long sys_handle_dup(int handle, uint32_t new_rights) {
  struct process *cur = current_process;
  if (!cur || !cur->handles || handle < 0 || handle >= NPROC_HANDLES)
    return -EBADF;
  new_rights &= OS1_RIGHT_ALL;

  uint64_t flags;
  spin_lock_irqsave(&object_lock, &flags);
  struct handle_entry *src = &cur->handles[handle];
  if (!src->obj) {
    spin_unlock_irqrestore(&object_lock, flags);
    return -EBADF;
  }
  if (!(src->rights & OS1_RIGHT_DUPLICATE)) {
    spin_unlock_irqrestore(&object_lock, flags);
    return -EPERM;
  }
  uint32_t rights = new_rights & src->rights; /* attenuation only */
  int h = handle_install_locked(cur, src->obj, rights);
  spin_unlock_irqrestore(&object_lock, flags);
  return h;
}

/*
 * sys_handle_close - OS1low_handle_close(handle).  Drop this handle; free the
 * object if it was the last reference.
 */
long sys_handle_close(int handle) {
  struct process *cur = current_process;
  if (!cur || !cur->handles || handle < 0 || handle >= NPROC_HANDLES)
    return -EBADF;

  struct kobject *to_free = NULL;
  uint64_t flags;
  spin_lock_irqsave(&object_lock, &flags);
  struct handle_entry *e = &cur->handles[handle];
  if (!e->obj) {
    spin_unlock_irqrestore(&object_lock, flags);
    return -EBADF;
  }
  struct kobject *o = e->obj;
  e->obj = NULL;
  e->rights = 0;
  if (--o->refcount <= 0)
    to_free = o;
  spin_unlock_irqrestore(&object_lock, flags);
  if (to_free)
    kfree(to_free);
  return 0;
}

/*
 * sys_cap_query - OS1low_cap_query(handle).  Return (type<<24)|rights for an
 * installed handle, else -EBADF.  The unforgeability probe in tests.
 */
long sys_cap_query(int handle) {
  struct process *cur = current_process;
  if (!cur || !cur->handles || handle < 0 || handle >= NPROC_HANDLES)
    return -EBADF;
  uint64_t flags;
  spin_lock_irqsave(&object_lock, &flags);
  struct handle_entry *e = &cur->handles[handle];
  if (!e->obj) {
    spin_unlock_irqrestore(&object_lock, flags);
    return -EBADF;
  }
  long packed = OS1_CAPQ_PACK(e->obj->type, e->rights);
  spin_unlock_irqrestore(&object_lock, flags);
  return packed;
}

/*
 * sys_cap_grant - OS1low_cap_grant(target_pid, handle, rights).  Delegate a
 * capability: install an attenuated handle to the SAME object in target_pid's
 * table (true sharing — seL4 grant).  Needs OS1_RIGHT_TRANSFER on the source
 * and IPC authority to the target.  Returns the handle index installed in the
 * TARGET (the grantor relays it to the target, e.g. over IPC).
 *
 * NOTE(OBJ-GRANT-REAP): granting to a process that is being reaped on another
 * CPU at the same instant is unsupported (the common path — parent delegating
 * to a live descendant — is fully serialised by object_lock + the table-null
 * in process_handles_destroy).
 */
long sys_cap_grant(int target_pid, int handle, uint32_t rights) {
  struct process *cur = current_process;
  if (!cur || !cur->handles || handle < 0 || handle >= NPROC_HANDLES)
    return -EBADF;
  rights &= OS1_RIGHT_ALL;

  if (!process_ipc_allowed(cur, target_pid))
    return -EPERM;
  struct process *tgt = process_find_by_pid(target_pid);
  if (!tgt)
    return -ESRCH;
  if (handles_ensure(tgt) != 0)
    return -ENOMEM;

  uint64_t flags;
  spin_lock_irqsave(&object_lock, &flags);
  struct handle_entry *src = &cur->handles[handle];
  if (!src->obj) {
    spin_unlock_irqrestore(&object_lock, flags);
    return -EBADF;
  }
  if (!(src->rights & OS1_RIGHT_TRANSFER)) {
    spin_unlock_irqrestore(&object_lock, flags);
    return -EPERM;
  }
  uint32_t g = rights & src->rights; /* attenuation only */
  int h = handle_install_locked(tgt, src->obj, g);
  spin_unlock_irqrestore(&object_lock, flags);
  return h;
}

/* pin_handle - look up a handle, check required rights, and pin the object
 * (refcount++) so blocking I/O can run with the lock released.  Returns the
 * pinned object (caller must obj_unref) or NULL with *err set. */
static struct kobject *pin_handle(int handle, uint32_t need, long *err) {
  struct process *cur = current_process;
  if (!cur || !cur->handles || handle < 0 || handle >= NPROC_HANDLES) {
    *err = -EBADF;
    return NULL;
  }
  uint64_t flags;
  spin_lock_irqsave(&object_lock, &flags);
  struct handle_entry *e = &cur->handles[handle];
  if (!e->obj) {
    spin_unlock_irqrestore(&object_lock, flags);
    *err = -EBADF;
    return NULL;
  }
  if ((e->rights & need) != need) {
    spin_unlock_irqrestore(&object_lock, flags);
    *err = -EPERM;
    return NULL;
  }
  struct kobject *o = e->obj;
  o->refcount++; /* pin */
  spin_unlock_irqrestore(&object_lock, flags);
  return o;
}

/*
 * sys_object_read - OS1_object_read(handle, buf, n).  Needs OS1_RIGHT_READ.
 * FILE: VFS read at the object's offset (shared across handles to it).
 */
long sys_object_read(int handle, void *ubuf, size_t n) {
  if (n > OBJ_MAX_IO_BYTES)
    return -EINVAL;
  long err = 0;
  struct kobject *o = pin_handle(handle, OS1_RIGHT_READ, &err);
  if (!o)
    return err;

  long ret;
  if (o->type == OBJ_TYPE_FILE) {
    if (n == 0) {
      ret = 0;
    } else {
      uint8_t *kb = kmalloc(n);
      if (!kb) {
        ret = -ENOMEM;
      } else {
        int got = vfs_read(&o->node, o->offset, kb, (uint32_t)n);
        if (got < 0) {
          ret = -EIO;
        } else if (got > 0 && arch_copy_to_user(ubuf, kb, (size_t)got) != 0) {
          ret = -EFAULT;
        } else {
          o->offset += (uint64_t)got; /* benign shared-offset race, documented */
          ret = got;
        }
        kfree(kb);
      }
    }
  } else {
    ret = -EINVAL; /* PROCESS object is not byte-readable */
  }

  obj_unref(o);
  return ret;
}

/*
 * sys_object_write - OS1_object_write(handle, buf, n).  Needs OS1_RIGHT_WRITE.
 * FILE: VFS write at the object's offset.
 */
long sys_object_write(int handle, const void *ubuf, size_t n) {
  if (n > OBJ_MAX_IO_BYTES)
    return -EINVAL;
  long err = 0;
  struct kobject *o = pin_handle(handle, OS1_RIGHT_WRITE, &err);
  if (!o)
    return err;

  long ret;
  if (o->type == OBJ_TYPE_FILE) {
    if (n == 0) {
      ret = 0;
    } else {
      uint8_t *kb = kmalloc(n);
      if (!kb) {
        ret = -ENOMEM;
      } else if (arch_copy_from_user(kb, ubuf, n) != 0) {
        kfree(kb);
        ret = -EFAULT;
      } else {
        int wr = vfs_write_file(o->path, kb, (uint32_t)n, (uint32_t)o->offset);
        kfree(kb);
        if (wr < 0) {
          ret = -EIO;
        } else {
          o->offset += (uint64_t)wr;
          (void)vfs_open(o->path, &o->node); /* refresh cached size */
          ret = wr;
        }
      }
    }
  } else if (o->type == OBJ_TYPE_PROCESS) {
    /* Capability IPC send (ASTRA §6.2/§6.5, Mach ports-as-objects): writing to
     * a PROCESS object delivers an ipc_message to that process's queue.  The
     * handle's WRITE right IS the authority — there is deliberately NO ambient
     * process_ipc_allowed() check here, because that gates capability
     * ACQUISITION (handle_create/grant), not use.  A process holding a granted
     * WRITE handle can therefore message a target it could not reach by PID —
     * this is the seL4/Mach delegation property. */
    if (n != sizeof(struct ipc_message)) {
      ret = -EINVAL;
    } else {
      struct ipc_message m;
      if (arch_copy_from_user(&m, ubuf, sizeof(m)) != 0) {
        ret = -EFAULT;
      } else {
        m.from = current_process ? (int)current_process->pid : 0;
        ret = (kernel_ipc_send(o->pid, &m) == 0) ? 0 : -ESRCH;
      }
    }
  } else {
    ret = -EINVAL;
  }

  obj_unref(o);
  return ret;
}

/*
 * sys_object_wait - OS1_object_wait(handle, arg).  Needs OS1_RIGHT_WAIT.
 * PROCESS: non-blocking process_wait() — status if dead, -1 alive, -2 gone.
 */
long sys_object_wait(int handle, long arg) {
  (void)arg; /* reserved (timeout/flags) */
  long err = 0;
  struct kobject *o = pin_handle(handle, OS1_RIGHT_WAIT, &err);
  if (!o)
    return err;

  long ret;
  if (o->type == OBJ_TYPE_PROCESS) {
    ret = process_wait(o->pid);
  } else {
    ret = -EINVAL;
  }

  obj_unref(o);
  return ret;
}

/*
 * process_handles_destroy - close every handle a dying process holds and free
 * its table.  Called from BOTH process free sites (immediate teardown and the
 * deferred reaper) before the struct process page is freed.  NULL-safe.
 */
void process_handles_destroy(struct process *p) {
  if (!p)
    return;
  uint64_t flags;
  spin_lock_irqsave(&object_lock, &flags);
  struct handle_entry *tbl = p->handles;
  p->handles = NULL; /* blocks a concurrent grant-into-dead under this lock */
  if (!tbl) {
    spin_unlock_irqrestore(&object_lock, flags);
    return;
  }
  for (int i = 0; i < NPROC_HANDLES; i++) {
    struct kobject *o = tbl[i].obj;
    if (!o)
      continue;
    tbl[i].obj = NULL;
    if (--o->refcount <= 0)
      kfree(o); /* object_lock -> kmalloc_lock order is consistent */
  }
  spin_unlock_irqrestore(&object_lock, flags);
  kfree(tbl);
}
