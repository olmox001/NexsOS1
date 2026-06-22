/*
 * kernel/include/kernel/vfs.h
 * Virtual Filesystem Switch — the filesystem provider contract.
 *
 * ASTRA seam (docs/ASTRA.md): the kernel core (syscall dispatch, ELF loader)
 * consumes ONLY this interface; the concrete filesystem driver (ext4 today)
 * is a provider registered behind it at boot.  The composition root is
 * kernel/main.c: vfs_register_fs(&ext4_fs_ops) then vfs_init().
 *
 * Resolution model (deliberately minimal for now):
 *   - One root mount ("/"), chosen at vfs_init() by probing the GPT/MBR
 *     partitions with every registered provider, in partition order.
 *   - Paths handed to providers are absolute, normalized (see
 *     vfs_resolve_path) and relative to the provider's own root.
 *   - vfs_node is a plain value type: no refcount, no open-file table; it
 *     stays valid as long as its mount (mounts are never torn down).  The
 *     per-process fd table is ABI-03 (epic #93) and will layer on top.
 */
#ifndef _KERNEL_VFS_H
#define _KERNEL_VFS_H

#include <kernel/types.h>

struct partition; /* kernel/gpt.h */
struct vfs_mount;

/* Node types (vfs_node.type / vfs_stat.type) */
#define VFS_TYPE_FILE 1
#define VFS_TYPE_DIR 2

/* An open filesystem object. */
struct vfs_node {
  struct vfs_mount *mnt;
  uint64_t id;   /* provider-private identifier (ext4: inode number) */
  uint64_t size; /* size in bytes at open time */
  uint32_t type; /* VFS_TYPE_* */
};

struct vfs_stat {
  uint64_t size;
  uint32_t type; /* VFS_TYPE_* */
};

/*
 * The provider contract.
 *
 * mount: probe 'part' and, if the on-disk format is recognised AND
 *        supported, fill mnt->fs_private and return 0.  A format that is
 *        simply not this filesystem must fail QUIETLY (probing is normal);
 *        a recognised-but-unsupported filesystem must fail LOUDLY (pr_err)
 *        so feature gaps never turn into silent corruption.
 * open:  resolve path → node.  Returns 0 / negative error.
 * read:  random-access read; returns bytes read (clamped at EOF) or <0.
 * write: path-based write (provider may not support it: NULL or -1).
 * list:  fill buf with space-separated names; returns length, -1 not
 *        found, -2 not a directory.
 */
struct fs_ops {
  const char *name;
  int (*mount)(struct vfs_mount *mnt, struct partition *part);
  int (*open)(struct vfs_mount *mnt, const char *path, struct vfs_node *out);
  int (*read)(struct vfs_node *node, uint64_t offset, void *buf,
              uint32_t size);
  int (*write)(struct vfs_mount *mnt, const char *path, uint64_t offset,
               const void *buf, uint32_t size);
  int (*list)(struct vfs_mount *mnt, const char *path, char *buf,
              uint32_t size);
};

struct vfs_mount {
  const struct fs_ops *ops;
  void *fs_private;        /* provider state, owned by ops->mount */
  const char *mountpoint;  /* absolute mount path ("/", "/reg", ...) — Plan 9 namespace */
  uint32_t part_index;     /* GPT/MBR partition index backing this mount (0 = synthetic) */
  int in_use;
};

/* Provider registration + mounting (called from the composition root) */
int vfs_register_fs(const struct fs_ops *ops);
void vfs_init(void);
/* vfs_mount_at - mount a provider at an absolute path (Plan 9-style namespace
 * mount).  'fs_private' is the provider's state (NULL for a stateless synthetic
 * server like regfs).  Path resolution routes by LONGEST matching mountpoint;
 * the root "/" mount is the fallback.  Returns 0, or -1 if the table is full /
 * args are bad.  Called from the composition root, single-threaded. */
int vfs_mount_at(const char *mountpoint, const struct fs_ops *ops, void *fs_private);

/* Core-facing API (the only filesystem surface outside kernel/fs/) */
int vfs_open(const char *path, struct vfs_node *out);
int vfs_read(struct vfs_node *node, uint64_t offset, void *buf, uint32_t size);
/* vfs_read_file: buf==NULL / size==0 returns the file size (userland ABI
 * for SYS_FILE_READ relies on this — user/sys/lib/lib.c file_read). */
int vfs_read_file(const char *path, void *buf, uint32_t size, uint64_t offset);
int vfs_write_file(const char *path, const void *buf, uint32_t size,
                   uint64_t offset);
int vfs_list_dir(const char *path, char *buf, uint32_t size);
int vfs_stat(const char *path, struct vfs_stat *st);

void vfs_resolve_path(const char *in, char *out, size_t size);

#endif /* _KERNEL_VFS_H */
