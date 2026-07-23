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

/*
 * struct kpipe - the payload of an OBJ_TYPE_PIPE kobject (ASTRA §6.2 anonymous
 * byte pipe).  Heap-allocated and pointed to by kobject.pipe so the buffer +
 * wait queue don't bloat every kobject.  `buf` is a ring of PIPE_BUF_SIZE bytes.
 * `readers`/`writers` are OPEN-handle counts (maintained by pipe_handle_count at
 * every handle lifecycle edge): a reader on an empty pipe blocks on `wq` until a
 * writer publishes data (wake_up) or the LAST writer closes (writers==0 → EOF).
 */
#define PIPE_BUF_SIZE 4096u
struct kpipe {
  uint8_t *buf;
  uint32_t head; /* next byte to read */
  uint32_t tail; /* next slot to write */
  uint32_t count;
  int readers;
  int writers;
  struct wait_queue_head rq; /* readers waiting for data / EOF */
  struct wait_queue_head wq; /* writers waiting for buffer space / EPIPE */
};

/*
 * struct kport - the payload of an OBJ_TYPE_PORT kobject: a Mach-style message
 * MAILBOX that is itself a capability (ASTRA §6.5).  Heap-allocated like kpipe.
 *
 * The rights on a handle ARE the port rights: OS1_RIGHT_WRITE is the SEND right
 * and OS1_RIGHT_READ the RECEIVE right.  A service acquires the port with READ
 * (becoming its owner) and hands clients attenuated SEND-only handles, so a
 * client names the SERVICE, never a pid — the seL4 rule ("no PID-by-number
 * access without a capability") that ambient SYS_SEND violates.
 *
 * ASTRA §6.5 says the semantics "ride on the existing B3 IPC layer": we reuse
 * struct ipc_message verbatim and only relocate the QUEUE (from a pid's mailbox
 * to the port object) and the AUTHORITY (from process_ipc_allowed to the
 * handle's rights).  `name` is the published identity clients resolve.
 */
#define PORT_QUEUE_MAX 32
#define PORT_NAME_MAX 32
struct kport {
  struct ipc_message q[PORT_QUEUE_MAX];
  uint32_t head, tail, count;
  int senders;   /* open handles holding the SEND right    */
  int receivers; /* open handles holding the RECEIVE right */
  int owner_pid;
  char name[PORT_NAME_MAX];
  struct wait_queue_head rq; /* receivers waiting for a message  */
  struct wait_queue_head wq; /* senders waiting for queue space  */
};

/* Named-port registry: the name→port map that lets a client acquire a send
 * capability BY NAME.  Small and fixed: ports are system services, not a
 * per-application resource.  Guarded by object_lock. */
#define PORT_TABLE_MAX 16
static struct kobject *port_table[PORT_TABLE_MAX];

/* port_find_locked - resolve a published port name.  Caller holds object_lock. */
static struct kobject *port_find_locked(const char *name) {
  for (int i = 0; i < PORT_TABLE_MAX; i++) {
    if (port_table[i] && port_table[i]->port &&
        strcmp(port_table[i]->port->name, name) == 0)
      return port_table[i];
  }
  return NULL;
}

/* endpoint_handle_count - adjust a PIPE's or PORT's open endpoint counts by
 * 'delta' for a handle carrying 'rights'.  Called (under object_lock) at EVERY
 * handle lifecycle edge — install (+1), close/destroy/redirect-overwrite (-1) —
 * so the counts exactly track live handles.  A no-op for other types, so callers
 * need not special-case.  For a pipe, closing the last writer wakes blocked
 * readers (EOF) and closing the last reader wakes blocked writers (EPIPE); a
 * port behaves the same way, so a client blocked on a reply is released when the
 * service dies instead of hanging forever. */
static void pipe_handle_count(struct kobject *o, uint32_t rights, int delta) {
  if (o && o->type == OBJ_TYPE_PORT && o->port) {
    if (rights & OS1_RIGHT_READ) {
      o->port->receivers += delta;
      if (delta < 0 && o->port->receivers == 0) {
        /* The service released its receive right: UNPUBLISH the name here, under
         * object_lock, so no new client can resolve a send capability to a dead
         * service.  Doing it at free time instead would race, because kobj_free
         * runs both inside and outside the lock. */
        for (int i = 0; i < PORT_TABLE_MAX; i++) {
          if (port_table[i] == o)
            port_table[i] = NULL;
        }
        wake_up(&o->port->wq); /* service gone → senders stop waiting */
      }
    }
    if (rights & OS1_RIGHT_WRITE) {
      o->port->senders += delta;
      if (delta < 0 && o->port->senders == 0)
        wake_up(&o->port->rq); /* last client gone → receiver wakes */
    }
    return;
  }
  if (!o || o->type != OBJ_TYPE_PIPE || !o->pipe)
    return;
  if (rights & OS1_RIGHT_READ) {
    o->pipe->readers += delta;
    if (delta < 0 && o->pipe->readers == 0)
      wake_up(&o->pipe->wq); /* last reader gone → writers get EPIPE */
  }
  if (rights & OS1_RIGHT_WRITE) {
    o->pipe->writers += delta;
    if (delta < 0 && o->pipe->writers == 0)
      wake_up(&o->pipe->rq); /* last writer gone → readers see EOF */
  }
}

/* pipe still-block predicates for kthread_block (return 1 = keep sleeping).  A
 * reader sleeps while the pipe is empty AND a writer still exists; a writer
 * sleeps while the pipe is full AND a reader still exists.  Evaluated under the
 * wait queue lock, serialised against the producer/consumer's wake_up(). */
static int pipe_readable_block(void *arg) {
  struct kpipe *kp = ((struct kobject *)arg)->pipe;
  return kp->count == 0 && kp->writers > 0;
}
static int pipe_writable_block(void *arg) {
  struct kpipe *kp = ((struct kobject *)arg)->pipe;
  return kp->count == PIPE_BUF_SIZE && kp->readers > 0;
}
/* Port equivalents: a receiver sleeps while the mailbox is empty AND a sender
 * still exists; a sender sleeps while it is full AND a receiver still exists. */
static int port_readable_block(void *arg) {
  struct kport *kp = ((struct kobject *)arg)->port;
  return kp->count == 0 && kp->senders > 0;
}
static int port_writable_block(void *arg) {
  struct kport *kp = ((struct kobject *)arg)->port;
  return kp->count == PORT_QUEUE_MAX && kp->receivers > 0;
}

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
  if (o->type == OBJ_TYPE_PIPE && o->pipe) {
    kfree(o->pipe->buf);
    kfree(o->pipe);
  }
  if (o->type == OBJ_TYPE_PORT && o->port)
    kfree(o->port); /* already unpublished when its last receiver closed */
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
      pipe_handle_count(o, rights, +1); /* track a new pipe reader/writer */
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
  /* CREATE is acquisition-only (open(O_CREAT) semantics, FS namespace):
   * capture it, then strip it via the OS1_RIGHT_ALL mask so it never lands
   * in the installed handle's rights (cannot be duplicated/granted). */
  int want_create = (rights & OS1_RIGHT_CREATE) != 0;
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
    if (rr != 0) {
      /* O_CREAT semantics (ASTRA §6.8: open(O_CREAT) → handle_create): a
       * missing path with CREATE+WRITE is created as an empty FILE through
       * the provider, behind the SAME vfs_write_allowed seam every other
       * write-class modification uses (S-ALIGN F6).  CREATE without WRITE
       * is meaningless and stays -ENOENT. */
      if (!want_create || !(rights & OS1_RIGHT_WRITE))
        return -ENOENT;
      long cperm = vfs_write_allowed(resolved);
      if (cperm != 0)
        return cperm;
      if (vfs_create(resolved, VFS_TYPE_FILE) != 0)
        return -EIO;
      rr = vfs_resolve_object(resolved, &ref);
      if (rr != 0)
        return -ENOENT;
    }

    struct kobject *o;
    if (ref.obj_type == OBJ_TYPE_FILE) {
      /* Ambient gate to ACQUIRE a write capability — the SAME
       * vfs_write_allowed() seam SYS_FILE_WRITE/SYS_UNLINK check, so the
       * path-based and handle-based entry points cannot drift (S-ALIGN F6).
       * Use of an already-held handle is rights-only (seL4/Mach delegation). */
      if (rights & OS1_RIGHT_WRITE) {
        long wperm = vfs_write_allowed(resolved);
        if (wperm != 0)
          return wperm;
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

  /*
   * OS1_NS_PORT — acquire a capability to a NAMED service port (ASTRA §6.5).
   * This is the verb that lets a client address a SERVICE instead of a pid.
   *
   * Acquisition policy:
   *   - asking for the RECEIVE right (OS1_RIGHT_READ) PUBLISHES the port and
   *     makes the caller its owner — that is how a service announces itself.
   *     A second receiver for an existing name is refused (-EADDRINUSE-ish
   *     -EEXIST): one mailbox, one service, so a rogue process cannot steal a
   *     service's identity by racing it.
   *   - asking without READ yields a SEND-only capability to an EXISTING port;
   *     possession is thereafter the authority (no ambient pid check).
   */
  if (ns == OS1_NS_PORT) {
    (void)type;
    if (handles_ensure(cur) != 0)
      return -ENOMEM;
    int want_recv = (rights & OS1_RIGHT_READ) != 0;

    uint64_t pflags;
    spin_lock_irqsave(&object_lock, &pflags);
    struct kobject *existing = port_find_locked(kpath);
    if (!want_recv) {
      if (!existing) {
        spin_unlock_irqrestore(&object_lock, pflags);
        return -ENOENT; /* no such service published */
      }
      int h = handle_install_locked(cur, existing, rights & ~OS1_RIGHT_READ);
      spin_unlock_irqrestore(&object_lock, pflags);
      return h;
    }
    if (existing) {
      spin_unlock_irqrestore(&object_lock, pflags);
      return -EEXIST; /* a service already owns this name */
    }
    int slot = -1;
    for (int i = 0; i < PORT_TABLE_MAX; i++) {
      if (!port_table[i]) {
        slot = i;
        break;
      }
    }
    spin_unlock_irqrestore(&object_lock, pflags);
    if (slot < 0)
      return -ENOSPC; /* port table full */

    /* Allocate with the lock released (kmalloc), then publish under it. */
    struct kport *kp = kmalloc(sizeof(struct kport));
    if (!kp)
      return -ENOMEM;
    memset(kp, 0, sizeof(*kp));
    kp->owner_pid = (int)cur->pid;
    snprintf(kp->name, PORT_NAME_MAX, "%s", kpath);
    INIT_LIST_HEAD(&kp->rq.task_list);
    spin_lock_init(&kp->rq.lock);
    INIT_LIST_HEAD(&kp->wq.task_list);
    spin_lock_init(&kp->wq.lock);

    struct kobject *o = kobj_alloc(OBJ_TYPE_PORT);
    if (!o) {
      kfree(kp);
      return -ENOMEM;
    }
    o->port = kp;
    o->pid = (int)cur->pid;

    spin_lock_irqsave(&object_lock, &pflags);
    if (port_find_locked(kpath)) { /* lost a publish race */
      spin_unlock_irqrestore(&object_lock, pflags);
      kobj_free(o); /* never installed: refcount still 0 */
      return -EEXIST;
    }
    int h = handle_install_locked(cur, o, rights);
    if (h >= 0)
      port_table[slot] = o;
    spin_unlock_irqrestore(&object_lock, pflags);
    if (h < 0)
      kobj_free(o);
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
     * associated capability).  A WRITE handle needs CAP_REG_WRITE to acquire —
     * the SAME registry_write_allowed() seam sys_registry and regfs check, so
     * the three entry points cannot drift (S-ALIGN F5); reads are open.  The
     * key is stored in the kobject's path field, capped at the registry key
     * length so create/get/set all agree on the same string. */
    if ((rights & OS1_RIGHT_WRITE) && !registry_write_allowed())
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
  pipe_handle_count(o, e->rights, -1); /* untrack this pipe reader/writer */
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

/*
 * sys_pipe - OS1low_pipe(int fds[2]).  Create an anonymous byte pipe (ASTRA §6.2
 * OBJ_TYPE_PIPE) and install BOTH ends in the caller's handle table: fds[0] is
 * the READ end, fds[1] the WRITE end (POSIX pipe() order).  One kobject, two
 * handles (rights = READ / WRITE); reader/writer counts start at 1/1 via the
 * install path.  Returns 0, or -ENOMEM/-EMFILE/-EFAULT.
 */
long sys_pipe(int *ufds) {
  struct process *cur = current_process;
  if (!cur)
    return -EPERM;
  if (handles_ensure(cur) != 0)
    return -ENOMEM;

  struct kpipe *kp = kmalloc(sizeof(struct kpipe));
  if (!kp)
    return -ENOMEM;
  memset(kp, 0, sizeof(*kp));
  kp->buf = kmalloc(PIPE_BUF_SIZE);
  if (!kp->buf) {
    kfree(kp);
    return -ENOMEM;
  }
  INIT_LIST_HEAD(&kp->rq.task_list);
  spin_lock_init(&kp->rq.lock);
  INIT_LIST_HEAD(&kp->wq.task_list);
  spin_lock_init(&kp->wq.lock);

  struct kobject *o = kobj_alloc(OBJ_TYPE_PIPE);
  if (!o) {
    kfree(kp->buf);
    kfree(kp);
    return -ENOMEM;
  }
  o->pipe = kp;

  /* Both ends carry TRANSFER and DUPLICATE alongside their direction right.
   * Without TRANSFER a pipe end cannot be handed to another process at all
   * (sys_cap_grant requires it), which would make a pipe usable only INSIDE the
   * creating process — defeating its whole purpose as an IPC channel and, in
   * particular, breaking the out-of-line request transfer the execution service
   * depends on.  Note this does NOT widen access to the pipe itself: the
   * direction right is still what gates read vs write, and a grant can only
   * attenuate. */
  uint64_t flags;
  spin_lock_irqsave(&object_lock, &flags);
  int rfd = handle_install_locked(
      cur, o, OS1_RIGHT_READ | OS1_RIGHT_TRANSFER | OS1_RIGHT_DUPLICATE);
  int wfd = (rfd >= 0) ? handle_install_locked(cur, o,
                                               OS1_RIGHT_WRITE |
                                                   OS1_RIGHT_TRANSFER |
                                                   OS1_RIGHT_DUPLICATE)
                       : rfd;
  spin_unlock_irqrestore(&object_lock, flags);

  if (rfd < 0 || wfd < 0) {
    if (rfd >= 0)
      sys_handle_close(rfd); /* drops the read end; frees o at refcount 0 */
    else
      kobj_free(o); /* never installed: refcount 0, free the whole pipe */
    return (rfd < 0) ? rfd : wfd;
  }

  int fds[2] = {rfd, wfd};
  if (arch_copy_to_user(ufds, fds, sizeof(fds)) != 0) {
    sys_handle_close(rfd);
    sys_handle_close(wfd);
    return -EFAULT;
  }
  return 0;
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
 * sys_port_send_caps - send a message through a PORT, TRANSFERRING capabilities
 * with it (ASTRA §6.5, Mach: "ports are first-class objects ... a port is itself
 * a capability", and a Mach message carries port RIGHTS).
 *
 * Why this verb has to exist: a handle index is meaningful only inside ONE
 * process's table, so a client that merely names its fds in a message is
 * describing slots the service cannot resolve.  The alternative — cap_grant
 * first — is addressed BY PID, which would drag pid-addressing straight back
 * into the one path that exists to remove it (you would need the service's pid
 * to talk to a service you found by NAME).  Transferring the rights along the
 * message keeps discovery and delegation both capability-based.
 *
 * The kernel installs each handle into the RECEIVER (the port's owner) and
 * rewrites the leading int slots of the message payload with the indices as the
 * receiver sees them, so the service reads valid handles with no translation
 * step of its own.  Attenuation only: the transferred rights are the sender's.
 */
long sys_port_send_caps(int handle, const void *umsg, const int *ufds,
                        int nfds) {
  if (nfds < 0 || nfds > 4)
    return -EINVAL; /* payload space for the rewritten indices */
  long err = 0;
  struct kobject *o = pin_handle(handle, OS1_RIGHT_WRITE, &err);
  if (!o)
    return err;

  long ret;
  struct kport *kp = o->port;
  struct ipc_message m;
  int fds[4];
  if (o->type != OBJ_TYPE_PORT || !kp) {
    ret = -EINVAL;
  } else if (arch_copy_from_user(&m, umsg, sizeof(m)) != 0 ||
             (nfds > 0 &&
              arch_copy_from_user(fds, ufds, (size_t)nfds * sizeof(int)) != 0)) {
    ret = -EFAULT;
  } else {
    struct process *rcv = process_find_by_pid(kp->owner_pid);
    if (!rcv) {
      ret = -ESRCH;
    } else if (handles_ensure(rcv) != 0) {
      ret = -ENOMEM;
    } else {
      int installed[4];
      int n_ok = 0;
      ret = 0;
      uint64_t f2;
      spin_lock_irqsave(&object_lock, &f2);
      for (int i = 0; i < nfds; i++) {
        if (fds[i] < 0 || fds[i] >= NPROC_HANDLES || !current_process ||
            !current_process->handles ||
            !current_process->handles[fds[i]].obj) {
          ret = -EBADF;
          break;
        }
        struct handle_entry *src = &current_process->handles[fds[i]];
        int h = handle_install_locked(rcv, src->obj, src->rights);
        if (h < 0) {
          ret = h;
          break;
        }
        installed[n_ok++] = h;
      }
      if (ret != 0) {
        /* Roll back a partial transfer: leaving half the rights installed would
         * hand the service capabilities for a request it will never see. */
        for (int i = 0; i < n_ok; i++) {
          struct handle_entry *e = &rcv->handles[installed[i]];
          pipe_handle_count(e->obj, e->rights, -1);
          if (--e->obj->refcount <= 0)
            kobj_free(e->obj);
          e->obj = NULL;
          e->rights = 0;
        }
        spin_unlock_irqrestore(&object_lock, f2);
      } else {
        /* Publish the receiver-side indices in the payload, then enqueue. */
        for (int i = 0; i < nfds; i++)
          memcpy(m.payload + (size_t)i * sizeof(int), &installed[i],
                 sizeof(int));
        m.from = current_process ? (int)current_process->pid : 0;
        if (kp->receivers == 0) {
          ret = -EPIPE;
        } else if (kp->count >= PORT_QUEUE_MAX) {
          ret = -EAGAIN; /* caller retries; blocking here would hold the lock */
        } else {
          kp->q[kp->tail] = m;
          kp->tail = (kp->tail + 1) % PORT_QUEUE_MAX;
          kp->count++;
          ret = (long)sizeof(m);
        }
        spin_unlock_irqrestore(&object_lock, f2);
        if (ret > 0)
          wake_up(&kp->rq);
      }
    }
  }
  obj_unref(o);
  return ret;
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
        /* A2 (capability read/write symmetry): re-sync the cached node from the
         * handle's path before reading, exactly as sys_object_write refreshes it
         * after writing.  Without this a long-lived READ handle keeps the size it
         * saw at handle_create and never sees data a later writer appended.  The
         * path is the handle's stable identity; the node is a cache.  Best-effort:
         * if the path vanished, fall back to the cached node (an open handle to a
         * since-deleted file still reads what it last resolved). */
        (void)vfs_open(o->path, &o->node);
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
    /* Lineage is read separately rather than widened into ps_info: that struct
     * is a userland ABI read as an ARRAY by ps/nxproc, so growing it would move
     * every element. */
    int lin_parent = 0, lin_owner = 0;
    (void)proc_get_lineage(o->pid, &lin_parent, &lin_owner);
    if (proc_get_info(o->pid, &pi) != 0) {
      ret = -ESRCH;
    } else {
      char stbuf[256];
      int len = snprintf(
          stbuf, sizeof(stbuf),
          "pid=%d\nname=%s\nstate=%s\nprio=%d\ncpu_ms=%llu\noncpu=%d\n"
          "parent=%d\nowner=%d\n",
          pi.pid, pi.name, proc_state_name(pi.state), pi.priority,
          (unsigned long long)pi.cpu_time, pi.on_cpu, lin_parent, lin_owner);
      size_t cn = (len > 0) ? (size_t)len : 0;
      if (cn > n)
        cn = n;
      ret = (cn > 0 && arch_copy_to_user(ubuf, stbuf, cn) != 0) ? -EFAULT
                                                                : (long)cn;
    }
  } else if (o->type == OBJ_TYPE_PORT) {
    /* Port RECEIVE (needs the receive right, checked by pin_handle above).
     * Dequeues one whole ipc_message — the message is the unit, unlike the
     * pipe's byte stream.  Blocks while empty and a sender still exists;
     * returns 0 when the last sender is gone, so a service loop terminates
     * instead of hanging. */
    struct kport *kp = o->port;
    if (!kp) {
      ret = -EIO;
    } else if (n < sizeof(struct ipc_message)) {
      ret = -EINVAL; /* a partial message would desync the mailbox */
    } else {
      for (;;) {
        uint64_t f2;
        spin_lock_irqsave(&object_lock, &f2);
        if (kp->count > 0) {
          struct ipc_message m = kp->q[kp->head];
          kp->head = (kp->head + 1) % PORT_QUEUE_MAX;
          kp->count--;
          spin_unlock_irqrestore(&object_lock, f2);
          wake_up(&kp->wq); /* freed a slot → wake a blocked sender */
          ret = (arch_copy_to_user(ubuf, &m, sizeof(m)) != 0)
                    ? -EFAULT
                    : (long)sizeof(m);
          break;
        }
        if (kp->senders == 0) { /* nobody left to send → end of service */
          spin_unlock_irqrestore(&object_lock, f2);
          ret = 0;
          break;
        }
        spin_unlock_irqrestore(&object_lock, f2);
        kthread_block(&kp->rq, port_readable_block, o);
      }
    }
  } else if (o->type == OBJ_TYPE_PIPE) {
    /* Pipe read: block until data is available or every writer has closed
     * (EOF → 0).  The still-block predicate is re-checked under the reader wait
     * queue lock so a writer's publish-then-wake_up can never be lost. */
    struct kpipe *kp = o->pipe;
    if (n == 0 || !kp) {
      ret = (n == 0) ? 0 : -EIO;
    } else {
      uint8_t *kb = kmalloc(n);
      if (!kb) {
        ret = -ENOMEM;
      } else {
        for (;;) {
          uint64_t f2;
          spin_lock_irqsave(&object_lock, &f2);
          if (kp->count > 0) {
            uint32_t take = (kp->count < n) ? kp->count : (uint32_t)n;
            for (uint32_t i = 0; i < take; i++) {
              kb[i] = kp->buf[kp->head];
              kp->head = (kp->head + 1) % PIPE_BUF_SIZE;
            }
            kp->count -= take;
            spin_unlock_irqrestore(&object_lock, f2);
            wake_up(&kp->wq); /* freed space → wake a blocked writer */
            ret = (arch_copy_to_user(ubuf, kb, take) != 0) ? -EFAULT
                                                           : (long)take;
            break;
          }
          if (kp->writers == 0) { /* empty and no writers → EOF */
            spin_unlock_irqrestore(&object_lock, f2);
            ret = 0;
            break;
          }
          spin_unlock_irqrestore(&object_lock, f2);
          kthread_block(&kp->rq, pipe_readable_block, o);
        }
        kfree(kb);
      }
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
        int rc = registry_set(o->path, val, registry_caller_owner());
        ret = (rc == 0) ? (long)n : (rc == -EACCES ? -EACCES : -EINVAL);
      }
    }
  } else if (o->type == OBJ_TYPE_CONSOLE) {
    /* stdout/stderr: resolve the caller's OWN window first (a process with its
     * own window renders there), else its controlling terminal (the launching
     * shell) — the folded-in FD_WIN path.  window_text_write copies the user
     * buffer and mirrors it to the UART.
     *
     * self_rendered (sched.h) processes are excluded from the own-window
     * branch: once a process has blitted its OWN pixel content into a
     * window (SYS_WINDOW_BLIT, syscall_dispatch.c), that window's buffer is
     * its custom UI, not a text console — an implicit stdout write here
     * (printf, an uncaught crash message, ...) must not draw glyphs over
     * it.  Falls through to ctty_win (a self-rendering app's own printf
     * still lands SOMEWHERE sane if it has a controlling terminal) or -1
     * (window_text_write's `win_id > 0` gate then skips the compositor
     * draw entirely; the UART mirror above is unaffected either way).
     * SYS_WINDOW_WRITE (printf_win) is a SEPARATE, explicit call a self-
     * rendering app can still use on purpose — only this implicit fd=1/2
     * path is gated. */
    int win_id = (current_process && !current_process->self_rendered)
                     ? compositor_get_window_by_pid((int)current_process->pid)
                     : -1;
    if (win_id <= 0 && current_process)
      win_id = current_process->ctty_win;
    ret = window_text_write(win_id, (const char *)ubuf, n);
  } else if (o->type == OBJ_TYPE_PORT) {
    /* Port SEND (needs the send right).  The handle's WRITE right IS the
     * authority — deliberately NO process_ipc_allowed() check, exactly as the
     * OBJ_TYPE_PROCESS send path documents: ambient checks gate ACQUISITION,
     * not use.  That is the whole point of a port: the client never names a
     * pid.  `from` is stamped by the kernel so it cannot be forged. */
    struct kport *kp = o->port;
    if (!kp) {
      ret = -EIO;
    } else if (n != sizeof(struct ipc_message)) {
      ret = -EINVAL;
    } else {
      struct ipc_message m;
      if (arch_copy_from_user(&m, ubuf, sizeof(m)) != 0) {
        ret = -EFAULT;
      } else {
        m.from = current_process ? (int)current_process->pid : 0;
        for (;;) {
          uint64_t f2;
          spin_lock_irqsave(&object_lock, &f2);
          if (kp->receivers == 0) { /* service gone → no point queueing */
            spin_unlock_irqrestore(&object_lock, f2);
            ret = -EPIPE;
            break;
          }
          if (kp->count < PORT_QUEUE_MAX) {
            kp->q[kp->tail] = m;
            kp->tail = (kp->tail + 1) % PORT_QUEUE_MAX;
            kp->count++;
            spin_unlock_irqrestore(&object_lock, f2);
            wake_up(&kp->rq); /* message available → wake the service */
            ret = (long)sizeof(m);
            break;
          }
          spin_unlock_irqrestore(&object_lock, f2);
          kthread_block(&kp->wq, port_writable_block, o); /* mailbox full */
        }
      }
    }
  } else if (o->type == OBJ_TYPE_PIPE) {
    /* Pipe write: append to the ring buffer, blocking while it is full and a
     * reader still exists; -EPIPE if every reader has closed (no SIGPIPE here).
     * Wakes a blocked reader after each chunk lands. */
    struct kpipe *kp = o->pipe;
    if (n == 0 || !kp) {
      ret = (n == 0) ? 0 : -EIO;
    } else {
      uint8_t *kb = kmalloc(n);
      if (!kb) {
        ret = -ENOMEM;
      } else if (arch_copy_from_user(kb, ubuf, n) != 0) {
        kfree(kb);
        ret = -EFAULT;
      } else {
        size_t done = 0;
        ret = 0;
        while (done < n) {
          uint64_t f2;
          spin_lock_irqsave(&object_lock, &f2);
          if (kp->readers == 0) { /* no reader will ever consume → broken pipe */
            spin_unlock_irqrestore(&object_lock, f2);
            ret = (done > 0) ? (long)done : -EPIPE;
            break;
          }
          uint32_t space = PIPE_BUF_SIZE - kp->count;
          if (space == 0) { /* full: wait for a reader to drain */
            spin_unlock_irqrestore(&object_lock, f2);
            kthread_block(&kp->wq, pipe_writable_block, o);
            continue;
          }
          uint32_t chunk = (n - done < space) ? (uint32_t)(n - done) : space;
          for (uint32_t i = 0; i < chunk; i++) {
            kp->buf[kp->tail] = kb[done + i];
            kp->tail = (kp->tail + 1) % PIPE_BUF_SIZE;
          }
          kp->count += chunk;
          done += chunk;
          spin_unlock_irqrestore(&object_lock, f2);
          wake_up(&kp->rq); /* data available → wake a blocked reader */
        }
        if (ret == 0)
          ret = (long)done;
        kfree(kb);
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
  long err = 0;
  struct kobject *o = pin_handle(handle, OS1_RIGHT_WAIT, &err);
  if (!o)
    return err;

  long ret;
  if (o->type == OBJ_TYPE_PROCESS) {
    /* Phase 2: `arg`, if non-NULL, is a user int* that receives the reaped
     * process's exit_code (raw). The libc personality (waitpid) wraps it in
     * the POSIX status encoding. Only meaningful on reap (ret == pid). */
    int code = 0;
    ret = process_wait(o->pid, &code);
    if (arg) {
      extern int arch_copy_to_user(void *dst, const void *src, size_t n);
      (void)arch_copy_to_user((void *)arg, &code, sizeof(code));
    }
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

  /* PROCESS job control (Phase 2): suspend/resume via the same DESTROY
   * capability as kill — process-control authority. */
  if (cmd == OBJ_CTL_STOP || cmd == OBJ_CTL_CONT) {
    long err = 0;
    struct kobject *o = pin_handle(handle, OS1_RIGHT_DESTROY, &err);
    if (!o)
      return err;
    long ret;
    if (o->type == OBJ_TYPE_PROCESS)
      ret = (cmd == OBJ_CTL_STOP) ? process_stop(o->pid) : process_cont(o->pid);
    else
      ret = -EINVAL;
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

  /*
   * PROCESS: hand a spawned job to its LOGICAL owner (Q3, ASTRA §6.5).
   *
   * Authority is deliberately doubled: DESTROY on the target (you must already
   * control the process) AND a privileged caller — because this DELEGATES
   * kill/stop/cont authority over that process to `arg`.  Without the
   * privilege gate any process could hand its child to a third party, or
   * re-home itself under a victim to borrow their authority chain.
   */
  if (cmd == OBJ_CTL_SETOWNER) {
    long err = 0;
    struct kobject *o = pin_handle(handle, OS1_RIGHT_DESTROY, &err);
    if (!o)
      return err;
    long ret;
    if (o->type != OBJ_TYPE_PROCESS || arg <= 0) {
      ret = -EINVAL;
    } else if (!current_process || !proc_is_privileged(current_process)) {
      ret = -EPERM;
    } else {
      ret = process_set_owner(o->pid, (int)arg);
    }
    obj_unref(o);
    return ret;
  }

  /* FILE: truncate to empty through an open descriptor (Phase 1 leftover:
   * ftruncate).  Needs OS1_RIGHT_WRITE — this destroys data, unlike SEEK.  The
   * provider's primitive is whole-file-replace, so an offset-0 zero-length
   * vfs_write_file() empties the file; this is the one truncation a POSIX
   * write(fd,...,0) cannot express (that is defined as a no-op).  Non-empty
   * lengths are composed in libc's ftruncate() and never arrive here. */
  if (cmd == OBJ_CTL_TRUNCATE) {
    long err = 0;
    struct kobject *o = pin_handle(handle, OS1_RIGHT_WRITE, &err);
    if (!o)
      return err;
    long ret;
    if (o->type != OBJ_TYPE_FILE || arg != 0) {
      ret = -EINVAL;
    } else if (vfs_write_file(o->path, "", 0, 0) < 0) {
      ret = -EIO;
    } else {
      o->offset = 0;
      (void)vfs_open(o->path, &o->node); /* refresh the cached size */
      ret = 0;
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
 * process_redirect_child_fd - install the SPAWNER's open handle 'parent_fd' into
 * the freshly-created child's slot 'child_slot', OVERWRITING the pre-installed
 * console there (Phase 4 shell redirection: `<`/`>`/`>>`/`2>`).  This is the
 * fork+dup2 primitive expressed as a capability dup — the child gets a SECOND
 * handle to the SAME kobject (shared FILE offset, ASTRA §6.2), so its
 * read(0)/write(1) flow to the file instead of the terminal.  Called from
 * dispatch_spawn BEFORE the child is enqueued, with the spawner as
 * current_process; the spawner closes its own copies after the spawn returns.
 * Returns 0, or -EINVAL/-EBADF/-ENOMEM.
 */
int process_redirect_child_fd_from(struct process *owner, struct process *child,
                                   int child_slot, int parent_fd) {
  struct process *parent = owner; /* the fd's owner; not necessarily the spawner */
  if (!child || !parent)
    return -EINVAL;
  if (child_slot < 0 || child_slot >= NPROC_HANDLES ||
      parent_fd < 0 || parent_fd >= NPROC_HANDLES)
    return -EBADF;
  if (!parent->handles)
    return -EBADF;
  if (handles_ensure(child) != 0) /* normally already done by install_stdio */
    return -ENOMEM;

  struct kobject *to_free = NULL;
  uint64_t flags;
  spin_lock_irqsave(&object_lock, &flags);
  struct handle_entry *src = &parent->handles[parent_fd];
  if (!src->obj) {
    spin_unlock_irqrestore(&object_lock, flags);
    return -EBADF;
  }
  /* Drop whatever the child already held there (its shared console handle) and
   * point the slot at the spawner's object with the SAME rights. */
  struct handle_entry *dst = &child->handles[child_slot];
  struct kobject *old = dst->obj;
  uint32_t old_rights = dst->rights;
  dst->obj = src->obj;
  dst->rights = src->rights;
  src->obj->refcount++;
  pipe_handle_count(src->obj, src->rights, +1); /* child gains a pipe end */
  if (old) {
    pipe_handle_count(old, old_rights, -1); /* child loses the old handle */
    if (--old->refcount <= 0)
      to_free = old; /* last ref to the console → free outside the lock */
  }
  spin_unlock_irqrestore(&object_lock, flags);
  if (to_free)
    kobj_free(to_free);
  return 0;
}

/* process_redirect_child_fd - the spawner-is-the-source form (today's callers:
 * the SYS_SPAWN redirection list, where the fds belong to the process calling
 * spawn).  Kept as a thin wrapper so the source stays parameterised: an
 * execution SERVICE spawning on a client's behalf needs the source to be the
 * CLIENT, not itself (Q2 hybrid).  Splitting the source out now means that path
 * is a caller change rather than a rewrite. */
int process_redirect_child_fd(struct process *child, int child_slot,
                              int parent_fd) {
  return process_redirect_child_fd_from(current_process, child, child_slot,
                                        parent_fd);
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
    pipe_handle_count(o, tbl[i].rights, -1); /* untrack pipe reader/writer */
    tbl[i].obj = NULL;
    if (--o->refcount <= 0)
      kobj_free(o); /* object_lock -> kmalloc_lock order is consistent */
  }
  spin_unlock_irqrestore(&object_lock, flags);
  kfree(tbl);
}
