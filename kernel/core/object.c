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
 *   - Attenuation only: duplicate()/grant() compute new_rights &
 * current_rights, so rights can never grow (no escalation), mirroring the
 * caps.h spawn cut.
 *   - Acquisition is ambient-gated: getting a write FILE capability still needs
 *     CAP_FS_WRITE + the /bin,/sys ACL; getting a PROCESS capability needs the
 *     same authority as kill (self/descendant/privileged).  Once held, the
 *     handle is the only thing that matters.
 *
 * Locking: one global object_lock guards refcounts and slot install/close/grant
 * (short, allocation-free critical sections).  Blocking VFS I/O runs OUTSIDE
 * the lock via a pin (refcount++) / use / unpin pattern, so a concurrent
 * close() cannot free an object mid-I/O.  Lock order object_lock ->
 * kmalloc_lock is consistent (kmalloc/kfree never take object_lock), so kfree()
 * under the lock cannot deadlock.
 */
#include <kernel/cpu.h>
#include <kernel/graphics.h> /* OBJ_TYPE_WINDOW backends: compositor_* (§6.7) */
#include <kernel/hal_uaccess.h>
#include <kernel/kmalloc.h>
#include <kernel/nx_contract.h>
#include <kernel/object.h>
#include <kernel/printk.h>
#include <kernel/registry.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/types.h>
#include <kernel/vfs.h>

/* Upper bound for a single object read/write bounce buffer (mirrors the
 * dispatcher's SYSCALL_MAX_IO_BYTES so a user size argument cannot ask the
 * kernel for an absurd allocation). */
#define OBJ_MAX_IO_BYTES (16u * 1024u * 1024u)


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
 * wait queue don't bloat every kobject.  `buf` is a ring of PIPE_BUF_SIZE
 * bytes. `readers`/`writers` are OPEN-handle counts (maintained by
 * pipe_handle_count at every handle lifecycle edge): a reader on an empty pipe
 * blocks on `wq` until a writer publishes data (wake_up) or the LAST writer
 * closes (writers==0 → EOF).
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
  uint32_t q_cap_count[PORT_QUEUE_MAX];
  uint32_t head, tail, count;
  int senders;   /* open handles holding the SEND right    */
  int receivers; /* open handles holding the RECEIVE right */
  int owner_pid;
  int private_port; /* 0 public, 1 machine-only, 2 descendant-scoped */
  char name[PORT_NAME_MAX];
  struct wait_queue_head rq; /* receivers waiting for a message  */
  struct wait_queue_head wq; /* senders waiting for queue space  */
};

/* Named-port registry: the name→port map that lets a client acquire a send
 * capability BY NAME.  Small and fixed: ports are system services, not a
 * per-application resource.  Guarded by object_lock. */
#define PORT_TABLE_MAX 16
static struct kobject *port_table[PORT_TABLE_MAX];

/* port_find_locked - resolve a published port name.  Caller holds object_lock.
 */
static struct kobject *port_find_locked(const char *name) {
  for (int i = 0; i < PORT_TABLE_MAX; i++) {
    if (port_table[i] && port_table[i]->port &&
        strcmp(port_table[i]->port->name, name) == 0)
      return port_table[i];
  }
  return NULL;
}

enum endpoint_wake_flags {
  ENDPOINT_WAKE_NONE = 0U,
  ENDPOINT_WAKE_READERS = UINT32_C(1),
  ENDPOINT_WAKE_WRITERS = UINT32_C(2)
};

static void endpoint_handle_add_locked(struct kobject *object,
                                       uint32_t rights) {
  if (object != NULL && object->type == OBJ_TYPE_PORT && object->port != NULL) {
    if ((rights & OS1_RIGHT_READ) != 0U)
      ++object->port->receivers;
    if ((rights & OS1_RIGHT_WRITE) != 0U)
      ++object->port->senders;
    return;
  }
  if (object == NULL || object->type != OBJ_TYPE_PIPE || object->pipe == NULL)
    return;
  if ((rights & OS1_RIGHT_READ) != 0U)
    ++object->pipe->readers;
  if ((rights & OS1_RIGHT_WRITE) != 0U)
    ++object->pipe->writers;
}

static uint32_t endpoint_handle_drop_locked(struct kobject *object,
                                            uint32_t rights) {
  uint32_t wake = ENDPOINT_WAKE_NONE;
  if (object != NULL && object->type == OBJ_TYPE_PORT && object->port != NULL) {
    if ((rights & OS1_RIGHT_READ) != 0U && --object->port->receivers == 0) {
      for (int slot = 0; slot < PORT_TABLE_MAX; ++slot)
        if (port_table[slot] == object)
          port_table[slot] = NULL;
      wake |= ENDPOINT_WAKE_READERS | ENDPOINT_WAKE_WRITERS;
    }
    if ((rights & OS1_RIGHT_WRITE) != 0U)
      --object->port->senders;
    return wake;
  }
  if (object == NULL || object->type != OBJ_TYPE_PIPE || object->pipe == NULL)
    return wake;
  if ((rights & OS1_RIGHT_READ) != 0U && --object->pipe->readers == 0)
    wake |= ENDPOINT_WAKE_WRITERS;
  if ((rights & OS1_RIGHT_WRITE) != 0U && --object->pipe->writers == 0)
    wake |= ENDPOINT_WAKE_READERS;
  return wake;
}

static void endpoint_wake(struct kobject *object, uint32_t wake) {
  struct wait_queue_head *readers = NULL;
  struct wait_queue_head *writers = NULL;
  if (object != NULL && object->type == OBJ_TYPE_PORT && object->port != NULL) {
    readers = &object->port->rq;
    writers = &object->port->wq;
  } else if (object != NULL && object->type == OBJ_TYPE_PIPE &&
             object->pipe != NULL) {
    readers = &object->pipe->rq;
    writers = &object->pipe->wq;
  }
  if ((wake & ENDPOINT_WAKE_READERS) != 0U && readers != NULL)
    wake_up(readers);
  if ((wake & ENDPOINT_WAKE_WRITERS) != 0U && writers != NULL)
    wake_up(writers);
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
/* A published service receiver remains valid while no client is connected.
 * Sender count is therefore not an EOF condition.  Only receiver revocation
 * terminates the wait; message publication wakes the receiver normally. */
static int port_readable_block(void *arg) {
  struct kport *kp = ((struct kobject *)arg)->port;
  return kp->count == 0 && kp->receivers > 0;
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
  if (o->resource_ops && o->resource_ops->release)
    o->resource_ops->release(o->resource_state);
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
      endpoint_handle_add_locked(o, rights);
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

static int handle_release_exact(struct process *process, int handle,
                                struct kobject *expected) {
  struct kobject *to_free = NULL;
  uint32_t wake = ENDPOINT_WAKE_NONE;
  uint64_t flags;
  int status = 0;

  if (process == NULL || expected == NULL || handle < 0 ||
      handle >= NPROC_HANDLES)
    return -EINVAL;
  spin_lock_irqsave(&object_lock, &flags);
  struct handle_entry *entry = &process->handles[handle];
  if (entry->obj != expected) {
    status = -EBADF;
  } else {
    wake = endpoint_handle_drop_locked(expected, entry->rights);
    entry->obj = NULL;
    entry->rights = 0U;
    if (--expected->refcount <= 0)
      to_free = expected;
  }
  spin_unlock_irqrestore(&object_lock, flags);
  endpoint_wake(expected, wake);
  if (to_free != NULL)
    kobj_free(to_free);
  return status;
}

static void object_refresh_cached_node(struct kobject *object) {
  struct vfs_node refreshed;
  if (object == NULL)
    return;
  if (vfs_open(object->path, &refreshed) == 0)
    object->node = refreshed;
}

static long install_new_object(struct process *process, struct kobject *object,
                               uint32_t rights) {
  uint64_t flags;
  int handle;

  spin_lock_irqsave(&object_lock, &flags);
  handle = handle_install_locked(process, object, rights);
  spin_unlock_irqrestore(&object_lock, flags);
  if (handle < 0)
    kobj_free(object);
  return handle;
}

static long handle_create_resource(struct process *process, const char *path,
                                  uint32_t rights, int type) {
  if (type != OBJ_TYPE_RESOURCE) return -EINVAL;
  struct kobject *object = kobj_alloc(OBJ_TYPE_RESOURCE);
  if (!object) return -ENOMEM;
  int result = nx_resource_acquire(process, path, rights, object);
  if (result != 0) {
    kobj_free(object);
    return result;
  }
  return install_new_object(process, object, rights);
}

static long handle_create_time(struct process *process, const char *path,
                               uint32_t rights, int type) {
  const uint32_t allowed = OS1_RIGHT_READ | OS1_RIGHT_DUPLICATE |
                           OS1_RIGHT_TRANSFER;
  struct kobject *object;

  if (type != OBJ_TYPE_TIMER)
    return -EINVAL;
  if (strcmp(path, OS1_TIME_OBJECT_SYSTEM) != 0 &&
      strcmp(path, OS1_TIME_OBJECT_STATS) != 0)
    return -EINVAL;
  if ((rights & OS1_RIGHT_READ) == 0U || (rights & ~allowed) != 0U)
    return -EPERM;
  if (!proc_is_privileged(process))
    return -EPERM;
  object = kobj_alloc(OBJ_TYPE_TIMER);
  if (object == NULL)
    return -ENOMEM;
  strlcpy(object->path, path, sizeof(object->path));
  return install_new_object(process, object, rights);
}

static long fs_resolve_or_create(const char *path, uint32_t rights,
                                 int want_create, struct vfs_objref *reference) {
  int status = vfs_resolve_object(path, reference);

  if (status == -2) {
    if ((rights & OS1_RIGHT_WRITE) != 0U || want_create)
      return -EISDIR;
    status = 0;
  }
  if (status == 0)
    return 0;
  if (!want_create || (rights & OS1_RIGHT_WRITE) == 0U)
    return -ENOENT;
  status = (int)vfs_write_allowed(path);
  if (status != 0)
    return status;
  if (vfs_create(path, VFS_TYPE_FILE) != 0)
    return -EIO;
  return vfs_resolve_object(path, reference) == 0 ? 0 : -ENOENT;
}

static long fs_file_object(struct process *process, const char *path,
                           uint32_t rights,
                           const struct vfs_objref *reference) {
  struct kobject *object;
  long permission;

  if ((rights & OS1_RIGHT_WRITE) != 0U) {
    permission = vfs_write_allowed(path);
    if (permission != 0)
      return permission;
  }
  if ((rights & OS1_RIGHT_MUTATE) != 0U) {
    if (reference->node.type != VFS_TYPE_DIR)
      return -ENOTDIR;
    permission = vfs_write_allowed(path);
    if (permission != 0)
      return permission;
  }
  object = kobj_alloc(OBJ_TYPE_FILE);
  if (object == NULL)
    return -ENOMEM;
  object->node = reference->node;
  object->offset = 0U;
  strncpy(object->path, path, OBJ_FILE_PATH_MAX - 1);
  object->path[OBJ_FILE_PATH_MAX - 1] = '\0';
  return install_new_object(process, object, rights);
}

static long fs_process_object(struct process *process, uint32_t rights,
                              const struct vfs_objref *reference) {
  struct kobject *object;

  if ((rights & (OS1_RIGHT_DESTROY | OS1_RIGHT_WRITE)) != 0U &&
      !process_kill_allowed(process, reference->pid))
    return -EPERM;
  object = kobj_alloc(OBJ_TYPE_PROCESS);
  if (object == NULL)
    return -ENOMEM;
  object->pid = reference->pid;
  return install_new_object(process, object, rights);
}

static long handle_create_fs(struct process *process, const char *path,
                             uint32_t rights, int want_create) {
  char resolved[OBJ_FILE_PATH_MAX];
  struct vfs_objref reference;
  long status;

  vfs_resolve_path(path, resolved, OBJ_FILE_PATH_MAX);
  memset(&reference, 0, sizeof(reference));
  status = fs_resolve_or_create(resolved, rights, want_create, &reference);
  if (status != 0)
    return status;
  if (reference.obj_type == OBJ_TYPE_FILE)
    return fs_file_object(process, resolved, rights, &reference);
  if (reference.obj_type == OBJ_TYPE_PROCESS)
    return fs_process_object(process, rights, &reference);
  return -EINVAL;
}

static int port_free_slot_locked(void) {
  for (int slot = 0; slot < PORT_TABLE_MAX; ++slot)
    if (port_table[slot] == NULL)
      return slot;
  return -1;
}

/* Acquire authority before receiving/writing, never retrospectively.
 * Two short object_lock sections avoid a sched_lock/object_lock inversion:
 * owner/scope are snapshotted, the process tree is checked under sched_lock,
 * then both port identity and scope are revalidated before installing the
 * sender's handle. A vanished/replaced port fails closed. */
static long handle_create_port_sender(struct process *process, const char *name,
                                      uint32_t rights) {
  uint64_t flags;
  struct kobject *existing;
  int owner, scope, handle;
  spin_lock_irqsave(&object_lock, &flags);
  existing = port_find_locked(name);
  if (existing == NULL) {
    spin_unlock_irqrestore(&object_lock, flags);
    return -ENOENT;
  }
  owner = existing->port->owner_pid;
  scope = existing->port->private_port;
  spin_unlock_irqrestore(&object_lock, flags);

  if (scope == 1 && !proc_is_machine(process)) return -EPERM;
  if (scope == 2 && !proc_descends_from((int)process->pid, owner))
    return -EPERM;

  spin_lock_irqsave(&object_lock, &flags);
  existing = port_find_locked(name);
  if (existing == NULL) {
    handle = -ENOENT;
  } else if (existing->port->owner_pid != owner ||
             existing->port->private_port != scope) {
    handle = -EAGAIN;
  } else {
    handle = handle_install_locked(process, existing,
                                    rights & ~OS1_RIGHT_READ);
  }
  spin_unlock_irqrestore(&object_lock, flags);
  return handle;
}

static struct kobject *port_receiver_object(struct process *process,
                                            const char *name, int is_private) {
  /* Reject unrepresentable names before allocating the port object. */
  size_t name_len = strlen(name);
  if (name_len == 0U || name_len >= PORT_NAME_MAX)
    return NULL;
  struct kport *port = kmalloc(sizeof(*port));
  struct kobject *object;

  if (port == NULL)
    return NULL;
  memset(port, 0, sizeof(*port));
  port->owner_pid = (int)process->pid;
  port->private_port = is_private;
  for (size_t i = 0U; i < PORT_NAME_MAX; ++i) {
    if (i > name_len)
      break;
    port->name[i] = name[i];
  }
  INIT_LIST_HEAD(&port->rq.task_list);
  spin_lock_init(&port->rq.lock);
  INIT_LIST_HEAD(&port->wq.task_list);
  spin_lock_init(&port->wq.lock);
  object = kobj_alloc(OBJ_TYPE_PORT);
  if (object == NULL) {
    kfree(port);
    return NULL;
  }
  object->port = port;
  object->pid = (int)process->pid;
  return object;
}

static long handle_create_port_receiver(struct process *process,
                                        const char *name, uint32_t rights,
                                        int is_private) {
  struct kobject *object = port_receiver_object(process, name, is_private);
  uint64_t flags;
  int slot;
  int handle;

  if (object == NULL)
    return -ENOMEM;
  spin_lock_irqsave(&object_lock, &flags);
  if (port_find_locked(name) != NULL) {
    spin_unlock_irqrestore(&object_lock, flags);
    kobj_free(object);
    return -EEXIST;
  }
  slot = port_free_slot_locked();
  if (slot < 0) {
    spin_unlock_irqrestore(&object_lock, flags);
    kobj_free(object);
    return -ENOSPC;
  }
  handle = handle_install_locked(process, object, rights);
  if (handle >= 0)
    port_table[slot] = object;
  spin_unlock_irqrestore(&object_lock, flags);
  if (handle < 0)
    kobj_free(object);
  return handle;
}

/* Generic protocol namespace acquisition, independent of any service's
 * executable, PID, or protocol semantics. Opening SEND on a public port is
 * still available to USER holders; RECEIVER creation is restricted:
 *   OS1nx_machine_*  only MACHINE
 *   OS1nx_svc_*      ROOT or MACHINE
 * All other OS1nx_* names remain available to ordinary capability owners;
 * otherwise this guard would break the existing user PORT ABI.
 * Other names retain ordinary unprivileged PORT behavior.
 * This closes public service-name squatting during supervisor restarts. */
static int port_receiver_authorized(const struct process *process,
                                    const char *name) {
  if (strncmp(name, "OS1nx_machine_", sizeof("OS1nx_machine_") - 1U) == 0)
    return proc_is_machine(process);
  if (strncmp(name, "OS1nx_svc_", sizeof("OS1nx_svc_") - 1U) == 0)
    return proc_is_privileged(process);
  return 1;
}

static long handle_create_port(struct process *process, const char *name,
                               uint32_t rights, int is_private) {
  if (strlen(name) == 0U || strlen(name) >= PORT_NAME_MAX)
    return -ENAMETOOLONG;
  if ((rights & OS1_RIGHT_READ) == 0U)
    return handle_create_port_sender(process, name, rights);
  if (!port_receiver_authorized(process, name))
    return -EPERM;
  return handle_create_port_receiver(process, name, rights, is_private);
}

static long handle_create_process(struct process *process, const char *path,
                                  uint32_t rights, int type) {
  struct kobject *object;
  int pid;

  if (type != OBJ_TYPE_PROCESS)
    return -EINVAL;
  pid = atoi(path);
  if (pid <= 0)
    return -EINVAL;
  if ((rights & (OS1_RIGHT_DESTROY | OS1_RIGHT_WRITE)) != 0U &&
      !process_kill_allowed(process, pid))
    return -EPERM;
  if (process_find_by_pid(pid) == NULL)
    return -ESRCH;
  object = kobj_alloc(OBJ_TYPE_PROCESS);
  if (object == NULL)
    return -ENOMEM;
  object->pid = pid;
  return install_new_object(process, object, rights);
}

static long handle_create_registry(struct process *process, const char *path,
                                   uint32_t rights, int type) {
  struct kobject *object;

  if (type != OBJ_TYPE_REGKEY || path[0] == '\0')
    return -EINVAL;
  /* Names must be representable without truncation: a truncated authority
   * path could resolve an entirely different registry object. */
  if (strlen(path) >= MAX_KEY_LEN)
    return -ENAMETOOLONG;
  if ((rights & OS1_RIGHT_WRITE) != 0U &&
      !registry_machine_key_write_allowed(path))
    return -EACCES;
  if ((rights & OS1_RIGHT_WRITE) != 0U &&
      !registry_key_is_virtual(path) && !registry_write_allowed())
    return -EPERM;
  object = kobj_alloc(OBJ_TYPE_REGKEY);
  if (object == NULL)
    return -ENOMEM;
  strncpy(object->path, path, MAX_KEY_LEN - 1);
  object->path[MAX_KEY_LEN - 1] = '\0';
  return install_new_object(process, object, rights);
}

static long handle_create_window(struct process *process, const char *path,
                                 uint32_t rights, int type) {
  struct kobject *object;
  int window_id;
  int owner;

  if (type != OBJ_TYPE_WINDOW)
    return -EINVAL;
  window_id = atoi(path);
  if (window_id <= 0)
    return -EINVAL;
  owner = compositor_window_owner(window_id);
  if (owner < 0)
    return -ESRCH;
  if (owner != (int)process->pid &&
      (rights & (OS1_RIGHT_WRITE | OS1_RIGHT_DESTROY)) != 0U &&
      !proc_is_machine(process) && process->level > PLVL_ROOT)
    return -EPERM;
  object = kobj_alloc(OBJ_TYPE_WINDOW);
  if (object == NULL)
    return -ENOMEM;
  object->window_id = window_id;
  object->pid = owner;
  return install_new_object(process, object, rights);
}

/*
 * sys_handle_create - acquire a named object capability through one explicit
 * namespace dispatcher. Namespace helpers own all ambient acquisition policy;
 * once installed, the handle rights are the authority.
 */
long sys_handle_create(int ns, const char *upath, uint32_t rights, int type) {
  struct process *process = current_process;
  char path[OBJ_FILE_PATH_MAX];
  int want_create;
  int private_port = (rights & OS1_PORT_CREATE_PRIVATE) != 0U;
  int family_port = (rights & OS1_PORT_CREATE_FAMILY) != 0U;

  if (process == NULL)
    return -EPERM;
  if (private_port && (ns != OS1_NS_PORT ||
                       (rights & OS1_RIGHT_READ) == 0U ||
                       !proc_is_machine(process)))
    return -EPERM;
  if (family_port && (private_port || ns != OS1_NS_PORT ||
                      (rights & OS1_RIGHT_READ) == 0U))
    return -EINVAL;
  want_create = (rights & OS1_RIGHT_CREATE) != 0U;
  rights &= OS1_RIGHT_ALL;
  if (hal_copy_string_from_user(path, upath, sizeof(path)) != 0)
    return -EFAULT;
  if (handles_ensure(process) != 0)
    return -ENOMEM;

  switch (ns) {
  case OS1_NS_RESOURCE:
    return handle_create_resource(process, path, rights, type);
  case OS1_NS_TIME:
    return handle_create_time(process, path, rights, type);
  case OS1_NS_FS:
    return handle_create_fs(process, path, rights, want_create);
  case OS1_NS_PORT:
    return handle_create_port(process, path, rights,
                              private_port ? 1 : family_port ? 2 : 0);
  case OS1_NS_PROC:
    return handle_create_process(process, path, rights, type);
  case OS1_NS_REG:
    return handle_create_registry(process, path, rights, type);
  case OS1_NS_WIN:
    return handle_create_window(process, path, rights, type);
  default:
    return -EINVAL;
  }
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
  uint32_t wake = endpoint_handle_drop_locked(o, e->rights);
  e->obj = NULL;
  e->rights = 0;
  if (--o->refcount <= 0)
    to_free = o;
  spin_unlock_irqrestore(&object_lock, flags);
  endpoint_wake(o, wake);
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
 * OBJ-GRANT-REAP (RESOLVED 2026-07-23, was: "granting to a process being reaped
 * on another CPU is unsupported"): it is supported now.  The whole grant runs
 * under sched_lock, which pins the target in the process pool — a concurrent
 * reap cannot remove and free it mid-install (PROC-REF-01).  Lock order is
 * sched_lock -> object_lock; the source handle belongs to current_process,
 * which cannot be reaped underneath its own syscall.
 */
long sys_cap_grant(int target_pid, int handle, uint32_t rights) {
  struct process *cur = current_process;
  if (!cur || !cur->handles || handle < 0 || handle >= NPROC_HANDLES)
    return -EBADF;
  rights &= OS1_RIGHT_ALL;

  if (!process_ipc_allowed(cur, target_pid))
    return -EPERM;

  uint64_t sflags;
  spin_lock_irqsave(&sched_lock, &sflags);
  struct process *tgt = __process_find_by_pid(target_pid);
  int h;
  if (!tgt) {
    h = -ESRCH;
  } else if (!tgt->handles) {
    /* No table and none can be made here: handles_ensure would kmalloc, which
     * must not happen under sched_lock.  Every real IPC target already has one
     * (it took a handle to be addressable); a kernel thread that never did is
     * simply not a grant target. */
    h = -ENOMEM;
  } else {
    spin_lock(&object_lock);
    struct handle_entry *src = &cur->handles[handle];
    if (!src->obj)
      h = -EBADF;
    else if (!(src->rights & OS1_RIGHT_TRANSFER))
      h = -EPERM;
    else
      h = handle_install_locked(tgt, src->obj, rights & src->rights);
    spin_unlock(&object_lock);
  }
  spin_unlock_irqrestore(&sched_lock, sflags);
  return h;
}

/*
 * sys_pipe - OS1low_pipe(int fds[2]).  Create an anonymous byte pipe (ASTRA
 * §6.2 OBJ_TYPE_PIPE) and install BOTH ends in the caller's handle table:
 * fds[0] is the READ end, fds[1] the WRITE end (POSIX pipe() order).  One
 * kobject, two handles (rights = READ / WRITE); reader/writer counts start at
 * 1/1 via the install path.  Returns 0, or -ENOMEM/-EMFILE/-EFAULT.
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
  int wfd = (rfd >= 0)
                ? handle_install_locked(cur, o,
                                        OS1_RIGHT_WRITE | OS1_RIGHT_TRANSFER |
                                            OS1_RIGHT_DUPLICATE)
                : rfd;
  spin_unlock_irqrestore(&object_lock, flags);

  if (rfd < 0 || wfd < 0) {
    if (rfd >= 0 && handle_release_exact(cur, rfd, o) != 0)
      return -EIO;
    if (rfd < 0)
      kobj_free(o);
    return (rfd < 0) ? rfd : wfd;
  }

  int fds[2] = {rfd, wfd};
  if (hal_copy_to_user(ufds, fds, sizeof(fds)) != 0) {
    int read_release = handle_release_exact(cur, rfd, o);
    int write_release = handle_release_exact(cur, wfd, o);
    if (read_release != 0 || write_release != 0)
      return -EIO;
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
 * with it (ASTRA §6.5, Mach: "ports are first-class objects ... a port is
 * itself a capability", and a Mach message carries port RIGHTS).
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
  } else if (hal_copy_from_user(&m, umsg, sizeof(m)) != 0 ||
             (nfds > 0 && hal_copy_from_user(
                              fds, ufds, (size_t)nfds * sizeof(int)) != 0)) {
    ret = -EFAULT;
  } else {
    /* PROC-REF-01: hold sched_lock across the receiver lookup AND the whole
     * handle install, so the service cannot be reaped and freed out from under
     * us between finding it and writing into its table.  process_find_by_pid()
     * would return a pointer valid only for the instant it held the lock.  Lock
     * order sched_lock -> object_lock; the receiver's table is NOT allocated
     * here (kmalloc under sched_lock is forbidden) — a real port owner always
     * has one, having taken a handle to publish the port. */
    uint64_t sflags;
    spin_lock_irqsave(&sched_lock, &sflags);
    struct process *rcv = __process_find_by_pid(kp->owner_pid);
    if (!rcv) {
      ret = -ESRCH;
      spin_unlock_irqrestore(&sched_lock, sflags);
    } else if (!rcv->handles) {
      ret = -ENOMEM;
      spin_unlock_irqrestore(&sched_lock, sflags);
    } else {
      int installed[4];
      int n_ok = 0;
      struct kobject *to_free = NULL;
      struct kobject *wake_objects[4] = {NULL, NULL, NULL, NULL};
      uint32_t wake_flags[4] = {0U, 0U, 0U, 0U};
      int wake_count = 0;
      ret = 0;
      spin_lock(&object_lock); /* nested under sched_lock; IRQs already off */
      for (int i = 0; i < nfds; i++) {
        if (fds[i] < 0 || fds[i] >= NPROC_HANDLES || !current_process ||
            !current_process->handles ||
            !current_process->handles[fds[i]].obj) {
          ret = -EBADF;
          break;
        }
        struct handle_entry *src = &current_process->handles[fds[i]];
        if ((src->rights & OS1_RIGHT_TRANSFER) == 0U) {
          ret = -EPERM;
          break;
        }
        int h = handle_install_locked(rcv, src->obj, src->rights);
        if (h < 0) {
          ret = h;
          break;
        }
        installed[n_ok++] = h;
      }
      /* Only try to enqueue if every handle installed.  The enqueue can STILL
       * fail (receiver gone, queue full) AFTER the handles are in the
       * receiver's table — and those failures must unwind the install too.
       *
       * PORTCAP-01 (found 2026-07-23, audit programme A): the rollback used to
       * cover ONLY the install loop.  On -EPIPE / -EAGAIN the transferred
       * handles were left installed, so:
       *   - a receiver that died between the install and the enqueue kept
       *     capabilities for a message it never got (a leak that also touches
       *     PROC-REF-01: rcv may be freeing right now);
       *   - worse, -EAGAIN is the RETRYABLE path (PORT_QUEUE_MAX full), and the
       *     client retries the whole send — so a peer that merely keeps a
       *     service's port queue full makes every retry leak a fresh set of
       *     handles into the service's table until it hits NPROC_HANDLES.  A
       *     capability-table exhaustion DoS from an unprivileged sender.
       * The unwind is now driven by `ret < 0` regardless of WHERE it was set.
       */
      if (ret == 0) {
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
          kp->q_cap_count[kp->tail] = (uint32_t)nfds;
          kp->tail = (kp->tail + 1) % PORT_QUEUE_MAX;
          kp->count++;
          ret = (long)sizeof(m);
        }
      }
      if (ret < 0) {
        /* Any failure after (or during) install unwinds every handle that made
         * it in — leaving half the rights installed hands the service
         * capabilities for a request it will never see.
         *
         * The unwind DROPS the reference but never frees here: kobj_free()
         * kfree()s, and we hold sched_lock, which would create the
         * sched_lock -> kmalloc_lock order process.c says nothing establishes
         * (the same order that deadlocked dispatch_spawn against an idle core).
         * A last reference is handed to `to_free` and released after both locks
         * are dropped; at most one object can reach zero per unwind, because
         * each installed slot holds exactly one reference. */
        for (int i = 0; i < n_ok; i++) {
          struct handle_entry *e = &rcv->handles[installed[i]];
          struct kobject *dropped = e->obj;
          uint32_t wake = endpoint_handle_drop_locked(dropped, e->rights);
          if (wake != 0U && wake_count < 4) {
            wake_objects[wake_count] = dropped;
            wake_flags[wake_count] = wake;
            ++wake_count;
          }
          if (--dropped->refcount <= 0)
            to_free = dropped;
          e->obj = NULL;
          e->rights = 0;
        }
      }
      spin_unlock(&object_lock);
      spin_unlock_irqrestore(&sched_lock, sflags);
      for (int i = 0; i < wake_count; ++i)
        endpoint_wake(wake_objects[i], wake_flags[i]);
      if (to_free)
        kobj_free(to_free); /* frees (kfree) — never under sched_lock */
      if (ret > 0)
        wake_up(&kp->rq); /* outside both locks: wake_up touches runqueues */
    }
  }
  obj_unref(o);
  return ret;
}

static int object_read_directory(struct kobject *object, uint8_t *buffer,
                                 size_t size) {
  size_t window = (size_t)object->offset + size;
  char *listing;
  int total;
  int copied;

  if (window > OBJ_MAX_IO_BYTES)
    window = OBJ_MAX_IO_BYTES;
  listing = kmalloc(window);
  if (listing == NULL)
    return -ENOMEM;
  total = vfs_list_dir(object->path, listing, (uint32_t)window);
  if (total < 0) {
    kfree(listing);
    return -EIO;
  }
  if (object->offset >= (uint64_t)total) {
    kfree(listing);
    return 0;
  }
  size_t available = (size_t)total - (size_t)object->offset;
  copied = (int)(available < size ? available : size);
  memcpy(buffer, listing + object->offset, (size_t)copied);
  kfree(listing);
  return copied;
}

static long object_read_file(struct kobject *object, void *user_buffer,
                             size_t size) {
  uint8_t *buffer;
  int got;

  if (size == 0U)
    return 0;
  buffer = kmalloc(size);
  if (buffer == NULL)
    return -ENOMEM;
  object_refresh_cached_node(object);
  got = object->node.type == VFS_TYPE_DIR
      ? object_read_directory(object, buffer, size)
      : vfs_read(&object->node, object->offset, buffer, (uint32_t)size);
  if (got < 0) {
    kfree(buffer);
    return got == -ENOMEM ? -ENOMEM : -EIO;
  }
  if (got > 0 && hal_copy_to_user(user_buffer, buffer, (size_t)got) != 0) {
    kfree(buffer);
    return -EFAULT;
  }
  object->offset += (uint64_t)got;
  kfree(buffer);
  return got;
}

static long object_read_time_system(void *user_buffer, size_t size) {
  struct os1_time_snapshot_v1 snapshot;
  const size_t header_size = offsetof(struct os1_time_snapshot_v1, cpu);
  size_t copy_size = sizeof(snapshot);
  if (size < header_size)
    return -EINVAL;
  if (OS1libkernel_time_snapshot(&snapshot) != 0)
    return -EAGAIN;
  if (copy_size > size)
    copy_size = size;
  return hal_copy_to_user(user_buffer, &snapshot, copy_size) != 0
      ? -EFAULT : (long)copy_size;
}

static long object_read_time_stats(void *user_buffer, size_t size) {
  struct os1_timer_stats_snapshot_v4 snapshot;
  const size_t header_size = offsetof(struct os1_timer_stats_snapshot_v4, cpu);
  size_t copy_size = sizeof(snapshot);
  if (size < header_size)
    return -EINVAL;
  if (OS1libkernel_timer_stats_export(&snapshot) != 0)
    return -EAGAIN;
  if (copy_size > size)
    copy_size = size;
  return hal_copy_to_user(user_buffer, &snapshot, copy_size) != 0
      ? -EFAULT : (long)copy_size;
}

static long object_read_timer(const struct kobject *object, void *user_buffer,
                              size_t size) {
  if (strcmp(object->path, OS1_TIME_OBJECT_SYSTEM) == 0)
    return object_read_time_system(user_buffer, size);
  if (strcmp(object->path, OS1_TIME_OBJECT_STATS) == 0)
    return object_read_time_stats(user_buffer, size);
  return -EINVAL;
}

static long object_read_registry(const struct kobject *object,
                                 void *user_buffer, size_t size) {
  char value[MAX_VAL_LEN];
  size_t length;

  if (registry_get(object->path, value, sizeof(value)) != 0)
    return -ENOENT;
  length = strlen(value) + 1U;
  if (length > size)
    length = size;
  if (length != 0U && hal_copy_to_user(user_buffer, value, length) != 0)
    return -EFAULT;
  return (long)length;
}

static long object_read_window(const struct kobject *object, void *user_buffer,
                               size_t size) {
  struct window_info info;
  size_t copy_size;

  if (compositor_window_info(object->window_id, &info) != 0)
    return -ESRCH;
  copy_size = sizeof(info);
  if (copy_size > size)
    copy_size = size;
  if (copy_size != 0U &&
      hal_copy_to_user(user_buffer, &info, copy_size) != 0)
    return -EFAULT;
  return (long)copy_size;
}

static long object_read_console(void *user_buffer, size_t size) {
  if (size == 0U)
    return 0;
  /* The queue is statically bounded by OS1LIBKERNEL_IPC_QUEUE_MAX. Draining at most that
   * many messages prevents malformed traffic from turning one read into an
   * unbounded kernel loop. */
  for (uint32_t scan = 0U; scan < OS1LIBKERNEL_IPC_QUEUE_MAX; ++scan) {
    struct ipc_node *node = pop_message(current_process, -1);
    if (node == NULL)
      return -EAGAIN;
    if (node->msg.type == IPC_TYPE_INPUT && node->msg.data2 != 0U) {
      char character = (char)node->msg.data1;
      long result = hal_copy_to_user(user_buffer, &character, 1U) != 0
          ? -EFAULT : 1;
      kfree(node);
      return result;
    }
    kfree(node);
  }
  return -EAGAIN;
}

static long object_read_process(const struct kobject *object, void *user_buffer,
                                size_t size) {
  struct ps_info info;
  int parent = 0;
  int owner = 0;
  char status[256];
  int length;
  size_t copy_size;

  if (proc_get_lineage(object->pid, &parent, &owner) != 0) {
    parent = 0;
    owner = 0;
  }
  if (proc_get_info(object->pid, &info) != 0)
    return -ESRCH;
  length = snprintf(status, sizeof(status),
                    "pid=%d\nname=%s\nstate=%s\nprio=%d\ncpu_ms=%llu\n"
                    "oncpu=%d\nparent=%d\nowner=%d\n",
                    info.pid, info.name, proc_state_name(info.state),
                    info.priority, (unsigned long long)info.cpu_time,
                    info.on_cpu, parent, owner);
  copy_size = length > 0 ? (size_t)length : 0U;
  if (copy_size > size)
    copy_size = size;
  if (copy_size != 0U &&
      hal_copy_to_user(user_buffer, status, copy_size) != 0)
    return -EFAULT;
  return (long)copy_size;
}

/* Shared dequeue for blocking and non-blocking port consumers. The message
 * remains in a bounded kernel ring and the sender PID is kernel-stamped.
 * Returns 1/0 or -EPIPE, with exactly one locked queue transition. */
static int port_pop(struct kobject *object, struct os1_port_packet *packet) {
  struct kport *port = object->port;
  if (port == NULL)
    return -EIO;
  uint64_t flags;
  spin_lock_irqsave(&object_lock, &flags);
  if (port->count > 0U) {
    *packet = (struct os1_port_packet){
      .abi_version = OS1_PORT_PACKET_VERSION,
      .struct_size = sizeof(*packet),
      .cap_count = port->q_cap_count[port->head],
      .message = port->q[port->head]
    };
    port->head = (port->head + 1U) % PORT_QUEUE_MAX;
    --port->count;
    spin_unlock_irqrestore(&object_lock, flags);
    wake_up(&port->wq);
    return 1;
  }
  int result = port->receivers == 0 ? -EPIPE : 0;
  spin_unlock_irqrestore(&object_lock, flags);
  return result;
}

static long object_read_port(struct kobject *object, void *user_buffer,
                             size_t size) {
  struct kport *port = object->port;

  if (port == NULL)
    return -EIO;
  if (size != sizeof(struct ipc_message) &&
      size != sizeof(struct os1_port_packet)) return -EINVAL;
  /* Blocking service receive is an intentional kernel wait loop. kthread_block
   * sleeps until the queue predicate changes, so the loop does not busy-spin. */
  for (;;) {
    struct os1_port_packet packet;
    int result = port_pop(object, &packet);
    if (result < 0)
      return result;
    if (result > 0)
      return hal_copy_to_user(user_buffer,
          size == sizeof(packet) ? (const void *)&packet :
                                   (const void *)&packet.message, size) != 0
          ? -EFAULT : (long)size;
    kthread_block(&port->rq, port_readable_block, object);
  }
}

static long object_read_pipe(struct kobject *object, void *user_buffer,
                             size_t size) {
  struct kpipe *pipe = object->pipe;
  uint8_t *buffer;

  if (size == 0U)
    return 0;
  if (pipe == NULL)
    return -EIO;
  buffer = kmalloc(size);
  if (buffer == NULL)
    return -ENOMEM;
  /* Pipe read is intentionally blocking, not a busy loop; kthread_block waits
   * on a bounded kernel queue until data or EOF changes the predicate. */
  for (;;) {
    uint64_t flags;
    spin_lock_irqsave(&object_lock, &flags);
    if (pipe->count > 0U) {
      uint32_t take = pipe->count < size ? pipe->count : (uint32_t)size;
      for (uint32_t index = 0U; index < take; ++index) {
        buffer[index] = pipe->buf[pipe->head];
        pipe->head = (pipe->head + 1U) % PIPE_BUF_SIZE;
      }
      pipe->count -= take;
      spin_unlock_irqrestore(&object_lock, flags);
      wake_up(&pipe->wq);
      long result = hal_copy_to_user(user_buffer, buffer, take) != 0
          ? -EFAULT : (long)take;
      kfree(buffer);
      return result;
    }
    if (pipe->writers == 0) {
      spin_unlock_irqrestore(&object_lock, flags);
      kfree(buffer);
      return 0;
    }
    spin_unlock_irqrestore(&object_lock, flags);
    kthread_block(&pipe->rq, pipe_readable_block, object);
  }
}

static long object_read_dispatch(struct kobject *object, void *user_buffer,
                                 size_t size) {
  switch (object->type) {
  case OBJ_TYPE_RESOURCE:
    return object->resource_ops && object->resource_ops->read ?
        object->resource_ops->read(object->resource_state, user_buffer, size) :
        -EINVAL;
  case OBJ_TYPE_FILE:
    return object_read_file(object, user_buffer, size);
  case OBJ_TYPE_TIMER:
    return object_read_timer(object, user_buffer, size);
  case OBJ_TYPE_REGKEY:
    return object_read_registry(object, user_buffer, size);
  case OBJ_TYPE_WINDOW:
    return object_read_window(object, user_buffer, size);
  case OBJ_TYPE_CONSOLE:
    return object_read_console(user_buffer, size);
  case OBJ_TYPE_PROCESS:
    return object_read_process(object, user_buffer, size);
  case OBJ_TYPE_PORT:
    return object_read_port(object, user_buffer, size);
  case OBJ_TYPE_PIPE:
    return object_read_pipe(object, user_buffer, size);
  default:
    return -EINVAL;
  }
}

/* sys_object_read - pin the capability once, dispatch by object type, unpin. */
long sys_object_read(int handle, void *user_buffer, size_t size) {
  long error = 0;
  struct kobject *object;
  long result;

  if (size > OBJ_MAX_IO_BYTES)
    return -EINVAL;
  object = pin_handle(handle, OS1_RIGHT_READ, &error);
  if (object == NULL)
    return error;
  result = object_read_dispatch(object, user_buffer, size);
  obj_unref(object);
  return result;
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
  if (o->type == OBJ_TYPE_RESOURCE) {
    ret = o->resource_ops && o->resource_ops->write ?
        o->resource_ops->write(o->resource_state, ubuf, n) : -EINVAL;
  } else if (o->type == OBJ_TYPE_FILE) {
    if (n == 0) {
      ret = 0;
    } else {
      uint8_t *kb = kmalloc(n);
      if (!kb) {
        ret = -ENOMEM;
      } else if (hal_copy_from_user(kb, ubuf, n) != 0) {
        kfree(kb);
        ret = -EFAULT;
      } else {
        int wr = vfs_write_file(o->path, kb, (uint32_t)n, (uint32_t)o->offset);
        kfree(kb);
        if (wr < 0) {
          ret = -EIO;
        } else {
          o->offset += (uint64_t)wr;
          object_refresh_cached_node(o);
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
      if (hal_copy_from_user(&m, ubuf, sizeof(m)) != 0) {
        ret = -EFAULT;
      } else {
        m.from = current_process ? (int)current_process->pid : 0;
        ret = (kernel_ipc_send(o->pid, &m) == 0) ? 0 : -ESRCH;
      }
    }
  } else if (o->type == OBJ_TYPE_REGKEY) {
    /* write = registry_set the key's value (ASTRA §6.6).  Ownership stays
     * first-writer-wins (registry_set), so a granted REGKEY cannot hijack
     * another owner's key.  REG-ERRNO-01: the provider errno propagates
     * verbatim — the old `rc == -EACCES ? -EACCES : -EINVAL` collapse made
     * this door report OOM (-ENOMEM) as a bad argument while the /reg and
     * SYS_REGISTRY doors reported the true cause, the exact three-doors-
     * disagree shape R2 exists to remove. */
    if (n == 0 || n > MAX_VAL_LEN) {
      ret = -EINVAL;
    } else {
      size_t cn = (n < MAX_VAL_LEN) ? n : (MAX_VAL_LEN - 1);
      char val[MAX_VAL_LEN];
      if (hal_copy_from_user(val, ubuf, cn) != 0) {
        ret = -EFAULT;
      } else {
        val[cn] = '\0';
        int rc = registry_set(o->path, val, registry_caller_owner());
        ret = (rc == 0) ? (long)n : rc;
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
    /* Focus controls INPUT, never stdout authority. The userland nxttyd
     * owns each graphical TTY; legacy console writes with no own window or
     * inherited ctty remain diagnostic-only (UART mirror from the common
     * writer), rather than corrupting the currently focused GUI surface. */
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
      if (hal_copy_from_user(&m, ubuf, sizeof(m)) != 0) {
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
            kp->q_cap_count[kp->tail] = 0U;
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
      } else if (hal_copy_from_user(kb, ubuf, n) != 0) {
        kfree(kb);
        ret = -EFAULT;
      } else {
        size_t done = 0;
        ret = 0;
        while (done < n) {
          uint64_t f2;
          spin_lock_irqsave(&object_lock, &f2);
          if (kp->readers ==
              0) { /* no reader will ever consume → broken pipe */
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
  if (o->type == OBJ_TYPE_RESOURCE) {
    ret = o->resource_ops && o->resource_ops->wait ?
        o->resource_ops->wait(o->resource_state, arg) : -EINVAL;
  } else if (o->type == OBJ_TYPE_PROCESS) {
    /* Phase 2: `arg`, if non-NULL, is a user int* that receives the reaped
     * process's exit_code (raw). The libc personality (waitpid) wraps it in
     * the POSIX status encoding. Only meaningful on reap (ret == pid). */
    int code = 0;
    ret = process_wait(o->pid, &code);
    if (arg) {
            /* A bad status pointer is -EFAULT, not a reap that quietly delivered no
       * status.  The (void) cast here suppressed nothing and only made the
       * omission read as intentional. */
      if (hal_copy_to_user((void *)arg, &code, sizeof(code)) != 0)
        ret = -EFAULT;
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
 * DESTROY capability IS the authority — a GRANTED destroy handle lets its
 * holder kill a process it could not reach by process_kill_allowed (seL4
 * delegation of the kill right; acquisition was gated at handle_create time).
 */
long sys_object_ctl(int handle, int cmd, long arg) {
  if (cmd == OBJ_CTL_SPAWN_OWNED) {
    long error = 0;
    struct kobject *owner = pin_handle(handle, OS1_RIGHT_DESTROY, &error);
    if (owner == NULL) return error;
    long result = owner->type == OBJ_TYPE_PROCESS ?
        kernel_spawn_owned(owner->pid,
            (const struct os1_spawn_request_v1 *)(uintptr_t)arg) : -EINVAL;
    obj_unref(owner);
    return result;
  }
  /* A non-blocking read of the SAME buffered PORT queue used by blocking
   * receive. No new syscall and no polling inside the kernel. */
  if (cmd == OBJ_CTL_PORT_TRY_RECV || cmd == OBJ_CTL_PORT_TRY_RECV_PACKET) {
    long err = 0;
    struct kobject *object = pin_handle(handle, OS1_RIGHT_READ, &err);
    if (object == NULL)
      return err;
    long result = -EINVAL;
    if (object->type == OBJ_TYPE_PORT && arg != 0) {
      struct os1_port_packet packet;
      int popped = port_pop(object, &packet);
      size_t bytes = cmd == OBJ_CTL_PORT_TRY_RECV_PACKET ? sizeof(packet) :
                                                        sizeof(packet.message);
      const void *data = cmd == OBJ_CTL_PORT_TRY_RECV_PACKET ?
          (const void *)&packet : (const void *)&packet.message;
      if (popped == 1)
        result = hal_copy_to_user((void *)arg, data, bytes) != 0
                     ? -EFAULT : (long)bytes;
      else
        result = popped == 0 ? -EAGAIN : popped;
    }
    obj_unref(object);
    return result;
  }
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
      ret = process_kill_subtree(o->pid);
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
    /* NOTE(OBJ-WIN-FOCUS): FOCUS needs only READ, so a READ-only window handle
     * — which handle_create hands out for ANY window, ungated — can redirect
     * keyboard focus.  This deliberately mirrors the compositor's unprivileged
     * click-to-focus (any user may focus any window with the mouse) so the
     * keyboard `focus` path is as open as the mouse.  minimize/restore/close
     * stay window-manager authority (WRITE/DESTROY, gated at handle_create).
     * Unifying this with SYS_SET_FOCUS's stricter cross-pid rule is a
     * security-phase item. */
    uint32_t need = (cmd == OBJ_CTL_CLOSE)   ? OS1_RIGHT_DESTROY
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
       * button).  Consistent with the titlebar red button
       * (window_request_close). Previously a plain process_terminate; before
       * THAT it only destroyed the WINDOW, leaving the app headless ("close
       * does not kill").  Machine-level owners stay protected inside
       * process_terminate. */
      ret = process_kill_subtree(o->pid);
    }
    obj_unref(o);
    return ret;
  }

  /* WINDOW geometry: owner changes its logical framebuffer; the manager
   * changes only display bounds (userland nxcompositor performs scaling).
   * The manager privilege is required in addition to a WINDOW WRITE right. */
  if (cmd == OBJ_CTL_WINDOW_RESIZE || cmd == OBJ_CTL_WINDOW_PRESENT) {
    long err = 0;
    struct kobject *o = pin_handle(handle, OS1_RIGHT_WRITE, &err);
    if (!o) return err;
    long ret = -EINVAL;
    if (o->type == OBJ_TYPE_WINDOW) {
      uint64_t packed = (uint64_t)(unsigned long)arg;
      int high = (int)(int32_t)(uint32_t)(packed >> 32);
      int low = (int)(int32_t)(uint32_t)packed;
      int manager = proc_has_cap(current_process, CAP_WINDOW_MANAGE);
      if (cmd != OBJ_CTL_WINDOW_RESIZE && !manager)
        ret = -EPERM;
      else if (cmd == OBJ_CTL_WINDOW_RESIZE &&
               o->pid != (int)current_process->pid)
        ret = -EPERM;
      else if (cmd == OBJ_CTL_WINDOW_RESIZE)
        ret = compositor_resize_window(o->window_id, high, low);
      else if (cmd == OBJ_CTL_WINDOW_PRESENT)
        ret = compositor_set_window_draw_size(o->window_id, high, low);
    }
    obj_unref(o);
    return ret;
  }

  /* FILE: seek the object's shared byte offset (F4 M4.5).  Positioning needs
   * only the handle itself (any FILE capability), so write-at-offset works on a
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

  /* FILE: explicit length change through the existing WRITE capability.
   * The descriptor position is preserved, including when it lies past EOF. */
  if (cmd == OBJ_CTL_TRUNCATE) {
    long err = 0;
    struct kobject *o = pin_handle(handle, OS1_RIGHT_WRITE, &err);
    if (!o)
      return err;
    long ret;
    if (o->type != OBJ_TYPE_FILE || arg < 0) {
      ret = -EINVAL;
    } else {
      ret = vfs_truncate(o->path, (uint64_t)arg);
      if (ret == 0) object_refresh_cached_node(o);
    }
    obj_unref(o);
    return ret;
  }

  /* Namespace mutation (Programme R1b): the parent directory capability,
   * rather than an ambient path syscall, names the namespace being changed.
   * `arg` is deliberately only ONE child name: accepting slash-separated text
   * would let a caller escape the directory it holds through `..` or a nested
   * path. Recursive work stays in userland composition. */
  if (cmd == OBJ_CTL_MKDIR || cmd == OBJ_CTL_UNLINK) {
    long err = 0;
    struct kobject *o = pin_handle(handle, OS1_RIGHT_MUTATE, &err);
    if (!o)
      return err;

    long ret;
    if (o->type != OBJ_TYPE_FILE || o->node.type != VFS_TYPE_DIR) {
      ret = -ENOTDIR;
    } else if (arg == 0) {
      ret = -EFAULT;
    } else {
      char name[OBJ_FILE_PATH_MAX];
      if (hal_copy_string_from_user(name, (const char *)arg, sizeof(name)) != 0) {
        ret = -EFAULT;
      } else {
        size_t nlen = strlen(name);
        int invalid = nlen == 0 || strcmp(name, ".") == 0 ||
                      strcmp(name, "..") == 0;
        for (size_t i = 0; i < nlen; i++)
          if (name[i] == '/')
            invalid = 1;
        if (invalid) {
          ret = -EINVAL;
        } else {
          size_t plen = strlen(o->path);
          size_t sep = (plen == 1 && o->path[0] == '/') ? 0 : 1;
          if (plen + sep + nlen >= OBJ_FILE_PATH_MAX) {
            ret = -EINVAL;
          } else {
            char child[OBJ_FILE_PATH_MAX];
            memcpy(child, o->path, plen);
            if (sep)
              child[plen++] = '/';
            memcpy(child + plen, name, nlen + 1);
            ret = (cmd == OBJ_CTL_MKDIR)
                      ? vfs_create(child, VFS_TYPE_DIR)
                      : vfs_unlink(child);
          }
        }
      }
    }
    obj_unref(o);
    return ret;
  }

  /* FILE: report the CURRENT size in bytes (Stage 4: lseek SEEK_END / stat via
   * the object, so a FILE handle can fully back open()/read()/write()/lseek()).
   * Reads the live size (another handle may have grown the file), falling back
   * to the cached open-time size. */
  if (cmd == OBJ_CTL_STAT) {
    long err = 0;
    struct kobject *o = pin_handle(handle, 0, &err);
    if (!o)
      return err;
    long ret;
    if (o->type != OBJ_TYPE_FILE) {
      ret = -EINVAL;
    } else {
      /* Re-sync the cached node, do NOT run a second full path resolution.
       *
       * This used to call vfs_stat(o->path), which resolves the path from the
       * mount table all over again — while handle_create had just resolved the
       * very same path into o->node microseconds earlier.  For the size-probe
       * idiom (handle_create -> STAT -> close, which is what every fopen does)
       * that doubled the VFS work for no new information.  vfs_open refreshes
       * the node in place, the same way sys_object_read does before a read, so
       * a long-lived handle still sees a writer's appended size. */
      object_refresh_cached_node(o);
      ret = (long)o->node.size;
    }
    obj_unref(o);
    return ret;
  }

  return -EINVAL;
}

/*
 * sys_object_lseek - POSIX lseek(2) on a FILE handle: reposition the shared
 * byte offset per 'whence' and return the new absolute position.  SEEK_END
 * reads the live size (vfs_stat) so it tracks growth by another handle.  A
 * non-seekable object (CONSOLE stdin/stdout/stderr, …) returns -ESPIPE.
 * Positioning needs only the handle itself (no rights), matching OBJ_CTL_SEEK.
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
 * process_install_stdio - give a freshly created process its
 * stdin/stdout/stderr as capability handles (ASTRA §6.2: the fd table folds
 * into the object table). Handles 0/1/2 share ONE refcounted CONSOLE object — a
 * process has a single controlling terminal: handle 0 holds READ (stdin
 * keyboard drain), handles 1/2 hold WRITE (stdout/stderr → the process's own
 * window, else its ctty).  The table is allocated eagerly so every process is
 * born with the trio, replacing the old fd-table pre-open.  Returns 0, or
 * -ENOMEM.
 */
int process_install_stdio(struct process *p, struct process *parent) {
  (void)
      parent; /* currently unused; reserved for future ctty/stdio inheritance */
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
 * process_redirect_child_fd - install the SPAWNER's open handle 'parent_fd'
 * into the freshly-created child's slot 'child_slot', OVERWRITING the
 * pre-installed console there (Phase 4 shell redirection: `<`/`>`/`>>`/`2>`).
 * This is the fork+dup2 primitive expressed as a capability dup — the child
 * gets a SECOND handle to the SAME kobject (shared FILE offset, ASTRA §6.2), so
 * its read(0)/write(1) flow to the file instead of the terminal.  Called from
 * dispatch_spawn BEFORE the child is enqueued, with the spawner as
 * current_process; the spawner closes its own copies after the spawn returns.
 * Returns 0, or -EINVAL/-EBADF/-ENOMEM.
 */
int process_redirect_child_fd_from(struct process *owner, struct process *child,
                                   int child_slot, int parent_fd) {
  struct process *parent =
      owner; /* the fd's owner; not necessarily the spawner */
  if (!child || !parent)
    return -EINVAL;
  if (child_slot < 0 || child_slot >= NPROC_HANDLES || parent_fd < 0 ||
      parent_fd >= NPROC_HANDLES)
    return -EBADF;
  if (!parent->handles)
    return -EBADF;
  if (handles_ensure(child) != 0) /* normally already done by install_stdio */
    return -ENOMEM;

  struct kobject *to_free = NULL;
  uint32_t wake = ENDPOINT_WAKE_NONE;
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
  endpoint_handle_add_locked(src->obj, src->rights);
  if (old) {
    wake = endpoint_handle_drop_locked(old, old_rights);
    if (--old->refcount <= 0)
      to_free = old;
  }
  spin_unlock_irqrestore(&object_lock, flags);
  if (old != NULL)
    endpoint_wake(old, wake);
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
  struct kobject *retired[NPROC_HANDLES];
  struct kobject *wake_objects[NPROC_HANDLES];
  uint32_t wake_flags[NPROC_HANDLES];
  unsigned retired_count = 0U;
  unsigned wake_count = 0U;
  uint64_t flags;
  if (p == NULL)
    return;
  spin_lock_irqsave(&object_lock, &flags);
  struct handle_entry *tbl = p->handles;
  p->handles = NULL;
  if (tbl == NULL) {
    spin_unlock_irqrestore(&object_lock, flags);
    return;
  }
  for (int i = 0; i < NPROC_HANDLES; ++i) {
    struct kobject *object = tbl[i].obj;
    if (object == NULL)
      continue;
    uint32_t wake = endpoint_handle_drop_locked(object, tbl[i].rights);
    if (wake != 0U) {
      wake_objects[wake_count] = object;
      wake_flags[wake_count] = wake;
      ++wake_count;
    }
    tbl[i].obj = NULL;
    if (--object->refcount <= 0)
      retired[retired_count++] = object;
  }
  spin_unlock_irqrestore(&object_lock, flags);
  for (unsigned i = 0U; i < wake_count; ++i)
    endpoint_wake(wake_objects[i], wake_flags[i]);
  for (unsigned i = 0U; i < retired_count; ++i)
    kobj_free(retired[i]);
  kfree(tbl);
}
