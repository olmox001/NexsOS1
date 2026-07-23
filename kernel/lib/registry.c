/*
 * kernel/lib/registry.c
 * System Registry — hierarchical namespace TREE (Phase 4.1 A-tree-1).
 *
 * Purpose:
 *   The in-kernel COHERENT NAMESPACE: every piece of system state is a path.
 *   Dotted keys ("theme.color", "srv.notify_pid") are namespace PATHS — each
 *   segment is a node; a leaf carries a value (a "file"), an interior node is a
 *   directory.  This is the "tmpvfs"/ground-truth state services read at init,
 *   and the seam for "/reg" as a VFS file namespace (A1b) and for blocks/
 *   services/devices to attach as tree nodes (A-tree-3 mount).
 *
 * Role:
 *   Exposed to userland via sys_registry() (REG_OP_READ/WRITE/ENUM) and as
 *   capability objects (OBJ_TYPE_REGKEY, kernel/core/object.c).  Used by the
 *   desktop/UI for theme/system settings and by services for routing keys.
 *
 * Data model (replaces the old flat 128-slot array, LIB-REG-01):
 *   A tree of DYNAMICALLY allocated `struct reg_node`.  Children are kept
 * SORTED by name, so lookup is O(depth × children-scanned-with-early-stop) and
 * listing is ordered — it scales as the supervisory namespace grows, instead of
 * the old O(128) scan, and the key count is no longer capped.  kmalloc is
 * lazily initialised (safe at registry_init: PMM is up first), nodes live for
 * the kernel lifetime (registry keys are not deleted today).
 *
 * Security (LIB-REG-02): writes are first-writer-wins by owner_pid and gated by
 *   CAP_REG_WRITE (sys_registry) — a service's routing key cannot be hijacked.
 *
 * Locking: a reader/writer lock — readers (get/enum/regfs reads) run
 *   CONCURRENTLY, writers (set/del) are exclusive — so the read-heavy
 * supervisory namespace is not serialized.  IRQ-safe (sections run IRQs-off);
 * per-subtree locking is a later refinement.
 */

#include <kernel/kmalloc.h>
#include <kernel/printk.h>
#include <kernel/registry.h>
#include <kernel/sched.h> /* current_process / proc_has_cap / proc_is_machine */
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vfs.h> /* regfs: the registry mounted as a /reg file namespace */
#include <kernel/vmm.h>
#include <stdbool.h>

/* reg_node - one namespace node.  A node is a directory (has children) and/or a
 * leaf (is_leaf, carries a value).  Children live in a SORTED dynamic array of
 * pointers (by name) → O(log n) binary-search lookup and ordered listing; the
 * array grows by doubling (kmalloc, no krealloc in the kernel). */
struct reg_node {
  char name[MAX_KEY_LEN];     /* this path segment */
  char value[MAX_VAL_LEN];    /* leaf value (valid when is_leaf) */
  int owner_pid;              /* first-writer-wins owner of the leaf */
  int is_leaf;                /* 1 = carries a value */
  struct reg_node *parent;    /* owning directory (NULL for the root) */
  struct reg_node **children; /* sorted array of child pointers (by name) */
  int n_children;             /* number of children */
  int cap_children;           /* allocated capacity of children[] */
};

/* The root is the unnamed anchor; statically allocated (BSS-zeroed), its
 * children are kmalloc'd.  registry_count = number of leaves (init log). */
static struct reg_node reg_root_storage;
static struct reg_node *const reg_root = &reg_root_storage;
static int registry_count = 0;

/* Registry reader/writer lock (Phase 4.1 A-gap3): the supervisory namespace is
 * read-heavy, so readers run CONCURRENTLY and only writers are exclusive — the
 * registry is no longer serialized on every access (the old single spinlock).
 * Critical sections run with IRQs disabled (an IRQ handler never reenters
 * mid-section); registry writes never originate from IRQ context.
 *   reg_res_lock — the shared resource: held by a writer, or collectively by
 * the readers (first reader takes it, last reader releases it). reg_cnt_lock —
 * guards reg_readers across the reader lock/unlock transitions. */
static DEFINE_SPINLOCK(reg_cnt_lock);
static DEFINE_SPINLOCK(reg_res_lock);
static int reg_readers;

static void reg_read_lock(uint64_t *flags) {
  spin_lock_irqsave(&reg_cnt_lock, flags);
  if (++reg_readers == 1)
    spin_lock(&reg_res_lock); /* first reader excludes writers */
  spin_unlock(&reg_cnt_lock); /* keep IRQs off until reg_read_unlock */
}
static void reg_read_unlock(uint64_t flags) {
  spin_lock(&reg_cnt_lock); /* IRQs already disabled */
  if (--reg_readers == 0)
    spin_unlock(&reg_res_lock); /* last reader admits writers */
  spin_unlock_irqrestore(&reg_cnt_lock, flags);
}
static void reg_write_lock(uint64_t *flags) {
  spin_lock_irqsave(&reg_res_lock,
                    flags); /* exclusive: waits out readers/writers */
}
static void reg_write_unlock(uint64_t flags) {
  spin_unlock_irqrestore(&reg_res_lock, flags);
}

/* node_alloc - kmalloc + init a node for segment 'seg'.  NULL on OOM. */
static struct reg_node *node_alloc(const char *seg) {
  struct reg_node *n = kmalloc(sizeof(*n));
  if (!n)
    return NULL;
  memset(n, 0, sizeof(*n));
  strncpy(n->name, seg, MAX_KEY_LEN - 1);
  n->name[MAX_KEY_LEN - 1] = '\0';
  return n;
}

/* node_find_child - BINARY SEARCH of immediate child 'seg' in the sorted
 * children[] array (lock held).  O(log n_children). */
static struct reg_node *node_find_child(struct reg_node *p, const char *seg) {
  int lo = 0, hi = p->n_children - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    int cmp = strcmp(p->children[mid]->name, seg);
    if (cmp == 0)
      return p->children[mid];
    if (cmp < 0)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return NULL;
}

/* node_get_or_add - BINARY SEARCH child 'seg'; return it if present, else
 * insert it at the sorted position (growing children[] by doubling).  NULL on
 * OOM (lock held). */
static struct reg_node *node_get_or_add(struct reg_node *p, const char *seg) {
  /* Binary search for either the match or the insertion point 'lo'. */
  int lo = 0, hi = p->n_children - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    int cmp = strcmp(p->children[mid]->name, seg);
    if (cmp == 0)
      return p->children[mid];
    if (cmp < 0)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  /* Grow children[] if full (double, min 4; no krealloc in-kernel). */
  if (p->n_children == p->cap_children) {
    int newcap = p->cap_children ? p->cap_children * 2 : 4;
    struct reg_node **arr = kmalloc(sizeof(struct reg_node *) * (size_t)newcap);
    if (!arr)
      return NULL;
    if (p->children) {
      memcpy(arr, p->children,
             sizeof(struct reg_node *) * (size_t)p->n_children);
      kfree(p->children);
    }
    p->children = arr;
    p->cap_children = newcap;
  }
  struct reg_node *n = node_alloc(seg);
  if (!n)
    return NULL;
  n->parent = p;
  /* Insert at 'lo', shifting the larger names right (keeps the array sorted).
   */
  for (int i = p->n_children; i > lo; i--)
    p->children[i] = p->children[i - 1];
  p->children[lo] = n;
  p->n_children++;
  return n;
}

/* walk_path - resolve dotted 'key' to its node, creating the path when
 * 'create'. Returns the final node, or NULL (not found / OOM / empty key). Lock
 * held. */
static struct reg_node *walk_path(const char *key, int create) {
  struct reg_node *n = reg_root;
  const char *p = key;
  int any = 0;
  while (*p) {
    char seg[MAX_KEY_LEN];
    int i = 0;
    while (*p && *p != '.' && i < MAX_KEY_LEN - 1)
      seg[i++] = *p++;
    seg[i] = '\0';
    while (*p && *p != '.') /* drop any overflow tail of an over-long segment */
      p++;
    if (*p == '.')
      p++;
    if (seg[0] == '\0')
      continue; /* skip empty segment (leading/trailing/double dot) */
    struct reg_node *c =
        create ? node_get_or_add(n, seg) : node_find_child(n, seg);
    if (!c)
      return NULL;
    n = c;
    any = 1;
  }
  return any ? n : NULL;
}

/*
 * registry_init - seed the default entries.  reg_root is BSS-zeroed; kmalloc is
 * lazily initialised on the first node alloc (PMM is already up at this point).
 *
 * The base set of keys provides a complete namespace for the userland:
 *   - theme.color, style.name, background.name → compositor look & feel
 *   - system.hostname, system.arch, system.version, system.os → system identity
 *   - system.boot_time                           → uptime sanity (filled later)
 *   - sys.ntfy.panel_open                        → notification panel state
 *   - mouse.sensitivity                          → input driver
 *
 * Single-threaded before SMP.
 */
void registry_init(void) {
  registry_count = 0;

  /* ---- Compositor appearance (read by nxres, nxsettings, nxui, nxlauncher)
   * ---- */
  registry_set("theme.color", "dark", 0);
  registry_set("style.name", "nexs", 0);
  registry_set("background.name", "blue", 0);

  /* ---- System identity (displayed in "About", network, etc.) ---- */
  registry_set("system.hostname", "NeXs", 0);
  registry_set("system.arch", "unknown",
               0); /* will be updated by init/nxinfo */
  registry_set("system.version", "0.0.0", 0); /* kernel version placeholder */
  registry_set("system.os", "NEXS", 0);

  /* ---- Boot / runtime markers ---- */
  registry_set("system.boot_time", "0", 0); /* filled by init after spawn */

  /* ---- Notification infrastructure ---- */
  registry_set("sys.ntfy.panel_open", "0", 0);

  /* ---- Input ---- */
  registry_set("mouse.sensitivity", "1.0", 0);

  pr_info("Registry: Initialized with %d default keys (namespace tree).\n",
          registry_count);
}

/*
 * registry_set - create or update a key.  Walks/creates the path, then sets the
 * leaf value.  First-writer-wins: an existing leaf owned by someone else (and
 * owner_pid != 0) returns -EACCES.  owner_pid 0 = kernel/system (full rights).
 * Returns 0, -EACCES on ownership violation, -1 on bad args / OOM.
 */
int registry_set(const char *key, const char *value, int owner_pid) {
  if (!key || !value)
    return -1;

  uint64_t flags;
  reg_write_lock(&flags);

  struct reg_node *n = walk_path(key, 1);
  if (!n) {
    reg_write_unlock(flags);
    return -1; /* empty key or OOM */
  }
  /* Ownership ACL.  owner_pid 0 == the SYSTEM identity (machine/root, see
   * registry_caller_owner()); a non-zero owner_pid is an ordinary process
   * writing as itself.  A non-system caller may write ONLY a key it already
   * owns; everything else is denied.  That single rule covers two cases:
   *
   *   - another USER's key (n->owner_pid = some other pid) — first-writer-wins,
   *     unchanged;
   *   - a SYSTEM key (n->owner_pid == 0) — a config/service key first written
   *     by init or a root service.
   *
   * USR-SEC-01 (FIXED 2026-07-23, audit programme A): the deny used to also
   * require `n->owner_pid != 0`, which meant a system-owned key was writable by
   * ANYONE — an ordinary app could overwrite `srv.notify_pid` (redirecting every
   * notification), `theme.*`, `sys.env.*`, etc.  init re-published srv.* on each
   * respawn to paper over it (NOTIFY-REG-01), but that only bounded the window,
   * it did not close it.  The real separation of root from user here is
   * NAMESPACE OWNERSHIP, not the coarse capability mask: a user keeps
   * CAP_REG_WRITE (raptor/nxempire persist their own keys), but the keys it
   * creates are owned by its pid and it can no longer reach a system key.  A
   * system caller (owner_pid == 0) is never denied and may still override any
   * key, exactly as before. */
  if (n->is_leaf && owner_pid != 0 && n->owner_pid != owner_pid) {
    reg_write_unlock(flags);
    pr_warn("registry: PID %d denied write to '%s' (owner PID %d)\n", owner_pid,
            key, n->owner_pid);
    return -EACCES;
  }
  strncpy(n->value, value, MAX_VAL_LEN - 1);
  n->value[MAX_VAL_LEN - 1] = '\0';
  if (!n->is_leaf) {
    n->is_leaf = 1;
    n->owner_pid = owner_pid;
    registry_count++;
  }
  reg_write_unlock(flags);
  return 0;
}

/*
 * registry_get - resolve a key to its leaf and copy the value.
 * Returns 0 on success, -1 if not found / not a leaf / bad args.
 */

/*
 * VIRTUAL per-process keys (Phase 5b) — `sys.proc.<pid>.<field>`.
 *
 * These are COMPUTED from the process table on every read, never stored.  They
 * used to be written by userland (nxexec) after each spawn, which made the
 * registry a SECOND, best-effort copy of state the kernel already owns
 * authoritatively.  The proof that copy was a defect rather than a design: its
 * entries outlived their processes, so a garbage collector
 * (nxexec_prune_identities) had to be written to sweep them.
 *
 * Virtualising deletes the staleness class outright instead of maintaining it:
 * a dead pid simply has no keys, because there was never a stored node.  It
 * also avoids a new lock order — reaping stored keys at process death would
 * have nested sched_lock inside the registry locks, the same AB-BA shape the
 * roadmap already flags as an open hazard.
 *
 * `name` and lineage are FACTS THE KERNEL OWNS.  Program-level policy (an icon)
 * deliberately does NOT live here: keying a PROGRAM's property by PID is the
 * modelling error that made it go stale in the first place — it belongs under a
 * program-keyed namespace, stable across instances.
 *
 * Returns 1 if `key` was a virtual key (answer written to buf), 0 otherwise.
 */
/* reg_proc_split - parse "sys.proc.<pid>.<field>".  Returns the field (a
 * pointer into `key`) and writes the pid, or NULL if `key` is not in the
 * per-process namespace at all.  Shared by the read and write paths so they
 * cannot disagree about what counts as a virtual key. */
static const char *reg_proc_split(const char *key, int *out_pid) {
  const char *p = key;
  if (strncmp(p, "sys.proc.", 9) != 0)
    return NULL;
  p += 9;
  int pid = 0;
  if (*p < '0' || *p > '9')
    return NULL;
  while (*p >= '0' && *p <= '9')
    pid = pid * 10 + (*p++ - '0');
  if (*p != '.')
    return NULL;
  *out_pid = pid;
  return p + 1;
}

static int reg_virtual_proc(const char *key, char *buf, size_t size) {
  int pid = 0;
  const char *p = reg_proc_split(key, &pid);
  if (!p)
    return 0;

  struct ps_info pi;
  if (proc_get_info(pid, &pi) != 0) {
    buf[0] = '\0';
    return 1; /* live key space, dead process: EMPTY, never a stale value */
  }

  if (strcmp(p, "name") == 0) {
    strncpy(buf, pi.name, size - 1);
    buf[size - 1] = '\0';
    return 1;
  }
  if (strcmp(p, "state") == 0) {
    strncpy(buf, proc_state_name(pi.state), size - 1);
    buf[size - 1] = '\0';
    return 1;
  }
  if (strcmp(p, "parent") == 0 || strcmp(p, "owner") == 0) {
    int par = 0, own = 0;
    (void)proc_get_lineage(pid, &par, &own);
    snprintf(buf, size, "%d", (strcmp(p, "owner") == 0) ? own : par);
    return 1;
  }
  /* `env.<NAME>` — the per-process ENVIRONMENT (Phase 17), owned by the
   * scheduler.  An UNSET variable answers -1 ("ours, and definitively absent")
   * rather than an empty string: getenv() must be able to tell "not set" from
   * "set to the empty string", and an empty answer would collapse the two.
   * -1 also stops the lookup here instead of falling through to stored nodes,
   * so nothing can shadow the live block. */
  if (strncmp(p, "env.", 4) == 0) {
    if (!p[4])
      return -1;
    return proc_env_get(pid, p + 4, buf, size) == 0 ? 1 : -1;
  }
  return 0; /* not a field we own: fall through to stored nodes */
}

/*
 * reg_virtual_proc_write - route a WRITE/DELETE aimed at the virtual
 * per-process namespace.  Returns 1 (handled, *ret holds the result) or 0.
 *
 * Only `env.<NAME>` is writable.  The rest of the per-process view reports
 * facts the kernel derives — a write there is not "denied for now", it is
 * meaningless, so it is refused with -EACCES rather than quietly stored as a
 * node that would then be shadowed by the computed value forever.
 *
 * NOTE the authority split: this runs BEFORE the CAP_REG_WRITE gate in
 * sys_registry, because writing your own environment is ordinary unprivileged
 * work.  Requiring registry-write capability for setenv() would have set the
 * ceiling at "may reconfigure the machine" for an operation that only touches
 * the caller's own process.  proc_env_set enforces the correct rule instead:
 * self always, anyone else only if privileged.
 */
static int reg_virtual_proc_write(const char *key, const char *value,
                                  long *ret) {
  int pid = 0;
  const char *p = reg_proc_split(key, &pid);
  if (!p)
    return 0;
  if (strncmp(p, "env.", 4) == 0 && p[4]) {
    *ret = proc_env_set(current_process, pid, p + 4, value);
    return 1;
  }
  *ret = -EACCES; /* computed field: not writable by anyone */
  return 1;
}

int registry_get(const char *key, char *buffer, size_t size) {
  if (!key || !buffer || size == 0)
    return -1;

  /* Virtual keys are answered from live kernel state BEFORE consulting stored
   * nodes, so a stale leftover can never shadow the truth. */
  int v = reg_virtual_proc(key, buffer, size);
  if (v)
    return v > 0 ? 0 : -1;

  uint64_t flags;
  reg_read_lock(&flags);

  struct reg_node *n = walk_path(key, 0);
  if (!n || !n->is_leaf) {
    reg_read_unlock(flags);
    return -1;
  }
  strncpy(buffer, n->value, size - 1);
  buffer[size - 1] = '\0';
  reg_read_unlock(flags);
  return 0;
}

/* node_remove_child - unlink 'child' from parent 'p' (binary search) and free
 * it (and its now-unused children array).  Lock held; child must have no
 * children. */
static void node_remove_child(struct reg_node *p, struct reg_node *child) {
  int lo = 0, hi = p->n_children - 1, idx = -1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    int cmp = strcmp(p->children[mid]->name, child->name);
    if (cmp == 0) {
      idx = mid;
      break;
    }
    if (cmp < 0)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  if (idx < 0)
    return; /* not found (should not happen) */
  if (child->children)
    kfree(child->children);
  kfree(child);
  for (int i = idx; i < p->n_children - 1; i++)
    p->children[i] = p->children[i + 1];
  p->n_children--;
}

/*
 * registry_del - remove a key.  Clears the leaf, then prunes every now-empty
 * ancestor directory (freeing the nodes), so deleting "a.b.c" reclaims "c",
 * then "b", then "a" if they become empty.  First-writer-wins: owner_pid != 0
 * may delete only its own key.  Returns 0, -ENOENT if absent/not a leaf,
 * -EACCES on ownership violation.
 */
int registry_del(const char *key, int owner_pid) {
  if (!key)
    return -1;

  uint64_t flags;
  reg_write_lock(&flags);

  struct reg_node *n = walk_path(key, 0);
  if (!n || n == reg_root || !n->is_leaf) {
    reg_write_unlock(flags);
    return -ENOENT;
  }
  if (owner_pid != 0 && n->owner_pid != owner_pid) {
    reg_write_unlock(flags);
    return -EACCES;
  }

  n->is_leaf = 0;
  n->value[0] = '\0';
  n->owner_pid = 0;
  registry_count--;

  /* Prune empty nodes up the chain (a node with no children and no value is a
   * dead directory). */
  while (n != reg_root && n->n_children == 0 && !n->is_leaf) {
    struct reg_node *p = n->parent;
    node_remove_child(p, n);
    n = p;
  }

  reg_write_unlock(flags);
  return 0;
}

/* enum_dfs - depth-first walk emitting full dotted keys of LEAF nodes that
 * match 'prefix', in sorted order.  'path' is the shared path buffer (length
 * path_len), mutated as we descend and restored for each sibling so per-frame
 * stack stays small (depth-bounded recursion).  Lock held. */
static void enum_dfs(struct reg_node *n, char *path, size_t path_len,
                     size_t path_cap, const char *prefix, size_t prefix_len,
                     char *buf, size_t size, size_t *off) {
  for (int ci = 0; ci < n->n_children; ci++) {
    struct reg_node *c = n->children[ci];
    size_t saved = path_len;
    size_t cl = path_len;
    if (cl && cl + 1 < path_cap)
      path[cl++] = '.';
    for (size_t k = 0; c->name[k] && cl + 1 < path_cap; k++)
      path[cl++] = c->name[k];
    path[cl] = '\0';

    if (c->is_leaf &&
        (prefix_len == 0 ||
         (cl >= prefix_len && strncmp(path, prefix, prefix_len) == 0))) {
      if (*off + cl + 2 <= size) { /* key + '\n' + room for the NUL */
        memcpy(buf + *off, path, cl);
        *off += cl;
        buf[(*off)++] = '\n';
      }
    }
    enum_dfs(c, path, cl, path_cap, prefix, prefix_len, buf, size, off);
    path[saved] = '\0'; /* restore for the next sibling */
  }
}

/*
 * registry_enum - list used keys, newline-separated, into 'buf'
 * (NUL-terminated, bounded by 'size'); returns bytes written excluding the NUL,
 * -1 on bad args. Keys come out in sorted order (children are kept sorted).
 * 'prefix' (NULL/"" = all) filters to keys beginning with it — the "list a
 * namespace directory" primitive (Phase 4.1 A1a).
 */
int registry_enum(const char *prefix, char *buf, size_t size) {
  if (!buf || size == 0)
    return -1;

  /* `sys.proc.<pid>.env.` is VIRTUAL: it has no stored nodes to walk, so a
   * plain DFS would report a process's environment as empty.  Answer it from
   * the scheduler's block instead, in the same full-key form the DFS emits, so
   * callers cannot tell the two namespaces apart. */
  if (prefix) {
    int pid = 0;
    const char *f = reg_proc_split(prefix, &pid);
    if (f && strcmp(f, "env.") == 0) {
      char names[512];
      int n = proc_env_enum(pid, names, sizeof(names));
      size_t off = 0;
      /* Walk the newline-separated names in place (no strtok_r in the kernel
       * string library, and none is needed for a single scan). */
      for (char *k = names; n > 0 && *k;) {
        char *nl = k;
        while (*nl && *nl != '\n')
          nl++;
        char sep = *nl;
        *nl = '\0';
        size_t need = strlen(prefix) + strlen(k) + 2; /* separator + NUL */
        if (off + need > size)
          break;
        if (off)
          buf[off++] = '\n';
        off += (size_t)snprintf(buf + off, size - off, "%s%s", prefix, k);
        if (!sep)
          break;
        k = nl + 1;
      }
      buf[off] = '\0';
      return (int)off;
    }
  }

  size_t prefix_len = prefix ? strlen(prefix) : 0;
  char path[256];
  path[0] = '\0';

  uint64_t flags;
  reg_read_lock(&flags);
  size_t off = 0;
  enum_dfs(reg_root, path, 0, sizeof(path), prefix, prefix_len, buf, size,
           &off);
  buf[off] = '\0';
  reg_read_unlock(flags);
  return (int)off;
}

/* ------------------------------------------------------------------------- *
 * Single authority seam for BOTH registry entry points (S-ALIGN F5).
 * The registry is reachable two ways — the sys_registry op-switch (syscall 250)
 * and the /reg VFS mount below — and both mutate the same tree.  Every gate
 * decision and caller-identity derivation lives HERE, once, so the two entry
 * points can never drift apart on policy (they used to carry four independent
 * copies of these checks).
 * ------------------------------------------------------------------------- */

/* registry_write_allowed - mutating the registry needs CAP_REG_WRITE.  A NULL
 * current_process (in-kernel caller) passes: proc_has_cap(NULL) is true by
 * definition (kernel = machine identity). */
bool registry_write_allowed(void) {
  return proc_has_cap(current_process, CAP_REG_WRITE);
}

/* registry_caller_owner - first-writer-wins identity: machine AND root
 * callers write as owner 0 (full rights), everyone else as their own PID.
 *
 * Root is folded in with machine here (not just "machine/kernel" as the
 * name used to imply) because every /sys/bin system service — nxbar,
 * nxsettings, nxntfy_srv, the launcher — runs at PLVL_ROOT via the
 * per-path preset (level_for_path, syscall_dispatch.c), not PLVL_MACHINE.
 * Before this, a service's OWN keys (e.g. nxntfy_srv's sys.ntfy.log.* ring)
 * were owned by its PID; init respawning it after a crash handed the
 * replacement a FRESH pid, which then got -EACCES writing keys the dead
 * instance had created, silently freezing the ring until reboot.  Treating
 * PLVL_ROOT as system authority (interim, pending real per-namespace ACLs
 * — "system write", see the kernel/lib/registry.c file header) makes the
 * whole ROOT-level system-service tier share one first-writer-wins owner
 * and survive respawns; PLVL_USER (untrusted apps) still write as their own
 * PID and cannot hijack a service's key. */
int registry_caller_owner(void) {
  return proc_is_privileged(current_process) ? 0 : (int)current_process->pid;
}

/* ------------------------------------------------------------------------- *
 * regfs — the registry mounted as a "/reg" file namespace (Phase 4.1 A1b).
 * A synthetic VFS provider (no partition) over the registry tree: a key path
 * "/reg/system/hostname" is the registry key "system.hostname".  This realises
 * "everything is a file": registry state is read/written/listed through the
 * uniform VFS, and "/reg" is the first non-block server in the Plan 9
 * namespace.
 * ------------------------------------------------------------------------- */

/* regfs_path_to_key - mount-relative path ("/system/hostname") -> dotted key
 * ("system.hostname"): drop leading slashes, collapse each '/' run to one '.'.
 */
static void regfs_path_to_key(const char *relpath, char *key, size_t n) {
  size_t k = 0;
  const char *p = relpath;
  while (*p == '/')
    p++;
  while (*p && k + 1 < n) {
    if (*p == '/') {
      while (*p == '/')
        p++;
      if (*p && k + 1 < n)
        key[k++] = '.';
    } else {
      key[k++] = *p++;
    }
  }
  key[k] = '\0';
}

static int regfs_mount(struct vfs_mount *mnt, struct partition *p) {
  (void)mnt;
  (void)p; /* synthetic server: nothing to probe */
  return 0;
}

static int regfs_open(struct vfs_mount *mnt, const char *path,
                      struct vfs_node *out) {
  char key[MAX_KEY_LEN];
  regfs_path_to_key(path, key, sizeof(key));

  uint64_t flags;
  reg_read_lock(&flags);
  struct reg_node *n = key[0] ? walk_path(key, 0) : reg_root;
  if (!n) {
    reg_read_unlock(flags);
    return -1;
  }
  out->mnt = mnt;
  out->id = (uint64_t)(uintptr_t)n; /* nodes are never freed -> stable handle */
  out->type = n->is_leaf ? VFS_TYPE_FILE : VFS_TYPE_DIR;
  out->size = n->is_leaf ? (uint64_t)strlen(n->value) : 0;
  reg_read_unlock(flags);
  return 0;
}

static int regfs_read(struct vfs_node *node, uint64_t offset, void *buf,
                      uint32_t size) {
  struct reg_node *n = (struct reg_node *)(uintptr_t)node->id;
  if (!n)
    return -1;
  uint64_t flags;
  reg_read_lock(&flags);
  if (!n->is_leaf) {
    reg_read_unlock(flags);
    return -1;
  }
  size_t vlen = strlen(n->value);
  if (offset >= vlen) {
    reg_read_unlock(flags);
    return 0;
  }
  size_t avail = vlen - (size_t)offset;
  size_t cnt = size < avail ? size : avail;
  memcpy(buf, n->value + (size_t)offset, cnt);
  reg_read_unlock(flags);
  return (int)cnt;
}

static int regfs_write(struct vfs_mount *mnt, const char *path, uint64_t offset,
                       const void *buf, uint32_t size) {
  (void)mnt;
  /* CAP_REG_WRITE (shared seam) layers ON TOP of the CAP_FS_WRITE the
   * SYS_FILE_WRITE path already checked: VFS-write + registry-write authority.
   */
  if (!registry_write_allowed())
    return -EPERM;
  char key[MAX_KEY_LEN];
  regfs_path_to_key(path, key, sizeof(key));
  if (!key[0] || offset >= MAX_VAL_LEN - 1)
    return -1;

  size_t off = (size_t)offset;
  size_t cnt = size;
  if (off + cnt > MAX_VAL_LEN - 1)
    cnt = MAX_VAL_LEN - 1 - off;

  /* Read-modify-write so a non-zero offset OVERLAYS/extends the value instead
   * of replacing it (offset 0 stays a plain full write).  Pad a gap past the
   * end. */
  char val[MAX_VAL_LEN];
  if (registry_get(key, val, sizeof(val)) != 0)
    val[0] = '\0';
  size_t curlen = strlen(val);
  for (size_t i = curlen; i < off; i++)
    val[i] = ' ';
  memcpy(val + off, buf, cnt);
  if (off + cnt >= curlen)
    val[off + cnt] = '\0';

  return registry_set(key, val, registry_caller_owner()) == 0 ? (int)cnt : -1;
}

/* regfs_list - space-separated immediate child names of the node at 'path'
 * (the VFS directory-listing contract). */
static int regfs_list(struct vfs_mount *mnt, const char *path, char *buf,
                      uint32_t size) {
  (void)mnt;
  char key[MAX_KEY_LEN];
  regfs_path_to_key(path, key, sizeof(key));

  uint64_t flags;
  reg_read_lock(&flags);
  struct reg_node *n = key[0] ? walk_path(key, 0) : reg_root;
  if (!n) {
    reg_read_unlock(flags);
    return -1;
  }
  size_t off = 0;
  for (int i = 0; i < n->n_children; i++) {
    size_t nl = strlen(n->children[i]->name);
    if (off + nl + 2 > size)
      break;
    if (off)
      buf[off++] = ' ';
    memcpy(buf + off, n->children[i]->name, nl);
    off += nl;
  }
  buf[off] = '\0';
  reg_read_unlock(flags);
  return (int)off;
}

/* regfs_create - create an empty leaf key at 'path' (fs_ops.create).  Backs
 * handle_create(OS1_NS_FS, CREATE) on /reg so the capability route can create
 * keys exactly like regfs_write's implicit create does.  Same authority as
 * every registry write (registry_write_allowed + first-writer-wins ownership
 * inside registry_set).  Interior (directory) nodes appear implicitly when a
 * child leaf is set, so VFS_TYPE_DIR is not supported here. */
static int regfs_create(struct vfs_mount *mnt, const char *path,
                        uint32_t type) {
  (void)mnt;
  if (type != VFS_TYPE_FILE)
    return -1;
  if (!registry_write_allowed())
    return -EPERM;
  char key[MAX_KEY_LEN];
  regfs_path_to_key(path, key, sizeof(key));
  if (!key[0])
    return -1;
  return registry_set(key, "", registry_caller_owner()) == 0 ? 0 : -1;
}

/* regfs_unlink - remove the registry key at 'path' (rm /reg/...). */
static int regfs_unlink(struct vfs_mount *mnt, const char *path) {
  (void)mnt;
  if (!registry_write_allowed())
    return -EPERM;
  char key[MAX_KEY_LEN];
  regfs_path_to_key(path, key, sizeof(key));
  if (!key[0])
    return -1;
  return registry_del(key, registry_caller_owner());
}

static const struct fs_ops regfs_ops = {
    .name = "regfs",
    .mount = regfs_mount,
    .open = regfs_open,
    .read = regfs_read,
    .write = regfs_write,
    .create = regfs_create,
    .list = regfs_list,
    .unlink = regfs_unlink,
};

/* registry_mount_vfs - mount the registry as the "/reg" namespace.  Call from
 * the composition root after vfs_init() + registry_init(). */
void registry_mount_vfs(void) { vfs_mount_at("/reg", &regfs_ops, NULL); }

/*
 * sys_registry - syscall handler for registry access (syscall 250).
 *
 * Mediates userland access via secure user-space copy helpers; all pointer
 * arguments are validated through vmm before any data is consumed.
 *
 * REG_OP_ENUM: 'key' is an OPTIONAL namespace prefix (NULL = all keys).  Reads
 *   are open to everyone.
 * REG_OP_WRITE: needs CAP_REG_WRITE; carries the caller's identity (PID, or 0
 *   for machine/system) into registry_set() for first-writer-wins ownership.
 * REG_OP_READ: copies the value back (open to everyone).
 *
 * Returns 0 on success; negative errno on failure; -EINVAL on unknown op.
 */
long sys_registry(int op, const char *key, char *value, size_t size) {
  char k_key[MAX_KEY_LEN];
  char k_val[MAX_VAL_LEN];

  if (op == REG_OP_ENUM) {
    /* LIB-REG-04: enumerate keys into the user 'value' buffer.  Reads are open
     * to everyone (no capability required).  Phase 4.1 A1a: 'key' is an
     * OPTIONAL namespace prefix (NULL lists all keys — the legacy behaviour).
     */
    if (!value || size == 0)
      return -EINVAL;
    char k_prefix[MAX_KEY_LEN];
    const char *prefix = NULL;
    if (key) {
      if (vmm_copy_string_from_user(k_prefix, key, MAX_KEY_LEN) != 0)
        return -EFAULT;
      prefix = k_prefix;
    }
    size_t cap = size > 4096 ? 4096 : size;
    char *kbuf = kmalloc(cap);
    if (!kbuf)
      return -ENOMEM;
    int n = registry_enum(prefix, kbuf, cap);
    long ret;
    if (n < 0) {
      ret = -EINVAL;
    } else {
      size_t clen = (size_t)n + 1; /* include the NUL */
      if (clen > cap)
        clen = cap;
      ret = (vmm_copy_to_user(value, kbuf, clen) != 0) ? -EFAULT : (long)n;
    }
    kfree(kbuf);
    return ret;
  }

  /* 1. Copy the key from user space.  STRICT: a truncated key is a DIFFERENT
   * key, so the write would land somewhere the matching read never looks —
   * a silent miss rather than a reported failure.  Refuse instead
   * (kernel/hal_uaccess.h explains why the tolerant form still exists). */
  {
    int r = vmm_copy_string_from_user_strict(k_key, key, MAX_KEY_LEN);
    if (r == -E2BIG) {
      pr_warn("registry: key longer than %d bytes refused (not truncated)\n",
              MAX_KEY_LEN - 1);
      return -E2BIG;
    }
    if (r != 0) {
      pr_err("%s", "sys_registry: Invalid key pointer\n");
      return -EFAULT;
    }
  }

  if (op == REG_OP_WRITE) {
    /* 2. Copy the value.  STRICT for the same reason as the key, and it matters
     * most here: a truncated VALUE is reported as a successful write, so the
     * caller believes it stored something it did not.  This bit every registry
     * write over MAX_VAL_LEN, not just the environment — the environment is
     * simply the first consumer with values long enough to hit it. */
    {
      int r = vmm_copy_string_from_user_strict(k_val, value, MAX_VAL_LEN);
      if (r == -E2BIG) {
        pr_warn("registry: value for '%s' longer than %d bytes refused\n",
                k_key, MAX_VAL_LEN - 1);
        return -E2BIG;
      }
      if (r != 0) {
        pr_err("%s", "sys_registry: Invalid value pointer\n");
        return -EFAULT;
      }
    }
    /* Virtual per-process keys carry their OWN authority rule and are routed
     * before the CAP_REG_WRITE gate — see reg_virtual_proc_write. */
    long vret;
    if (reg_virtual_proc_write(k_key, k_val, &vret))
      return vret;
    /* USR-SEC-03 #79: writing the registry needs CAP_REG_WRITE (reads are
     * open to everyone). */
    if (!registry_write_allowed())
      return -EPERM;
    return registry_set(k_key, k_val, registry_caller_owner());
  } else if (op == REG_OP_READ) {
    if (registry_get(k_key, k_val, sizeof(k_val)) == 0) {
      /* 3. Copy Result to User Space securely */
      size_t copy_len = strlen(k_val) + 1;
      if (copy_len > size)
        copy_len = size;

      if (vmm_copy_to_user(value, k_val, copy_len) != 0) {
        pr_err("%s", "sys_registry: Failed to copy back to user\n");
        return -EFAULT;
      }
      return 0;
    }
    return -ENOENT;
  } else if (op == REG_OP_DEL) {
    /* Deleting a virtual env key IS unsetenv: same routing, empty value. */
    long vret;
    if (reg_virtual_proc_write(k_key, "", &vret))
      return vret;
    /* Deleting needs CAP_REG_WRITE; first-writer-wins owner is enforced in
     * registry_del (machine processes delete as owner 0 = full rights). */
    if (!registry_write_allowed())
      return -EPERM;
    return registry_del(k_key, registry_caller_owner());
  }

  return -EINVAL; /* Invalid op */
}
