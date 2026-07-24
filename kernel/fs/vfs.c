/*
 * kernel/fs/vfs.c
 * Virtual Filesystem Switch — provider registry, mount table, dispatch.
 *
 * Purpose (VFS-01 resolved):
 *   The kernel-core side of the filesystem provider contract defined in
 *   <kernel/vfs.h>.  Holds the registered fs drivers and the mount table,
 *   probes partitions at vfs_init(), and dispatches the vfs_* API through
 *   the mounted provider's fs_ops.  Also provides vfs_resolve_path(), the
 *   path canonicalisation helper used by the syscall layer.
 *
 * Role in the stack (ASTRA, docs/ASTRA.md):
 *   syscall_dispatch.c / elf.c  →  vfs_* (this file)  →  fs_ops provider
 *   (ext4 today).  No caller outside kernel/fs/ touches ext4_* anymore.
 *
 * Resolution model:
 *   Single root mount ("/"): the first registered provider that successfully
 *   mounts a partition (probed in partition-table order) becomes the root.
 *   Multiple mounts/mountpoints are a later step (the table is already
 *   sized for them).
 *
 * Key invariants:
 *   - vfs_resolve_path output is always NUL-terminated.
 *   - Normalisation is done entirely in-place on a stack copy; the original
 *     'in' string is never modified.
 *   - parts[] stores interior pointers into temp[]; they remain valid for the
 *     lifetime of the normalisation loop because temp[] is on the same frame.
 *
 * Known issues:
 *   VFS-02  RESOLVED (Phase B2): vfs_resolve_path() guards current_process;
 *           a relative path resolved from kernel context (no process) is now
 *           treated as relative to "/".
 *   VFS-03  (W2 BAD-IMPL) parts[32] limits the resolved path to 32 components;
 *           deeper paths are silently truncated with no error returned.
 *   VFS-04  (W1 REFINE) temp[256] and normalized[256] are both stack-allocated.
 *           A CWD of 127 bytes + in of 128 bytes = 255 chars before the NUL;
 *           strncat guards the overflow but silently truncates the result.
 */
#include <kernel/types.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/gpt.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>

/* Registered filesystem drivers (providers).  Registration happens at boot,
 * single-threaded, before vfs_init(); no locking needed. */
#define VFS_MAX_FS 4
static const struct fs_ops *fs_drivers[VFS_MAX_FS];
static int fs_driver_count;

/* Mount table.  mounts[0] is the root mount (established once at vfs_init()
 * and never torn down); further slots take vfs_mount_at() mounts, which CAN
 * be retired by vfs_umount().  mounts_lock guards slot claim/retire and the
 * resolve+active_ops increment, so a dispatch can never run on a mount being
 * torn down (see vfs_acquire/vfs_release). */
#define VFS_MAX_MOUNTS 4
static struct vfs_mount mounts[VFS_MAX_MOUNTS];
static DEFINE_SPINLOCK(mounts_lock);

/*
 * vfs_register_fs - register a filesystem provider.
 * Returns 0, or -1 if the driver table is full or ops is malformed.
 */
int vfs_register_fs(const struct fs_ops *ops) {
  if (!ops || !ops->mount || !ops->open || !ops->read) {
    pr_err("%s", "VFS: rejecting malformed fs_ops registration\n");
    return -1;
  }
  if (fs_driver_count >= VFS_MAX_FS) {
    pr_err("%s", "VFS: fs driver table full\n");
    return -1;
  }
  fs_drivers[fs_driver_count++] = ops;
  return 0;
}

/*
 * vfs_init - probe partitions and establish the root mount.
 *
 * For each partition (table order), each registered provider gets a chance
 * to mount it.  The first success becomes the root.  Providers must probe
 * quietly (a partition that is simply not their format is normal) and fail
 * loudly only on recognised-but-unsupported filesystems.
 */
/* __vfs_try_mount_root - try every FS provider on `p` as the root mount.
 * Returns 1 if one took it.  `idx` is only for the log line. */
static int __vfs_try_mount_root(struct partition *p, int idx) {
  if (!p)
    return 0;
  for (int d = 0; d < fs_driver_count; d++) {
    struct vfs_mount *mnt = &mounts[0];
    mnt->ops = fs_drivers[d];
    mnt->part_index = (uint32_t)idx;
    mnt->fs_private = NULL;
    mnt->mountpoint = "/"; /* the root mount (Plan 9 namespace fallback) */
    if (fs_drivers[d]->mount(mnt, p) == 0) {
      mnt->in_use = 1;
      pr_info("VFS: mounted %s on partition %d as /\n", fs_drivers[d]->name,
              idx);
      return 1;
    }
    mnt->ops = NULL;
  }
  return 0;
}

/*
 * vfs_init - establish the root mount.
 *
 * BY ROLE first (F3): the partition whose type GUID says NEXS_ROLE_ROOT becomes
 * "/", whatever its position in the table.  The old behaviour — mount the first
 * partition any provider accepts — was safe only while exactly one filesystem
 * existed on the disk.  It is the mechanism behind GPT-02's warning that
 * "changing the disk image layout will silently mount the wrong partition", and
 * it would break the moment the installer adds a second ext4 (MACHINE, USR):
 * whichever was probed first would silently become the system root.
 *
 * The scan is KEPT as a fallback, and that is deliberate rather than defensive:
 * images that predate roles must still boot, and so must the release ISO, whose
 * rootfs arrives as a RAM disk built by the same mkdisk.  Falling back is
 * announced in the log so an unroled disk is visible, not silent.
 */
void vfs_init(void) {
  if (fs_driver_count == 0) {
    pr_err("%s", "VFS: no filesystem providers registered\n");
    return;
  }

  struct partition *root = partition_find_by_role(NEXS_ROLE_ROOT);
  if (root) {
    if (__vfs_try_mount_root(root, (int)root->index))
      return;
    /* A partition that DECLARES it is the root but carries no mountable
     * filesystem is a corrupt install, not a reason to go looking elsewhere:
     * mounting some other partition as "/" would hide the damage and run the
     * system on the wrong tree. */
    pr_err("%s", "VFS: NEXS-ROOT partition has no mountable filesystem\n");
    return;
  }

  pr_info("%s", "VFS: no NEXS-ROOT partition; falling back to first mountable "
                "(pre-role image)\n");
  for (int i = 0; i < num_partitions; i++) {
    if (__vfs_try_mount_root(gpt_get_partition(i), i))
      return;
  }
  pr_err("%s", "VFS: no mountable filesystem found on any partition\n");
}

/*
 * vfs_mount_at - mount a provider at an absolute path (Plan 9 namespace mount).
 * mounts[0] is the root; further slots take synthetic/extra mounts.  Stateless
 * synthetic servers (e.g. regfs) pass fs_private = NULL and need no ops->mount.
 */
int vfs_mount_at(const char *mountpoint, const struct fs_ops *ops,
                 void *fs_private) {
  if (!mountpoint || !ops || !ops->open) {
    pr_err("%s", "VFS: rejecting malformed vfs_mount_at\n");
    return -1;
  }
  uint64_t flags;
  spin_lock_irqsave(&mounts_lock, &flags);
  for (int i = 1; i < VFS_MAX_MOUNTS; i++) { /* slot 0 is the root */
    if (mounts[i].in_use)
      continue;
    mounts[i].ops = ops;
    mounts[i].fs_private = fs_private;
    mounts[i].mountpoint = mountpoint;
    mounts[i].part_index = 0;
    mounts[i].active_ops = 0;
    mounts[i].in_use = 1;
    spin_unlock_irqrestore(&mounts_lock, flags);
    pr_info("VFS: mounted %s at %s\n", ops->name, mountpoint);
    return 0;
  }
  spin_unlock_irqrestore(&mounts_lock, flags);
  pr_err("%s", "VFS: mount table full\n");
  return -1;
}

/*
 * vfs_umount - retire a NON-root mount by exact mountpoint match.
 *
 * Under mounts_lock: refuse while path operations are in flight (-EBUSY —
 * vfs_acquire holds the same lock to resolve+count, so no new operation can
 * slip in), then retire the slot.  The provider's optional umount op runs
 * AFTER the unlock on a stack copy of the retired mount: no new dispatch can
 * reach it, and provider teardown (kfree etc.) never runs under a spinlock.
 * Root ("/") teardown is deliberately refused until the ISO-boot rework
 * (docs/userland-port PLAN phase 4).
 */
int vfs_umount(const char *mountpoint) {
  if (!mountpoint || strcmp(mountpoint, "/") == 0)
    return -1;
  uint64_t flags;
  spin_lock_irqsave(&mounts_lock, &flags);
  for (int i = 1; i < VFS_MAX_MOUNTS; i++) { /* slot 0 is the root */
    if (!mounts[i].in_use || !mounts[i].mountpoint ||
        strcmp(mounts[i].mountpoint, mountpoint) != 0)
      continue;
    if (mounts[i].active_ops != 0) {
      spin_unlock_irqrestore(&mounts_lock, flags);
      return -EBUSY;
    }
    struct vfs_mount retired = mounts[i];
    mounts[i].in_use = 0;
    mounts[i].ops = NULL;
    mounts[i].fs_private = NULL;
    mounts[i].mountpoint = NULL;
    spin_unlock_irqrestore(&mounts_lock, flags);
    if (retired.ops->umount)
      retired.ops->umount(&retired);
    pr_info("VFS: unmounted %s from %s\n", retired.ops->name, mountpoint);
    return 0;
  }
  spin_unlock_irqrestore(&mounts_lock, flags);
  return -1;
}

/*
 * vfs_resolve - find the mount responsible for 'path' (LONGEST matching
 * mountpoint; the root "/" is the fallback) and hand back the path relative to
 * that mount (always with a leading '/'; the root mount passes the path through
 * unchanged so existing providers see absolute paths exactly as before).
 * Matching is component-aware: "/reg" matches "/reg" and "/reg/x", not "/regfoo".
 */
static struct vfs_mount *vfs_resolve(const char *path, const char **rel) {
  struct vfs_mount *best = NULL;
  size_t best_len = 0;
  for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
    if (!mounts[i].in_use || !mounts[i].mountpoint)
      continue;
    const char *mp = mounts[i].mountpoint;
    size_t mlen = strlen(mp);
    int match;
    if (mlen == 1 && mp[0] == '/') {
      match = 1; /* root matches everything */
    } else {
      match = (strncmp(path, mp, mlen) == 0 &&
               (path[mlen] == '/' || path[mlen] == '\0'));
    }
    if (match && (best == NULL || mlen >= best_len)) {
      best = &mounts[i];
      best_len = mlen;
    }
  }
  if (!best) {
    *rel = path;
    return NULL;
  }
  if (best_len <= 1) {
    *rel = path; /* root "/": providers keep seeing the full absolute path */
  } else {
    *rel = path + best_len;   /* "/reg/x" -> "/x"; "/reg" -> "" */
    if (**rel == '\0')
      *rel = "/";
  }
  return best;
}

/*
 * vfs_acquire / vfs_release - resolve + pin a mount for one path operation.
 * The resolve and the active_ops increment happen under mounts_lock, so
 * vfs_umount (same lock) can never observe active_ops == 0 while a dispatch
 * is between resolve and provider call.  Release is a plain atomic decrement:
 * umount only needs the count to eventually reach zero, and a torn-down slot
 * is never handed out again while pinned.
 */
static struct vfs_mount *vfs_acquire(const char *path, const char **rel) {
  uint64_t flags;
  spin_lock_irqsave(&mounts_lock, &flags);
  struct vfs_mount *mnt = vfs_resolve(path, rel);
  if (mnt)
    __sync_fetch_and_add(&mnt->active_ops, 1);
  spin_unlock_irqrestore(&mounts_lock, flags);
  return mnt;
}

static void vfs_release(struct vfs_mount *mnt) {
  if (mnt)
    __sync_fetch_and_sub(&mnt->active_ops, 1);
}

/*
 * vfs_open - resolve an absolute normalized path to a vfs_node.
 * Returns 0 on success, negative on error/not-found.
 */
int vfs_open(const char *path, struct vfs_node *out) {
  if (!path || !out)
    return -1;
  const char *rel;
  struct vfs_mount *mnt = vfs_acquire(path, &rel);
  if (!mnt)
    return -1;
  int rc = mnt->ops->open ? mnt->ops->open(mnt, rel, out) : -1;
  vfs_release(mnt);
  return rc;
}

/*
 * vfs_resolve_object - resolve a path to the TYPED object it names.  A provider
 * with an object_at op maps the path to its native object type (e.g. /proc/<pid>
 * -> OBJ_TYPE_PROCESS); a provider without one (ext4, regfs) yields a plain FILE
 * object via open().  This is the namespace→object bridge behind open() ≡
 * handle_create(OS1_NS_FS): everything resolvable in the namespace is an object.
 * Returns 0 (out filled), -1 not found, -2 the path is a directory.
 */
int vfs_resolve_object(const char *path, struct vfs_objref *out) {
  if (!path || !out)
    return -1;
  const char *rel;
  struct vfs_mount *mnt = vfs_acquire(path, &rel);
  if (!mnt || !mnt->ops) {
    vfs_release(mnt);
    return -1;
  }
  int rc;
  if (mnt->ops->object_at) {
    rc = mnt->ops->object_at(mnt, rel, out);
  } else if (!mnt->ops->open) {
    rc = -1;
  } else {
    /* Default: the provider's paths are plain FILE objects. */
    struct vfs_node node;
    if (mnt->ops->open(mnt, rel, &node) != 0) {
      rc = -1;
    } else {
      out->obj_type = OBJ_TYPE_FILE;
      out->node = node;
      rc = (node.type == VFS_TYPE_DIR) ? -2 : 0;
    }
  }
  vfs_release(mnt);
  return rc;
}

/*
 * vfs_read - random-access read from an open node.
 * Returns bytes read (clamped at EOF) or negative on error.
 */
int vfs_read(struct vfs_node *node, uint64_t offset, void *buf,
             uint32_t size) {
  if (!node || !node->mnt || !node->mnt->ops->read)
    return -1;
  return node->mnt->ops->read(node, offset, buf, size);
}

/*
 * vfs_read_file - path-based convenience read.
 * buf==NULL or size==0 returns the file size without reading (userland ABI
 * of SYS_FILE_READ — see user/sys/lib/lib.c file_read).
 */
int vfs_read_file(const char *path, void *buf, uint32_t size,
                  uint64_t offset) {
  struct vfs_node node;
  if (vfs_open(path, &node) != 0)
    return -1;
  if (buf == NULL || size == 0)
    return (int)node.size;
  return vfs_read(&node, offset, buf, size);
}

/*
 * vfs_write_allowed - THE single write-authority seam for every VFS
 * write-class entry point (S-ALIGN F6): SYS_FILE_WRITE, SYS_UNLINK, and
 * open-for-write/handle acquisition (SYS_OPEN → sys_handle_create OS1_NS_FS).
 * Policy lives only here so the entry points cannot drift:
 *   - mutating the filesystem needs CAP_FS_WRITE (USR-SEC-03 #79);
 *   - the binary trees /sys,/bin are immutable for non-machine callers
 *     (EXT4-02/ABI-04: services + init chain cannot be overwritten).
 * 'resolved_path' must already be vfs_resolve_path()-canonical.  A NULL
 * current_process (in-kernel caller) passes: kernel = machine identity.
 * Use-time writes on an already-held FILE handle are deliberately NOT
 * re-checked (seL4/Mach delegation: the handle's rights are the authority).
 * Returns 0, -EPERM (no capability) or -EACCES (protected tree).
 */
int vfs_write_allowed(const char *resolved_path) {
  if (!proc_has_cap(current_process, CAP_FS_WRITE))
    return -EPERM;
  if (proc_is_machine(current_process))
    return 0; /* machine identity: full filesystem authority */

  /* Tree ACL (maintainer policy, 2026-07-23 — the LEVEL model's filesystem
   * half; the capability mask is the other half):
   *   machine           full authority (handled above).
   *   /home             every CAP_FS_WRITE holder writes here — except GUEST,
   *                     confined to /home/shared.  (Per-user partitions
   *                     /mnt/usrN/home are the forward work, B2.3.)
   *   /sys/bin,/system  MACHINE only — even ROOT is refused.  /sys/bin is the
   *                     supervised boot chain (init/services must not be
   *                     swappable out from under init); /system is the machine
   *                     configuration tree.  These two are the "root has full
   *                     access EXCEPT these" the maintainer named.
   *   /sys,/bin,else    ROOT or machine; closed to ordinary users.
   * Exact-match guards cover the directory nodes themselves (unlink/create
   * of "/bin" etc.), not just children. */
  if (strncmp(resolved_path, "/home/", 6) == 0 ||
      strcmp(resolved_path, "/home") == 0) {
    if (current_process && current_process->level >= PLVL_GUEST &&
        !(strncmp(resolved_path, "/home/shared/", 13) == 0 ||
          strcmp(resolved_path, "/home/shared") == 0)) {
      pr_warn("vfs: guest PID %d denied write outside /home/shared ('%s')\n",
              current_process->pid, resolved_path);
      return -EACCES;
    }
    return 0;
  }
  /* B2.2: /system joins /sys/bin as MACHINE-only — root is refused here, which
   * is the "root full access EXCEPT /sys/bin and /system" rule.  Guarding the
   * path now (before /system is populated — B2.3 owns its creation/layout) is
   * correct and harmless: nothing writes it today, and the ACL must not depend
   * on the tree already existing. */
  if (strncmp(resolved_path, "/sys/bin/", 9) == 0 ||
      strcmp(resolved_path, "/sys/bin") == 0 ||
      strncmp(resolved_path, "/system/", 8) == 0 ||
      strcmp(resolved_path, "/system") == 0) {
    pr_warn("vfs: PID %d denied write to machine-only path '%s'\n",
            current_process ? current_process->pid : 0, resolved_path);
    return -EACCES;
  }
  /* Explicit system trees (maintainer-requested branch): /bin and /sys
   * (incl. /sys/lib) are the named root-only trees. */
  if (strncmp(resolved_path, "/sys/", 5) == 0 ||
      strcmp(resolved_path, "/sys") == 0 ||
      strncmp(resolved_path, "/bin/", 5) == 0 ||
      strcmp(resolved_path, "/bin") == 0) {
    if (!proc_is_privileged(current_process)) {
      pr_warn("vfs: PID %d denied write to root-only path '%s'\n",
              current_process ? current_process->pid : 0, resolved_path);
      return -EACCES;
    }
    return 0;
  }
  /* Every remaining tree outside /home (/etc, /fonts, /lib, "/", ...) is
   * equally closed to ordinary users: only /home has expanded authority. */
  if (!proc_is_privileged(current_process)) {
    pr_warn("vfs: PID %d denied write outside /home ('%s')\n",
            current_process ? current_process->pid : 0, resolved_path);
    return -EACCES;
  }
  return 0;
}

/*
 * vfs_write_file - path-based write through the provider.
 * Returns bytes written or negative; -1 if the provider has no write support.
 */
int vfs_write_file(const char *path, const void *buf, uint32_t size,
                   uint64_t offset) {
  const char *rel;
  struct vfs_mount *mnt = vfs_acquire(path, &rel);
  if (!mnt)
    return -1;
  int rc = mnt->ops->write ? mnt->ops->write(mnt, rel, offset, buf, size) : -1;
  vfs_release(mnt);
  return rc;
}

/*
 * vfs_list_dir - list a directory through the provider.
 * Returns the formatted length, -1 not found, -2 not a directory.
 */
/* mount_child_of - if mount path 'mp' sits DIRECTLY inside directory 'dir', copy
 * its leaf name into 'leaf' and return 1; else 0.  Makes a mount visible in its
 * parent's listing (Plan 9: "ls /" shows "reg" for the /reg mount). */
static int mount_child_of(const char *mp, const char *dir, char *leaf,
                          size_t leafsize) {
  const char *last = mp;
  for (const char *p = mp; *p; p++)
    if (*p == '/')
      last = p;
  if (last == mp) { /* mp like "/reg": parent is "/" */
    if (strcmp(dir, "/") != 0)
      return 0;
  } else {
    size_t plen = (size_t)(last - mp);
    char parent[128];
    if (plen >= sizeof(parent))
      plen = sizeof(parent) - 1;
    memcpy(parent, mp, plen);
    parent[plen] = '\0';
    if (strcmp(parent, dir) != 0)
      return 0;
  }
  const char *name = last + 1;
  if (!*name)
    return 0;
  strncpy(leaf, name, leafsize - 1);
  leaf[leafsize - 1] = '\0';
  return 1;
}

int vfs_list_dir(const char *path, char *buf, uint32_t size) {
  const char *rel;
  struct vfs_mount *mnt = vfs_acquire(path, &rel);
  if (!mnt)
    return -1;
  if (!mnt->ops->list) {
    vfs_release(mnt);
    return -1;
  }
  int res = mnt->ops->list(mnt, rel, buf, size);
  vfs_release(mnt);
  if (res < 0)
    return res;

  /* Append any mounts that live directly under 'path' so they show up in the
   * listing (a synthetic mount like /reg is not in its parent provider's dir).
   * Under mounts_lock: slots can now be retired by vfs_umount, so the
   * in_use/mountpoint pair must be read atomically with respect to it. */
  size_t off = (size_t)res;
  uint64_t lflags;
  spin_lock_irqsave(&mounts_lock, &lflags);
  for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
    if (!mounts[i].in_use || !mounts[i].mountpoint || &mounts[i] == mnt)
      continue;
    char leaf[64];
    if (!mount_child_of(mounts[i].mountpoint, path, leaf, sizeof(leaf)))
      continue;
    size_t ll = strlen(leaf);
    if (off + ll + 2 >= size)
      break;
    if (off && buf[off - 1] != ' ')
      buf[off++] = ' ';
    memcpy(buf + off, leaf, ll);
    off += ll;
    buf[off++] = ' ';
    buf[off] = '\0';
  }
  spin_unlock_irqrestore(&mounts_lock, lflags);
  return (int)off;
}

/*
 * vfs_unlink - remove the file/node at 'path' through the responsible mount.
 * Returns 0, or negative (provider has no unlink, or its own error).
 */
int vfs_unlink(const char *path) {
  const char *rel;
  struct vfs_mount *mnt = vfs_acquire(path, &rel);
  if (!mnt)
    return -1;
  int rc = mnt->ops->unlink ? mnt->ops->unlink(mnt, rel) : -1;
  vfs_release(mnt);
  return rc;
}

/*
 * vfs_create - create an empty file/dir at 'path' through the responsible
 * mount's provider (issue #126 / NOTE(M4.5-FS-WRITE)).
 * Returns 0, or negative (provider has no create support, or its own error).
 */
int vfs_create(const char *path, uint32_t type) {
  const char *rel;
  struct vfs_mount *mnt = vfs_acquire(path, &rel);
  if (!mnt)
    return -1;
  int rc = mnt->ops->create ? mnt->ops->create(mnt, rel, type) : -1;
  vfs_release(mnt);
  return rc;
}

/*
 * vfs_stat - fill st with size/type for path.  0 on success, -1 otherwise.
 */
int vfs_stat(const char *path, struct vfs_stat *st) {
  struct vfs_node node;
  if (vfs_open(path, &node) != 0)
    return -1;
  if (st) {
    st->size = node.size;
    st->type = node.type;
  }
  return 0;
}

/*
 * vfs_resolve_path - canonicalise a raw user path into an absolute path string.
 *
 * @in:   input path (absolute or relative; not modified).
 * @out:  output buffer; receives the NUL-terminated absolute path.
 * @size: capacity of out[], including the NUL terminator.
 *
 * Preconditions:
 *   - out != NULL, size >= 1.
 *   - If in is relative and a current process exists, its cwd must be a
 *     valid NUL-terminated string.  Without a current process (kernel
 *     context) relative paths resolve from "/" (FIX VFS-02).
 *
 * Algorithm:
 *   1. Build temp[] = (absolute ? in : cwd + "/" + in), truncated to 255 chars.
 *   2. Walk temp[] splitting on '/', accumulating component pointers in
 *      parts[0..31].  ".." decrements part_count; "." is skipped.
 *      NOTE(VFS-03): parts[] has only 32 slots; the 33rd and later components
 *      are silently dropped — no error is returned to the caller.
 *   3. Reassemble parts[] into normalized[] with leading '/'.
 *   4. strncpy into out[]; force-NUL-terminate at out[size-1].
 *      NOTE(VFS-04): Both temp[] and normalized[] are 256-byte stack buffers.
 *      A CWD near 128 bytes joined with 'in' near 128 bytes will be silently
 *      truncated by strncat before the normalisation loop starts.
 *
 * Side effects: none (no disk I/O, no locks).
 *
 * Returns: void; result is written to out[].
 *
 * Path Resolution:
 * - If path starts with '/', it's absolute.
 * - Otherwise, it's relative to current_process->cwd.
 * - Normalizes . and ..
 */
void vfs_resolve_path(const char *in, char *out, size_t size) {
    /* temp[]: working copy of the unsplit path; max 255 chars + NUL.
     * NOTE(VFS-04): 256-byte stack buffer; CWD + "/" + in > 255 chars
     * will be silently truncated by strncat (the -1 guard below). */
    char temp[256];
    if (in[0] == '/') {
        /* Absolute path: copy verbatim into temp; strncpy NUL-pads. */
        strncpy(temp, in, sizeof(temp));
    } else {
        /* Relative path: prepend current working directory.
         * FIX(VFS-02): kernel-context callers (boot, no current process) have
         * no cwd — resolve relative to "/" instead of dereferencing NULL. */
        if (current_process) {
            strncpy(temp, current_process->cwd, sizeof(temp));
        } else {
            strncpy(temp, "/", sizeof(temp));
        }
        size_t len = strlen(temp);
        /* Ensure exactly one '/' separator between CWD and 'in'. */
        if (len > 0 && temp[len-1] != '/') {
            strncat(temp, "/", sizeof(temp) - len - 1);
        }
        strncat(temp, in, sizeof(temp) - strlen(temp) - 1);
    }
    /* Force NUL-termination in case strncpy filled all 256 bytes. */
    temp[sizeof(temp)-1] = '\0';

    /* Normalize path: remove redundant /./ and handle /../ */
    /* parts[32]: array of interior pointers into temp[], each pointing to
     * one decoded path component (NUL-terminated in-place at the '/' char).
     * NOTE(VFS-03): capped at 32 slots; the 33rd component is silently
     * dropped because the (part_count < 32) guard skips the assignment. */
    char *parts[32];
    int part_count = 0;

    char *s = temp;
    /* Skip the leading '/' so strtok-style splitting starts on the first
     * component name, not an empty string before the root slash. */
    if (*s == '/') s++;

    char *token = s;
    while (token && *token) {
        /* NUL-terminate this component in-place; next points past the '/'. */
        char *next = strchr(token, '/');
        if (next) *next = '\0';

        if (strcmp(token, "..") == 0) {
            /* Parent: pop last component (floor at root, never below 0). */
            if (part_count > 0) part_count--;
        } else if (strcmp(token, ".") != 0 && strlen(token) > 0) {
            /* Normal component: store interior pointer into temp[].
             * The pointer is valid until the function returns (temp is on
             * the same stack frame and is not modified after this loop). */
            if (part_count < 32) parts[part_count++] = token;
        }

        if (next) token = next + 1;
        else break;
    }

    /* Reassemble the canonical path directly into out[] in ONE bounded pass:
     * "/" then each component separated by "/".  This replaces the previous
     * build-into-a-256B-scratch-then-strncpy approach whose per-component
     * strncat + strlen rescanned the whole accumulated string every time —
     * O(n^2) in the component count, plus a redundant full-buffer copy.  Now
     * each output byte is written exactly once, bounded by 'size'. */
    if (size == 0) return;
    size_t w = 0;
    if (w < size - 1) out[w++] = '/';
    for (int i = 0; i < part_count; i++) {
        if (i > 0 && w < size - 1) out[w++] = '/';
        for (const char *p = parts[i]; *p && w < size - 1; p++)
            out[w++] = *p;
    }
    /* w <= size-1 here, so this NUL is always in bounds. */
    out[w] = '\0';
}
