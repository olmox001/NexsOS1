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
#include <kernel/registry.h>
#include <kernel/graphics.h> /* OBJ_TYPE_WINDOW backends: compositor_* (§6.7) */

/* Upper bound for a single object read/write bounce buffer (mirrors the
 * dispatcher's SYSCALL_MAX_IO_BYTES so a user size argument cannot ask the
 * kernel for an absurd allocation). */
#define OBJ_MAX_IO_BYTES (16u * 1024u * 1024u)

extern int arch_copy_from_user(void *dest, const void *src, size_t n);
extern int arch_copy_to_user(void *dest, const void *src, size_t n);
extern int arch_copy_string_from_user(char *dest, const char *src, size_t max_len);

/* Serialises refcount changes and handle-table slot mutations. */
static DEFINE_SPINLOCK(object_lock);

/* Live-object instrumentation (perf brief §1/§2.6; surfaced via OS1_sys_stats).
 * The capability layer is the newest, least-tested subsystem and the highest-
 * leverage leak suspect: a missed decref on process teardown leaks the kobject
 * even after the handle table is freed.  Every kobject is born in kobj_alloc()
 * and dies in kobj_free(); routing all frees through one wrapper keeps the
 * per-type live count exactly symmetric regardless of which free path runs
 * (never-installed cleanup, handle_close, or process_handles_destroy).
 * Relaxed atomics — independent of object_lock. */
static uint64_t obj_live_count[OBJ_TYPE_COUNT];

/* kobj_alloc - allocate a zeroed kobject of 'type'.  refcount starts at 0; the
 * first handle_install_locked() that succeeds takes it to 1. */
static struct kobject *kobj_alloc(uint8_t type) {
  struct kobject *o = kmalloc(sizeof(struct kobject));
  if (!o)
    return NULL;
  memset(o, 0, sizeof(*o));
  o->type = type;
  o->refcount = 0;
  if (type < OBJ_TYPE_COUNT)
    __sync_fetch_and_add(&obj_live_count[type], 1);
  return o;
}

/* kobj_free - the single free path for a kobject; keeps the live-object count
 * symmetric with kobj_alloc().  Replaces every bare kfree() of a kobject. */
static void kobj_free(struct kobject *o) {
  if (!o)
    return;
  if (o->type < OBJ_TYPE_COUNT)
    __sync_fetch_and_sub(&obj_live_count[o->type], 1);
  kfree(o);
}

/* object_get_live_counts - copy the per-type live-kobject counts (perf brief
 * §1/§2.6).  'out' is filled for indices [0, min(max, OBJ_TYPE_COUNT)). */
void object_get_live_counts(uint64_t *out, int max) {
  if (!out)
    return;
  for (int i = 0; i < max && i < OBJ_TYPE_COUNT; i++)
    out[i] = obj_live_count[i];
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
    kobj_free(to_free);
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

  if (ns == OS1_NS_FS) {
    /* open() ≡ handle_create(OS1_NS_FS): the PATH'S PROVIDER decides the object
     * TYPE (FILE for ext4/regfs, PROCESS for /proc/<pid>, …) — the namespace IS
     * the object space.  The 'type' argument is only a hint; the resolution is
     * the single source of truth. */
    (void)type;
    char resolved[OBJ_FILE_PATH_MAX];
    vfs_resolve_path(kpath, resolved, OBJ_FILE_PATH_MAX);
    struct vfs_objref ref;
    memset(&ref, 0, sizeof(ref));
    int rr = vfs_resolve_object(resolved, &ref);
    if (rr == -2)
      return -EISDIR;
    if (rr != 0)
      return -ENOENT;

    struct kobject *o;
    if (ref.obj_type == OBJ_TYPE_FILE) {
      /* Ambient gate to ACQUIRE a write capability: identical to SYS_OPEN. */
      if (rights & OS1_RIGHT_WRITE) {
        if (!proc_has_cap(cur, CAP_FS_WRITE))
          return -EPERM;
        if (!proc_is_machine(cur) &&
            (strncmp(resolved, "/sys/", 5) == 0 ||
             strncmp(resolved, "/bin/", 5) == 0))
          return -EACCES;
      }
      o = kobj_alloc(OBJ_TYPE_FILE);
      if (!o)
        return -ENOMEM;
      o->node = ref.node;
      o->offset = 0;
      strncpy(o->path, resolved, OBJ_FILE_PATH_MAX - 1);
      o->path[OBJ_FILE_PATH_MAX - 1] = '\0';
    } else if (ref.obj_type == OBJ_TYPE_PROCESS) {
      /* Same acquisition gate as OS1_NS_PROC: a destructive handle (kill /
       * IPC-send) needs kill authority; a READ/WAIT status handle only needs the
       * process to exist (object_at already verified it). */
      if ((rights & (OS1_RIGHT_DESTROY | OS1_RIGHT_WRITE)) &&
          !process_kill_allowed(cur, ref.pid))
        return -EPERM;
      o = kobj_alloc(OBJ_TYPE_PROCESS);
      if (!o)
        return -ENOMEM;
      o->pid = ref.pid;
    } else {
      return -EINVAL; /* namespace path resolved to an unsupported type */
    }

    uint64_t flags;
    spin_lock_irqsave(&object_lock, &flags);
    int h = handle_install_locked(cur, o, rights);
    spin_unlock_irqrestore(&object_lock, flags);
    if (h < 0) {
      kobj_free(o); /* never installed: refcount stayed 0 */
      return h;
    }
    return h;
  }

  if (ns == OS1_NS_PROC && type == OBJ_TYPE_PROCESS) {
    int pid = atoi(kpath); /* libkernel parser (kernel/lib/string.c) */
    if (pid <= 0)
      return -EINVAL;
    /* Acquisition policy (seL4 separable rights, F4 M4.5): a DESTRUCTIVE handle —
     * one that can kill (DESTROY) or IPC-send (WRITE) — still needs kill authority.
     * A non-destructive WAIT/READ-only handle (status query, the OS1_object_wait
     * path behind OS1low_process_wait) only needs the process to exist, matching
     * the permissive ambient SYS_WAIT.  This separates wait-right from kill-right
     * without widening kill/send acquisition. */
    int destructive = (rights & (OS1_RIGHT_DESTROY | OS1_RIGHT_WRITE)) != 0;
    if (destructive && !process_kill_allowed(cur, pid))
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
      kobj_free(o); /* never installed: refcount stayed 0 */
      return h;
    }
    return h;
  }

  if (ns == OS1_NS_REG && type == OBJ_TYPE_REGKEY) {
    /* A registry key is a capability too (ASTRA §6.6: every node has an
     * associated capability).  A WRITE handle needs CAP_REG_WRITE to acquire
     * (mirrors sys_registry); reads are open.  The key is stored in the
     * kobject's path field, capped at the registry key length so create/get/set
     * all agree on the same string. */
    if ((rights & OS1_RIGHT_WRITE) && !proc_has_cap(cur, CAP_REG_WRITE))
      return -EPERM;
    if (kpath[0] == '\0')
      return -EINVAL;
    struct kobject *o = kobj_alloc(OBJ_TYPE_REGKEY);
    if (!o)
      return -ENOMEM;
    strncpy(o->path, kpath, MAX_KEY_LEN - 1);
    o->path[MAX_KEY_LEN - 1] = '\0';

    uint64_t flags;
    spin_lock_irqsave(&object_lock, &flags);
    int h = handle_install_locked(cur, o, rights);
    spin_unlock_irqrestore(&object_lock, flags);
    if (h < 0) {
      kobj_free(o); /* never installed: refcount stayed 0 */
      return h;
    }
    return h;
  }

  if (ns == OS1_NS_WIN && type == OBJ_TYPE_WINDOW) {
    /* A compositor window is a capability object (ASTRA §6.7).  Acquisition is
     * ambient-gated like the other types: a handle to YOUR OWN window is free;
     * a WRITE/DESTROY handle to ANOTHER process's window is window-manager
     * authority (machine/root only) — that is how /sys/bin/nxui (the dock) can
     * minimize/restore/focus/close any app.  READ stays open (inspect).  Once
     * held, the handle's rights are the only authority that matters. */
    int wid = atoi(kpath);
    if (wid <= 0)
      return -EINVAL;
    int owner = compositor_window_owner(wid);
    if (owner < 0)
      return -ESRCH;
    if (owner != (int)cur->pid &&
        (rights & (OS1_RIGHT_WRITE | OS1_RIGHT_DESTROY)) &&
        !proc_is_machine(cur) && cur->level > PLVL_ROOT)
      return -EPERM;

    struct kobject *o = kobj_alloc(OBJ_TYPE_WINDOW);
    if (!o)
      return -ENOMEM;
    o->window_id = wid;
    o->pid = owner;

    uint64_t flags;
    spin_lock_irqsave(&object_lock, &flags);
    int h = handle_install_locked(cur, o, rights);
    spin_unlock_irqrestore(&object_lock, flags);
    if (h < 0) {
      kobj_free(o); /* never installed: refcount stayed 0 */
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
    kobj_free(to_free);
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
  } else if (o->type == OBJ_TYPE_REGKEY) {
    /* read = registry_get the key's value (ASTRA §6.6) */
    char val[MAX_VAL_LEN];
    if (registry_get(o->path, val, sizeof(val)) != 0) {
      ret = -ENOENT;
    } else {
      size_t vlen = strlen(val) + 1; /* include NUL */
      if (vlen > n)
        vlen = n;
      ret = (vlen > 0 && arch_copy_to_user(ubuf, val, vlen) != 0) ? -EFAULT
                                                                  : (long)vlen;
    }
  } else if (o->type == OBJ_TYPE_WINDOW) {
    /* read = this window's info record (ASTRA §6.7).  A short read (n <
     * sizeof) copies a prefix, like the other object reads. */
    struct window_info wi;
    if (compositor_window_info(o->window_id, &wi) != 0) {
      ret = -ESRCH;
    } else {
      size_t cn = sizeof(wi);
      if (cn > n)
        cn = n;
      ret = (cn > 0 && arch_copy_to_user(ubuf, &wi, cn) != 0) ? -EFAULT
                                                              : (long)cn;
    }
  } else if (o->type == OBJ_TYPE_CONSOLE) {
    /* stdin: drain the IPC input queue for one pressed/repeat key (the folded-in
     * FD_KBD path).  Release events and non-input messages are discarded, as
     * before.  When nothing is pending we return -EAGAIN; the SYS_READ
     * dispatcher turns that into a blocking sleep — rescheduling needs the trap
     * frame, which lives in the dispatcher, not here. */
    if (n == 0) {
      ret = 0;
    } else {
      ret = -EAGAIN;
      while (1) {
        struct ipc_node *node = pop_message(current_process, -1); /* from ANY */
        if (!node)
          break; /* queue empty → -EAGAIN, caller blocks */
        if (node->msg.type == IPC_TYPE_INPUT && node->msg.data2 != 0) {
          char c = (char)node->msg.data1;
          ret = (arch_copy_to_user(ubuf, &c, 1) != 0) ? -EFAULT : 1;
          kfree(node);
          break;
        }
        kfree(node); /* release event / non-input: discard */
      }
    }
  } else if (o->type == OBJ_TYPE_PROCESS) {
    /* read = the process's live state as a text status block.  The process
     * object NOTIFIES its state THROUGH the unified object mechanism (parallels
     * OBJ_TYPE_WINDOW returning window_info) — not a side-channel.  Acquiring a
     * READ handle is the ambient "status query" gate (sys_handle_create PROC
     * branch); this is its payload.  A short buffer truncates, like the
     * REGKEY/WINDOW reads. */
    struct ps_info pi;
    if (proc_get_info(o->pid, &pi) != 0) {
      ret = -ESRCH;
    } else {
      char stbuf[256];
      int len = snprintf(
          stbuf, sizeof(stbuf),
          "pid=%d\nname=%s\nstate=%s\nprio=%d\ncpu_ms=%llu\noncpu=%d\n", pi.pid,
          pi.name, proc_state_name(pi.state), pi.priority,
          (unsigned long long)pi.cpu_time, pi.on_cpu);
      size_t cn = (len > 0) ? (size_t)len : 0;
      if (cn > n)
        cn = n;
      ret = (cn > 0 && arch_copy_to_user(ubuf, stbuf, cn) != 0) ? -EFAULT
                                                                : (long)cn;
    }
  } else {
    ret = -EINVAL;
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
  } else if (o->type == OBJ_TYPE_REGKEY) {
    /* write = registry_set the key's value (ASTRA §6.6).  Ownership stays
     * first-writer-wins (registry_set), so a granted REGKEY cannot hijack
     * another owner's key. */
    if (n == 0 || n > MAX_VAL_LEN) {
      ret = -EINVAL;
    } else {
      size_t cn = (n < MAX_VAL_LEN) ? n : (MAX_VAL_LEN - 1);
      char val[MAX_VAL_LEN];
      if (arch_copy_from_user(val, ubuf, cn) != 0) {
        ret = -EFAULT;
      } else {
        val[cn] = '\0';
        int owner =
            proc_is_machine(current_process) ? 0 : (int)current_process->pid;
        int rc = registry_set(o->path, val, owner);
        ret = (rc == 0) ? (long)n : (rc == -EACCES ? -EACCES : -EINVAL);
      }
    }
  } else if (o->type == OBJ_TYPE_CONSOLE) {
    /* stdout/stderr: resolve the caller's OWN window first (a process with its
     * own window renders there), else its controlling terminal (the launching
     * shell) — the folded-in FD_WIN path.  window_text_write copies the user
     * buffer and mirrors it to the UART. */
    int win_id = current_process
                     ? compositor_get_window_by_pid((int)current_process->pid)
                     : -1;
    if (win_id <= 0 && current_process)
      win_id = current_process->ctty_win;
    ret = window_text_write(win_id, (const char *)ubuf, n);
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
 * sys_object_ctl - OS1_object_ctl(handle, cmd, arg).
 * Type-specific control verbs on an object you hold a capability to.
 * PROCESS + OBJ_CTL_KILL needs OS1_RIGHT_DESTROY and terminates the target
 * (machine-level processes stay protected inside process_terminate).  Holding a
 * DESTROY capability IS the authority — a GRANTED destroy handle lets its holder
 * kill a process it could not reach by process_kill_allowed (seL4 delegation of
 * the kill right; acquisition was gated at handle_create time).
 */
long sys_object_ctl(int handle, int cmd, long arg) {
  (void)arg;

  /* PROCESS: terminate via a DESTROY capability (seL4 delegable kill). */
  if (cmd == OBJ_CTL_KILL) {
    long err = 0;
    struct kobject *o = pin_handle(handle, OS1_RIGHT_DESTROY, &err);
    if (!o)
      return err;
    long ret;
    if (o->type == OBJ_TYPE_PROCESS) {
      /* window-aware kill: the target + its windowless helpers die; its own
       * windowed children are spared (docs/PROCESS-KILL-MODEL.md). */
      process_kill_subtree(o->pid);
      ret = 0;
    } else {
      ret = -EINVAL;
    }
    obj_unref(o);
    return ret;
  }

  /* WINDOW control (ASTRA §6.7): minimize/restore/focus need OS1_RIGHT_WRITE,
   * close needs OS1_RIGHT_DESTROY.  The right IS the authority — a granted
   * window capability lets its holder (e.g. the dock /sys/bin/nxui) drive a
   * window it does not own; acquisition was gated at handle_create. */
  if (cmd == OBJ_CTL_MINIMIZE || cmd == OBJ_CTL_RESTORE ||
      cmd == OBJ_CTL_FOCUS || cmd == OBJ_CTL_CLOSE) {
    /* NOTE(OBJ-WIN-FOCUS): FOCUS needs only READ, so a READ-only window handle —
     * which handle_create hands out for ANY window, ungated — can redirect
     * keyboard focus.  This deliberately mirrors the compositor's unprivileged
     * click-to-focus (any user may focus any window with the mouse) so the
     * keyboard `focus` path is as open as the mouse.  minimize/restore/close stay
     * window-manager authority (WRITE/DESTROY, gated at handle_create).  Unifying
     * this with SYS_SET_FOCUS's stricter cross-pid rule is a security-phase item. */
    uint32_t need = (cmd == OBJ_CTL_CLOSE)  ? OS1_RIGHT_DESTROY
                    : (cmd == OBJ_CTL_FOCUS) ? OS1_RIGHT_READ
                                             : OS1_RIGHT_WRITE;
    long err = 0;
    struct kobject *o = pin_handle(handle, need, &err);
    if (!o)
      return err;
    long ret;
    if (o->type != OBJ_TYPE_WINDOW) {
      ret = -EINVAL;
    } else if (cmd == OBJ_CTL_MINIMIZE) {
      ret = compositor_minimize_window(o->window_id);
    } else if (cmd == OBJ_CTL_RESTORE) {
      ret = compositor_restore_window(o->window_id);
    } else if (cmd == OBJ_CTL_FOCUS) {
      ret = compositor_focus_window(o->window_id);
    } else { /* OBJ_CTL_CLOSE */
      /* Close TERMINATES the owning process (window-aware, docs/PROCESS-KILL-
       * MODEL.md): the target app and its windowless helpers die, while its own
       * windowed children are SPARED (the user closes those via their red
       * button).  Consistent with the titlebar red button (window_request_close).
       * Previously a plain process_terminate; before THAT it only destroyed the
       * WINDOW, leaving the app headless ("close does not kill").  Machine-level
       * owners stay protected inside process_terminate. */
      process_kill_subtree(o->pid);
      ret = 0;
    }
    obj_unref(o);
    return ret;
  }

  /* FILE: seek the object's shared byte offset (F4 M4.5).  Positioning needs only
   * the handle itself (any FILE capability), so write-at-offset works on a
   * WRITE-only handle too.  arg = absolute offset; returns the new offset. */
  if (cmd == OBJ_CTL_SEEK) {
    long err = 0;
    struct kobject *o = pin_handle(handle, 0, &err);
    if (!o)
      return err;
    long ret;
    if (o->type != OBJ_TYPE_FILE || arg < 0) {
      ret = -EINVAL;
    } else {
      o->offset = (uint64_t)arg;
      ret = (long)o->offset;
    }
    obj_unref(o);
    return ret;
  }

  /* FILE: report the CURRENT size in bytes (Stage 4: lseek SEEK_END / stat via the
   * object, so a FILE handle can fully back open()/read()/write()/lseek()).  Reads
   * the live size (another handle may have grown the file), falling back to the
   * cached open-time size. */
  if (cmd == OBJ_CTL_STAT) {
    long err = 0;
    struct kobject *o = pin_handle(handle, 0, &err);
    if (!o)
      return err;
    long ret;
    if (o->type != OBJ_TYPE_FILE) {
      ret = -EINVAL;
    } else {
      struct vfs_stat st;
      ret = (vfs_stat(o->path, &st) == 0) ? (long)st.size : (long)o->node.size;
    }
    obj_unref(o);
    return ret;
  }

  return -EINVAL;
}

/*
 * sys_object_lseek - POSIX lseek(2) on a FILE handle: reposition the shared byte
 * offset per 'whence' and return the new absolute position.  SEEK_END reads the
 * live size (vfs_stat) so it tracks growth by another handle.  A non-seekable
 * object (CONSOLE stdin/stdout/stderr, …) returns -ESPIPE.  Positioning needs
 * only the handle itself (no rights), matching OBJ_CTL_SEEK.
 */
long sys_object_lseek(int handle, long off, int whence) {
  long err = 0;
  struct kobject *o = pin_handle(handle, 0, &err);
  if (!o)
    return err;
  long ret;
  if (o->type != OBJ_TYPE_FILE) {
    ret = -ESPIPE; /* streams cannot seek */
  } else {
    long base;
    if (whence == SEEK_SET) {
      base = 0;
    } else if (whence == SEEK_CUR) {
      base = (long)o->offset;
    } else if (whence == SEEK_END) {
      struct vfs_stat st;
      base = (vfs_stat(o->path, &st) == 0) ? (long)st.size : (long)o->node.size;
    } else {
      base = -1; /* invalid whence */
    }
    if (base < 0) {
      ret = -EINVAL;
    } else {
      long npos = base + off;
      if (npos < 0) {
        ret = -EINVAL;
      } else {
        o->offset = (uint64_t)npos;
        ret = npos;
      }
    }
  }
  obj_unref(o);
  return ret;
}

/*
 * process_install_stdio - give a freshly created process its stdin/stdout/stderr
 * as capability handles (ASTRA §6.2: the fd table folds into the object table).
 * Handles 0/1/2 share ONE refcounted CONSOLE object — a process has a single
 * controlling terminal: handle 0 holds READ (stdin keyboard drain), handles 1/2
 * hold WRITE (stdout/stderr → the process's own window, else its ctty).  The
 * table is allocated eagerly so every process is born with the trio, replacing
 * the old fd-table pre-open.  Returns 0, or -ENOMEM.
 */
int process_install_stdio(struct process *p) {
  if (!p)
    return -EINVAL;
  if (handles_ensure(p) != 0)
    return -ENOMEM;
  struct kobject *con = kobj_alloc(OBJ_TYPE_CONSOLE);
  if (!con)
    return -ENOMEM;
  uint64_t flags;
  spin_lock_irqsave(&object_lock, &flags);
  /* Slots 0/1/2 of a just-allocated table are free; install the shared console
   * directly (not via the find-free path, which is for handles >= 3). */
  p->handles[0].obj = con;
  p->handles[0].rights = OS1_RIGHT_READ;
  p->handles[1].obj = con;
  p->handles[1].rights = OS1_RIGHT_WRITE;
  p->handles[2].obj = con;
  p->handles[2].rights = OS1_RIGHT_WRITE;
  con->refcount = 3; /* three handles reference it */
  spin_unlock_irqrestore(&object_lock, flags);
  return 0;
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
      kobj_free(o); /* object_lock -> kmalloc_lock order is consistent */
  }
  spin_unlock_irqrestore(&object_lock, flags);
  kfree(tbl);
}
