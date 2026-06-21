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

/* Mount table.  mounts[0] is the root mount; further slots are reserved for
 * future mountpoints.  Mounts are created once at vfs_init() and never torn
 * down, so readers need no locking. */
#define VFS_MAX_MOUNTS 4
static struct vfs_mount mounts[VFS_MAX_MOUNTS];

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
void vfs_init(void) {
  if (fs_driver_count == 0) {
    pr_err("%s", "VFS: no filesystem providers registered\n");
    return;
  }
  for (int i = 0; i < num_partitions; i++) {
    struct partition *p = gpt_get_partition(i);
    if (!p)
      continue;
    for (int d = 0; d < fs_driver_count; d++) {
      struct vfs_mount *mnt = &mounts[0];
      mnt->ops = fs_drivers[d];
      mnt->part_index = (uint32_t)i;
      mnt->fs_private = NULL;
      if (fs_drivers[d]->mount(mnt, p) == 0) {
        mnt->in_use = 1;
        pr_info("VFS: mounted %s on partition %d as /\n",
                fs_drivers[d]->name, i);
        return;
      }
      mnt->ops = NULL;
    }
  }
  pr_err("%s", "VFS: no mountable filesystem found on any partition\n");
}

/* Root mount accessor; NULL if vfs_init found nothing. */
static struct vfs_mount *vfs_root(void) {
  return mounts[0].in_use ? &mounts[0] : NULL;
}

/*
 * vfs_open - resolve an absolute normalized path to a vfs_node.
 * Returns 0 on success, negative on error/not-found.
 */
int vfs_open(const char *path, struct vfs_node *out) {
  struct vfs_mount *mnt = vfs_root();
  if (!mnt || !path || !out)
    return -1;
  return mnt->ops->open(mnt, path, out);
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
 * vfs_write_file - path-based write through the provider.
 * Returns bytes written or negative; -1 if the provider has no write support.
 */
int vfs_write_file(const char *path, const void *buf, uint32_t size,
                   uint64_t offset) {
  struct vfs_mount *mnt = vfs_root();
  if (!mnt || !mnt->ops->write)
    return -1;
  return mnt->ops->write(mnt, path, offset, buf, size);
}

/*
 * vfs_list_dir - list a directory through the provider.
 * Returns the formatted length, -1 not found, -2 not a directory.
 */
int vfs_list_dir(const char *path, char *buf, uint32_t size) {
  struct vfs_mount *mnt = vfs_root();
  if (!mnt || !mnt->ops->list)
    return -1;
  return mnt->ops->list(mnt, path, buf, size);
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
    /* normalized[]: final assembled canonical path; same 256-byte limit.
     * NOTE(VFS-04): no growth beyond 255 chars; deep paths are truncated
     * silently by strncat's capacity guard in the assembly loop below. */
    char normalized[256];
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

    /* Reassemble: start with root '/' then append each component. */
    strlcpy(normalized, "/", sizeof(normalized));
    for (int i = 0; i < part_count; i++) {
        strncat(normalized, parts[i], sizeof(normalized) - strlen(normalized) - 1);
        /* Insert '/' between components but not after the last one. */
        if (i < part_count - 1) strncat(normalized, "/", sizeof(normalized) - strlen(normalized) - 1);
    }

    strncpy(out, normalized, size);
    /* Guarantee NUL-termination even if strncpy ran out of space. */
    out[size-1] = '\0';
}
