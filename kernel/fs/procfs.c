/*
 * kernel/fs/procfs.c
 * /proc — a synthetic namespace provider: live processes as TYPED capability
 * objects (the centrality model, ASTRA §6.2/§7; plan Stage 7 "everything in the
 * namespace").  A path /proc/<pid> resolves (object_at) to an OBJ_TYPE_PROCESS
 * object; reading it goes THROUGH the object/capability mechanism
 * (sys_object_read on the PROCESS handle, which reports the live state) — this
 * is NOT a side-channel that reformats process_pool into a fake file.
 *
 * Capability model: acquiring a /proc/<pid> handle is gated exactly like the
 * OS1_NS_PROC namespace (handle_create) — a destructive (WRITE/DESTROY) handle
 * needs kill authority; a READ status handle only needs the process to exist;
 * the read itself needs OS1_RIGHT_READ.  No partition (synthetic), read-only
 * (write/unlink unsupported).  Mounted at /proc by procfs_init().
 */
#include <kernel/types.h>
#include <kernel/vfs.h>
#include <kernel/sched.h>
#include <kernel/procfs.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <object.h> /* OBJ_TYPE_PROCESS */

/* Skip leading slashes; return the first path segment start. */
static const char *procfs_skip_slashes(const char *p) {
  while (*p == '/')
    p++;
  return p;
}

/*
 * procfs_object_at - map a /proc-relative path to the typed object it names.
 *   "/"        -> the directory (-2)
 *   "/<pid>"   -> OBJ_TYPE_PROCESS (if the process is live)
 * One level deep only (a deeper path is -1, not found).
 */
static int procfs_object_at(struct vfs_mount *mnt, const char *path,
                            struct vfs_objref *out) {
  (void)mnt;
  const char *p = procfs_skip_slashes(path);
  if (*p == '\0')
    return -2; /* /proc itself is a directory */
  for (const char *q = p; *q; q++)
    if (*q == '/')
      return -1; /* only /proc/<pid>, no deeper nodes yet */
  int pid = atoi(p);
  struct ps_info pi;
  if (pid <= 0 || proc_get_info(pid, &pi) != 0)
    return -1;
  out->obj_type = OBJ_TYPE_PROCESS;
  out->pid = pid;
  return 0;
}

/*
 * procfs_open - the fs_ops contract requires open().  /proc is a directory; a
 * /<pid> entry is really a PROCESS object (the byte read goes through that
 * object, not this node), so we expose it as a zero-size FILE node only so a
 * stat of the path succeeds.
 */
static int procfs_open(struct vfs_mount *mnt, const char *path,
                       struct vfs_node *out) {
  const char *p = procfs_skip_slashes(path);
  if (*p == '\0') {
    out->mnt = mnt;
    out->id = 0;
    out->type = VFS_TYPE_DIR;
    out->size = 0;
    return 0;
  }
  int pid = atoi(p);
  struct ps_info pi;
  if (pid <= 0 || proc_get_info(pid, &pi) != 0)
    return -1;
  out->mnt = mnt;
  out->id = (uint64_t)pid;
  out->type = VFS_TYPE_FILE;
  out->size = 0;
  return 0;
}

/* procfs_list - the live pids (each is a PROCESS capability object). */
static int procfs_list(struct vfs_mount *mnt, const char *path, char *buf,
                       uint32_t size) {
  (void)mnt;
  const char *p = procfs_skip_slashes(path);
  if (*p != '\0')
    return -2; /* /proc/<pid> is an object, not a directory */
  int pids[MAX_PROCESSES];
  int n = proc_enum_pids(pids, MAX_PROCESSES);
  size_t off = 0;
  for (int i = 0; i < n; i++) {
    char num[16];
    int nl = snprintf(num, sizeof(num), "%d", pids[i]);
    if (nl <= 0 || off + (size_t)nl + 2 > size)
      break;
    if (off)
      buf[off++] = ' ';
    memcpy(buf + off, num, (size_t)nl);
    off += (size_t)nl;
  }
  buf[off] = '\0';
  return (int)off;
}

static const struct fs_ops procfs_ops = {
    .name = "procfs",
    .mount = NULL, /* synthetic: mounted via vfs_mount_at, never probed */
    .open = procfs_open,
    .read = NULL,  /* a /proc/<pid> read goes through the PROCESS object */
    .write = NULL, /* read-only namespace */
    .list = procfs_list,
    .unlink = NULL,
    .object_at = procfs_object_at, /* path -> typed PROCESS object */
};

void procfs_init(void) { vfs_mount_at("/proc", &procfs_ops, NULL); }
