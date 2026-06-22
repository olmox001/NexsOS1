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
 *   A tree of DYNAMICALLY allocated `struct reg_node`.  Children are kept SORTED
 *   by name, so lookup is O(depth × children-scanned-with-early-stop) and listing
 *   is ordered — it scales as the supervisory namespace grows, instead of the old
 *   O(128) scan, and the key count is no longer capped.  kmalloc is lazily
 *   initialised (safe at registry_init: PMM is up first), nodes live for the
 *   kernel lifetime (registry keys are not deleted today).
 *
 * Security (LIB-REG-02): writes are first-writer-wins by owner_pid and gated by
 *   CAP_REG_WRITE (sys_registry) — a service's routing key cannot be hijacked.
 *
 * Locking: one registry_lock (IRQ-safe) guards the tree; critical sections are
 *   short (tree ops, not a full scan).  Per-subtree locking is a later refinement.
 */

#include <kernel/kmalloc.h>
#include <kernel/printk.h>
#include <kernel/registry.h>
#include <kernel/sched.h> /* current_process / proc_has_cap / proc_is_machine */
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vmm.h>
#include <stdbool.h>

/* reg_node - one namespace node.  A node is a directory (has children) and/or a
 * leaf (is_leaf, carries a value).  Children live in a SORTED dynamic array of
 * pointers (by name) → O(log n) binary-search lookup and ordered listing; the
 * array grows by doubling (kmalloc, no krealloc in the kernel). */
struct reg_node {
  char name[MAX_KEY_LEN];      /* this path segment */
  char value[MAX_VAL_LEN];     /* leaf value (valid when is_leaf) */
  int owner_pid;               /* first-writer-wins owner of the leaf */
  int is_leaf;                 /* 1 = carries a value */
  struct reg_node **children;  /* sorted array of child pointers (by name) */
  int n_children;              /* number of children */
  int cap_children;            /* allocated capacity of children[] */
};

/* The root is the unnamed anchor; statically allocated (BSS-zeroed), its
 * children are kmalloc'd.  registry_count = number of leaves (init log). */
static struct reg_node reg_root_storage;
static struct reg_node *const reg_root = &reg_root_storage;
static int registry_count = 0;
static DEFINE_SPINLOCK(registry_lock);

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

/* node_get_or_add - BINARY SEARCH child 'seg'; return it if present, else insert
 * it at the sorted position (growing children[] by doubling).  NULL on OOM
 * (lock held). */
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
      memcpy(arr, p->children, sizeof(struct reg_node *) * (size_t)p->n_children);
      kfree(p->children);
    }
    p->children = arr;
    p->cap_children = newcap;
  }
  struct reg_node *n = node_alloc(seg);
  if (!n)
    return NULL;
  /* Insert at 'lo', shifting the larger names right (keeps the array sorted). */
  for (int i = p->n_children; i > lo; i--)
    p->children[i] = p->children[i - 1];
  p->children[lo] = n;
  p->n_children++;
  return n;
}

/* walk_path - resolve dotted 'key' to its node, creating the path when 'create'.
 * Returns the final node, or NULL (not found / OOM / empty key).  Lock held. */
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
 * Defaults: "theme.color"="dark", "system.hostname"="NeXs",
 * "mouse.sensitivity"="1.0".  Single-threaded before SMP.
 */
void registry_init(void) {
  registry_count = 0;
  registry_set("theme.color", "dark", 0);
  registry_set("system.hostname", "NeXs", 0);
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
  spin_lock_irqsave(&registry_lock, &flags);

  struct reg_node *n = walk_path(key, 1);
  if (!n) {
    spin_unlock_irqrestore(&registry_lock, flags);
    return -1; /* empty key or OOM */
  }
  if (n->is_leaf && owner_pid != 0 && n->owner_pid != owner_pid) {
    spin_unlock_irqrestore(&registry_lock, flags);
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
  spin_unlock_irqrestore(&registry_lock, flags);
  return 0;
}

/*
 * registry_get - resolve a key to its leaf and copy the value.
 * Returns 0 on success, -1 if not found / not a leaf / bad args.
 */
int registry_get(const char *key, char *buffer, size_t size) {
  if (!key || !buffer || size == 0)
    return -1;

  uint64_t flags;
  spin_lock_irqsave(&registry_lock, &flags);

  struct reg_node *n = walk_path(key, 0);
  if (!n || !n->is_leaf) {
    spin_unlock_irqrestore(&registry_lock, flags);
    return -1;
  }
  strncpy(buffer, n->value, size - 1);
  buffer[size - 1] = '\0';
  spin_unlock_irqrestore(&registry_lock, flags);
  return 0;
}

/* enum_dfs - depth-first walk emitting full dotted keys of LEAF nodes that match
 * 'prefix', in sorted order.  'path' is the shared path buffer (length path_len),
 * mutated as we descend and restored for each sibling so per-frame stack stays
 * small (depth-bounded recursion).  Lock held. */
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
 * registry_enum - list used keys, newline-separated, into 'buf' (NUL-terminated,
 * bounded by 'size'); returns bytes written excluding the NUL, -1 on bad args.
 * Keys come out in sorted order (children are kept sorted).  'prefix' (NULL/"" =
 * all) filters to keys beginning with it — the "list a namespace directory"
 * primitive (Phase 4.1 A1a).
 */
int registry_enum(const char *prefix, char *buf, size_t size) {
  if (!buf || size == 0)
    return -1;

  size_t prefix_len = prefix ? strlen(prefix) : 0;
  char path[256];
  path[0] = '\0';

  uint64_t flags;
  spin_lock_irqsave(&registry_lock, &flags);
  size_t off = 0;
  enum_dfs(reg_root, path, 0, sizeof(path), prefix, prefix_len, buf, size, &off);
  buf[off] = '\0';
  spin_unlock_irqrestore(&registry_lock, flags);
  return (int)off;
}

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
     * to everyone (no capability required).  Phase 4.1 A1a: 'key' is an OPTIONAL
     * namespace prefix (NULL lists all keys — the legacy behaviour). */
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

  /* 1. Copy Key from User Space securely (stops at null!) */
  if (vmm_copy_string_from_user(k_key, key, MAX_KEY_LEN) != 0) {
    pr_err("%s", "sys_registry: Invalid key pointer\n");
    return -EFAULT;
  }

  if (op == REG_OP_WRITE) {
    /* USR-SEC-03 #79: writing the registry needs CAP_REG_WRITE (reads are
     * open to everyone). */
    if (!proc_has_cap(current_process, CAP_REG_WRITE))
      return -EPERM;
    /* 2. Copy Value from User Space securely (stops at null!) */
    if (vmm_copy_string_from_user(k_val, value, MAX_VAL_LEN) != 0) {
      pr_err("%s", "sys_registry: Invalid value pointer\n");
      return -EFAULT;
    }
    /* Caller identity for the ownership check (LIB-REG-02/USR-SEC-01):
     * machine-level processes write as owner 0 (full rights, e.g. init
     * seeding defaults); everyone else writes as their own PID. */
    int owner = !proc_is_machine(current_process) ? (int)current_process->pid : 0;
    return registry_set(k_key, k_val, owner);
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
  }

  return -EINVAL; /* Invalid op */
}
