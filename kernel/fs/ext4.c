/*
 * kernel/fs/ext4.c
 * Simplified Ext4 Driver — VFS provider (read + limited write)
 *
 * Role in the stack (ASTRA, docs/ASTRA.md):
 *   This driver is a filesystem PROVIDER behind the <kernel/vfs.h> contract.
 *   The only kernel-facing symbol is ext4_fs_ops; nothing outside kernel/fs/
 *   calls ext4_* anymore (VFS-01 resolved).  Per-mount state lives in a
 *   kmalloc'd struct ext4_fs hung off vfs_mount.fs_private (EXT4-12: multiple
 *   mounts are now structurally possible, though vfs_init mounts one root).
 *
 * On-disk layout accessed (4096-byte blocks, 8 sectors each):
 *   partition LBA 0        (LBA part+0)  : partition boot record (unused)
 *   superblock offset 1024 (LBA part+2)  : struct ext4_superblock
 *   GDT block 1            (LBA part+8)  : struct ext4_group_desc[0] at byte 0
 *   Block bitmap           (LBA part + bg_block_bitmap_lo × 8) : 4096-byte map
 *   Inode table            (LBA part + bg_inode_table_lo × 8)  : inode array
 *   Data blocks            (LBA part + phys_block × 8) : 4096-byte data
 *
 * Sector arithmetic common pattern:
 *   block N → sector = part_start_lba + (N × 8)   [8 = 4096 / 512]
 *   inode K → byte offset in table = (K-1) × inode_size (from superblock)
 *
 * Block mapping (EXT4-01 resolved):
 *   ext4_bmap() dispatches on i_flags & EXT4_EXTENTS_FL:
 *     - extent inodes: walk the extent tree rooted in i_block[] (any depth;
 *       unwritten extents and gaps read as zeros);
 *     - legacy inodes: direct (0-11), single-indirect (12), double-indirect
 *       (13) pointer blocks, as before.
 *
 * Mount-time enforcement (EXT4-06 resolved):
 *   - quiet probe failure when s_magic doesn't match (the VFS probes every
 *     partition; not-ext4 is normal);
 *   - LOUD rejection of: block size ≠ 4096, unsupported INCOMPAT features
 *     (64bit, dirty journal, anything unknown), multi-group images
 *     (EXT4-12/13 made explicit instead of silently misreading);
 *   - unknown RO_COMPAT features (e.g. metadata_csum, gdt_csum) force a
 *     read-only mount: our writer maintains no checksums, so writing would
 *     corrupt the image's self-consistency.
 *
 * Key invariants:
 *   - fs->lock serialises block allocation (alloc block bitmap r-m-w, GDT
 *     update, superblock update) and inode write-back (ext4_update_inode).
 *   - fs->sb/fs->bg/fs->part_start_lba are immutable after mount except for
 *     the allocation counters mutated under fs->lock.
 *   - The buffer cache (kernel/mm/buffer.c) is NOT used; all block I/O goes
 *     directly through virtio_blk_{read,write} (EXT4-15).
 *
 * Known issues (still open):
 *   EXT4-05  (W3 MISSING, partially lifted) Write supports: overwrite of any
 *            mapped block; growth on depth-0 extent roots with a free slot;
 *            growth through the legacy single-indirect range (~4.2 MB).
 *            Still rejected loudly: extent-tree growth (depth>0, full root,
 *            mid-hole insert), double-indirect allocation, file creation.
 *   EXT4-09  (W2 MISSING) ext4_list returns -2 without syscall-layer errno
 *            translation.
 *   EXT4-15  (W1 MISSING) Buffer cache bypassed; all I/O direct to virtio.
 *            (EXT4-11 resolved: per-loop interior-block cache, see
 *            struct ext4_icache.)
 */
#include <kernel/block.h>
#include <kernel/buffer.h>
#include <kernel/ext4.h>
#include <kernel/gpt.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

/* Per-mount driver state (vfs_mount.fs_private). */
struct ext4_fs {
  uint64_t part_start_lba; /* absolute LBA of the partition's first sector */
  uint32_t inode_size;     /* on-disk inode record size (sb.s_inode_size) */
  int read_only;           /* set at mount for unknown RO_COMPAT features */
  struct ext4_superblock sb;
  struct ext4_group_desc bg; /* group 0 descriptor (single-group images) */
  spinlock_t lock;
};

/*
 * Helper: Read/Write Sectors (absolute LBA; thin virtio wrappers).
 * NOTE(EXT4-15): bypasses the buffer cache (kernel/mm/buffer.c).
 */
static int ext4_bread(uint64_t sector, uint32_t count, void *buf) {
  return block_read(buf, sector, count);
}

static int ext4_bwrite(uint64_t sector, uint32_t count, void *buf) {
  return block_write(buf, sector, count);
}

/*
 * ext4_alloc_block - allocate one free block from block group 0.
 *
 * Algorithm:
 *   1. Read the group-0 block bitmap (one 4096-byte block) into a heap buffer.
 *   2. Under fs->lock: scan for the first clear bit (LSB-first per byte),
 *      set it, write the bitmap back.
 *   3. Decrement bg/sb free counters and write both back.
 *   4. Zero the new block on disk outside the lock (ownership established).
 *
 * Returns: absolute block number on success, 0 on failure (OOM, bitmap
 * inconsistency, or I/O error).
 */
static uint32_t ext4_alloc_block(struct ext4_fs *fs) {
  if (fs->bg.bg_free_blocks_count_lo == 0) {
    pr_err("%s", "Ext4: No free blocks in Group 0!\n");
    return 0;
  }

  uint64_t bitmap_blk = fs->bg.bg_block_bitmap_lo;
  uint8_t *bitmap = kmalloc(4096);
  if (!bitmap)
    return 0;

  uint64_t lock_flags;
  /* Lock before the bitmap read to serialise the whole read-modify-write
   * cycle; locking only the bit-set would let two allocators claim one bit. */
  spin_lock_irqsave(&fs->lock, &lock_flags);

  if (ext4_bread(fs->part_start_lba + (bitmap_blk * 8), 8, bitmap) != 0) {
    spin_unlock_irqrestore(&fs->lock, lock_flags);
    kfree(bitmap);
    return 0;
  }

  /* Scan 32768 bits, LSB-first within each byte. */
  uint32_t block_in_group = 0;
  int found = 0;
  for (int i = 0; i < 4096; i++) {
    if (bitmap[i] != 0xFF) {
      for (int bit = 0; bit < 8; bit++) {
        if (!((bitmap[i] >> bit) & 1)) {
          bitmap[i] |= (1 << bit);
          block_in_group = (i * 8) + bit;
          found = 1;
          break;
        }
      }
    }
    if (found)
      break;
  }

  if (!found) {
    pr_err("%s", "Ext4: Bitmap check failed (inconsistent with free_count)\n");
    spin_unlock_irqrestore(&fs->lock, lock_flags);
    kfree(bitmap);
    return 0;
  }

  if (ext4_bwrite(fs->part_start_lba + (bitmap_blk * 8), 8, bitmap) != 0) {
    pr_err("%s", "Ext4: Failed to update Block Bitmap\n");
    spin_unlock_irqrestore(&fs->lock, lock_flags);
    kfree(bitmap);
    return 0;
  }
  kfree(bitmap);

  fs->bg.bg_free_blocks_count_lo--;
  fs->sb.s_free_blocks_count_lo--;

  /* Write-back group descriptor 0 (32 bytes at the head of the GDT sector). */
  uint8_t *bg_buf = kmalloc(512);
  if (bg_buf && ext4_bread(fs->part_start_lba + 8, 1, bg_buf) == 0) {
    memcpy(bg_buf, &fs->bg, sizeof(fs->bg));
    ext4_bwrite(fs->part_start_lba + 8, 1, bg_buf);
  }
  if (bg_buf)
    kfree(bg_buf);

  /* Write-back superblock (1016 bytes at byte offset 1024). */
  uint8_t *sb_buf = kmalloc(4096);
  if (sb_buf && ext4_bread(fs->part_start_lba + 2, 2, sb_buf) == 0) {
    memcpy(sb_buf, &fs->sb, sizeof(fs->sb));
    ext4_bwrite(fs->part_start_lba + 2, 2, sb_buf);
  }
  if (sb_buf)
    kfree(sb_buf);
  spin_unlock_irqrestore(&fs->lock, lock_flags);

  /* Zero the new block outside the lock: ownership is established (bit set
   * and persisted), no concurrent allocator can claim it. */
  uint8_t *zero_buf = kmalloc(4096);
  if (zero_buf) {
    memset(zero_buf, 0, 4096);
    ext4_bwrite(fs->part_start_lba + (block_in_group * 8), 8, zero_buf);
    kfree(zero_buf);
  }

  return block_in_group;
}

/*
 * get_inode_struct - read inode 'ino' from the group-0 inode table.
 *
 * Copies sizeof(struct ext4_inode) == 128 bytes.  With inode_size 128 or 256
 * the record offset within its 512-byte sector is always a multiple of 128
 * with 128 bytes available before the boundary, so a single-sector read never
 * straddles (EXT4-10 resolved by construction; inode_size is validated at
 * mount to be a multiple of 128).
 */
static int get_inode_struct(struct ext4_fs *fs, uint32_t ino,
                            struct ext4_inode *inode_out) {
  uint64_t table_blk = fs->bg.bg_inode_table_lo;
  uint64_t table_byte_offset = table_blk * 4096;
  uint64_t inode_offset = table_byte_offset + (uint64_t)(ino - 1) * fs->inode_size;

  uint64_t sector = fs->part_start_lba + (inode_offset / 512);
  uint32_t sector_off = inode_offset % 512;

  uint8_t *k_buf = kmalloc(512);
  if (!k_buf)
    return -1;
  if (block_read(k_buf, sector, 1) != 0) {
    kfree(k_buf);
    return -1;
  }

  memcpy(inode_out, k_buf + sector_off, sizeof(struct ext4_inode));
  kfree(k_buf);
  return 0;
}

/*
 * ext4_update_inode - read-modify-write inode 'ino' back to the table.
 * Serialised by fs->lock (shared with the allocator's metadata writes).
 */
static int ext4_update_inode(struct ext4_fs *fs, uint32_t ino,
                             struct ext4_inode *inode) {
  uint64_t table_blk = fs->bg.bg_inode_table_lo;
  uint64_t table_byte_offset = table_blk * 4096;
  uint64_t inode_offset = table_byte_offset + (uint64_t)(ino - 1) * fs->inode_size;

  uint64_t sector = fs->part_start_lba + (inode_offset / 512);
  uint32_t sector_off = inode_offset % 512;

  uint8_t *k_buf = kmalloc(512);
  if (!k_buf)
    return -1;

  uint64_t lock_flags;
  spin_lock_irqsave(&fs->lock, &lock_flags);
  if (block_read(k_buf, sector, 1) != 0) {
    spin_unlock_irqrestore(&fs->lock, lock_flags);
    kfree(k_buf);
    return -1;
  }

  memcpy(k_buf + sector_off, inode, sizeof(struct ext4_inode));
  int ret = block_write(k_buf, sector, 1);
  spin_unlock_irqrestore(&fs->lock, lock_flags);
  kfree(k_buf);
  return ret;
}

/*
 * Interior-block cache (EXT4-11 resolved).
 *
 * A read/write loop calls ext4_bmap once per 4 KB chunk; without caching,
 * every call re-reads the same interior metadata blocks (indirect pointer
 * blocks, extent index/leaf nodes) from disk.  An ext4_icache lives on the
 * caller's frame for the duration of one loop and holds the last interior
 * block seen per tree level (two levels cover double-indirect and
 * depth-2 extent trees; deeper levels share slot 1).  Sequential access —
 * the dominant pattern (ELF load, WAD streaming, dir scans) — then reads
 * each metadata block once instead of once per chunk.
 *
 * Writers that modify mapping metadata on disk must icache_invalidate()
 * so later lookups in the same loop re-read the updated block.
 */
struct ext4_icache {
  uint64_t blk[2]; /* cached block number per level; 0 = empty slot */
  uint8_t *buf[2]; /* lazily allocated 4 KB buffers */
};

static void icache_release(struct ext4_icache *c) {
  for (int i = 0; i < 2; i++) {
    if (c->buf[i])
      kfree(c->buf[i]);
    c->buf[i] = NULL;
    c->blk[i] = 0;
  }
}

static void icache_invalidate(struct ext4_icache *c) {
  c->blk[0] = 0;
  c->blk[1] = 0;
}

/* Return the contents of interior block 'blk' via the level slot, reading
 * from disk only on a cache miss.  NULL on OOM or I/O error. */
static uint8_t *icache_get(struct ext4_fs *fs, struct ext4_icache *c,
                           int level, uint64_t blk) {
  if (level > 1)
    level = 1;
  if (c->buf[level] && c->blk[level] == blk)
    return c->buf[level];
  if (!c->buf[level]) {
    c->buf[level] = kmalloc(4096);
    if (!c->buf[level])
      return NULL;
  }
  if (ext4_bread(fs->part_start_lba + blk * 8, 8, c->buf[level]) != 0) {
    c->blk[level] = 0;
    return NULL;
  }
  c->blk[level] = blk;
  return c->buf[level];
}

/*
 * ext4_extent_lookup - map logical block → physical block via the extent
 * tree rooted in i_block[] (EXT4_EXTENTS_FL inodes).
 *
 * Walks interior nodes (eh_depth > 0: ext4_extent_idx records, sorted by
 * ei_block; the child covering 'blk' is the last entry with ei_block <= blk)
 * down to the leaf (ext4_extent records).  Holes — logical blocks covered by
 * no extent — and unwritten extents (ee_len > EXT4_EXT_UNWRITTEN_LEN) yield
 * *phys_out = 0, which the read loop turns into zeros, matching sparse-file
 * semantics of the legacy path.
 *
 * Returns 0 on success (*phys_out set; 0 means "reads as zeros"),
 * -1 on a structurally invalid tree or I/O error.
 */
static int ext4_extent_lookup(struct ext4_fs *fs,
                              const struct ext4_inode *inode, uint32_t blk,
                              uint64_t *phys_out, struct ext4_icache *cache) {
  const struct ext4_extent_header *eh =
      (const struct ext4_extent_header *)inode->i_block;
  /* Max records per node: 4 in the 60-byte i_block root, 340 in a 4 KB
   * block ((4096-12)/12).  Used to bound eh_entries before scanning. */
  uint16_t max_entries = 4;
  *phys_out = 0;

  /* eh_depth is bounded by 5 in the format; the guard catches cycles. */
  for (int level = 0; level < 6; level++) {
    if (eh->eh_magic != EXT4_EXT_MAGIC || eh->eh_entries > eh->eh_max ||
        eh->eh_entries > max_entries) {
      pr_err("Ext4: invalid extent node (magic=0x%x entries=%d max=%d)\n",
             eh->eh_magic, eh->eh_entries, eh->eh_max);
      return -1;
    }

    if (eh->eh_depth > 0) {
      /* Interior node: descend into the last child with ei_block <= blk. */
      const struct ext4_extent_idx *ix =
          (const struct ext4_extent_idx *)(eh + 1);
      int sel = -1;
      for (int i = 0; i < eh->eh_entries; i++) {
        if (ix[i].ei_block <= blk)
          sel = i;
        else
          break;
      }
      if (sel < 0)
        return 0; /* blk precedes the first mapped range: hole. */
      uint64_t child =
          ix[sel].ei_leaf_lo | ((uint64_t)ix[sel].ei_leaf_hi << 32);
      const uint8_t *node = icache_get(fs, cache, level, child);
      if (!node)
        return -1;
      eh = (const struct ext4_extent_header *)node;
      max_entries = (4096 - sizeof(*eh)) / sizeof(struct ext4_extent);
      continue;
    }

    /* Leaf node: find the extent covering blk. */
    const struct ext4_extent *ex = (const struct ext4_extent *)(eh + 1);
    for (int i = 0; i < eh->eh_entries; i++) {
      uint32_t start = ex[i].ee_block;
      int unwritten = ex[i].ee_len > EXT4_EXT_UNWRITTEN_LEN;
      uint32_t len =
          unwritten ? ex[i].ee_len - EXT4_EXT_UNWRITTEN_LEN : ex[i].ee_len;
      if (blk >= start && blk < start + len) {
        if (!unwritten)
          *phys_out = (ex[i].ee_start_lo |
                       ((uint64_t)ex[i].ee_start_hi << 32)) +
                      (blk - start);
        break; /* unwritten: leave *phys_out = 0 → reads as zeros */
      }
    }
    return 0; /* no covering extent: hole → zeros */
  }

  pr_err("%s", "Ext4: extent tree deeper than 5 levels (corrupt?)\n");
  return -1;
}

/*
 * ext4_bmap - map logical file block → physical block.
 * Dispatches on EXT4_EXTENTS_FL (EXT4-01); the legacy path supports direct
 * (0-11), single-indirect (12) and double-indirect (13) pointers.
 * *phys_out == 0 means "hole: reads as zeros".  Returns 0 / -1.
 * Interior metadata blocks go through the caller's icache (EXT4-11).
 */
static int ext4_bmap(struct ext4_fs *fs, const struct ext4_inode *inode,
                     uint32_t block_idx, uint64_t *phys_out,
                     struct ext4_icache *cache) {
  if (inode->i_flags & EXT4_EXTENTS_FL)
    return ext4_extent_lookup(fs, inode, block_idx, phys_out, cache);

  *phys_out = 0;
  if (block_idx < 12) {
    *phys_out = inode->i_block[block_idx];
    return 0;
  }

  if (block_idx < 12 + 1024) {
    uint32_t indirect_blk_num = inode->i_block[12];
    if (indirect_blk_num == 0)
      return 0; /* unallocated pointer block: hole */
    const uint8_t *ind = icache_get(fs, cache, 0, indirect_blk_num);
    if (!ind)
      return -1;
    *phys_out = ((const uint32_t *)ind)[block_idx - 12];
    return 0;
  }

  uint32_t d_idx = block_idx - 12 - 1024;
  uint32_t master_idx = d_idx / 1024;
  uint32_t sub_idx = d_idx % 1024;
  uint32_t double_indir_blk = inode->i_block[13];

  if (double_indir_blk == 0 || master_idx >= 1024)
    return double_indir_blk == 0 ? 0 : -1;
  const uint8_t *master = icache_get(fs, cache, 0, double_indir_blk);
  if (!master)
    return -1;
  uint32_t sub_indir_blk = ((const uint32_t *)master)[master_idx];
  if (sub_indir_blk == 0)
    return 0; /* hole */
  const uint8_t *sub = icache_get(fs, cache, 1, sub_indir_blk);
  if (!sub)
    return -1;
  *phys_out = ((const uint32_t *)sub)[sub_idx];
  return 0;
}

/*
 * ext4_read_data - random-access read from an already-fetched inode.
 * 64-bit offset/file-size arithmetic (EXT4-08 resolved).
 * Returns bytes read (clamped at EOF) or -1.
 */
static int ext4_read_data(struct ext4_fs *fs, const struct ext4_inode *inode,
                          uint64_t offset, uint8_t *buf, uint32_t size) {
  uint64_t file_size =
      inode->i_size_lo | ((uint64_t)inode->i_size_high << 32);
  if (size == 0 || buf == NULL)
    return 0;
  if (offset >= file_size)
    return 0;
  if (offset + size > file_size)
    size = (uint32_t)(file_size - offset);

  uint32_t bytes_read = 0;
  uint64_t current_offset = offset;
  struct ext4_icache cache = {{0, 0}, {NULL, NULL}};

  uint8_t *block_buf = kmalloc(4096);
  if (!block_buf)
    return -1;

  while (bytes_read < size) {
    uint32_t block_idx = (uint32_t)(current_offset / 4096);
    uint32_t block_off = current_offset % 4096;
    uint32_t to_copy = 4096 - block_off;
    if (to_copy > (size - bytes_read))
      to_copy = size - bytes_read;

    uint64_t phys_block;
    if (ext4_bmap(fs, inode, block_idx, &phys_block, &cache) != 0) {
      kfree(block_buf);
      icache_release(&cache);
      return -1;
    }

    if (phys_block == 0) {
      /* Sparse hole / unwritten extent. */
      memset(block_buf, 0, 4096);
    } else {
      uint64_t sector = fs->part_start_lba + (phys_block * 8);
      if (block_read(block_buf, sector, 8) != 0) {
        kfree(block_buf);
        icache_release(&cache);
        return -1;
      }
    }

    memcpy(buf + bytes_read, block_buf + block_off, to_copy);
    bytes_read += to_copy;
    current_offset += to_copy;
  }

  kfree(block_buf);
  icache_release(&cache);
  return bytes_read;
}

/*
 * read_inode_data - convenience: fetch inode 'ino' then read its data.
 */
static int read_inode_data(struct ext4_fs *fs, uint32_t ino, uint64_t offset,
                           uint8_t *buf, uint32_t size) {
  struct ext4_inode inode;
  if (get_inode_struct(fs, ino, &inode) != 0) {
    pr_err("Ext4: Failed to get inode struct for ino %d\n", ino);
    return -1;
  }
  return ext4_read_data(fs, &inode, offset, buf, size);
}

/*
 * ext4_lookup_in_dir - find 'name' in directory inode dir_ino.
 */
static int ext4_lookup_in_dir(struct ext4_fs *fs, uint32_t dir_ino,
                              const char *name, uint32_t name_len,
                              uint32_t *ino_out) {
  struct ext4_inode inode;
  if (get_inode_struct(fs, dir_ino, &inode) != 0)
    return -1;

  uint32_t dir_size = inode.i_size_lo;
  uint32_t current_blk_off = 0;
  uint8_t *dir_buf = kmalloc(4096);
  if (!dir_buf)
    return -1;

  while (current_blk_off < dir_size) {
    if (ext4_read_data(fs, &inode, current_blk_off, dir_buf, 4096) <= 0)
      break;

    struct ext4_dir_entry *de = (struct ext4_dir_entry *)dir_buf;
    uint32_t offset = 0;
    while (offset < 4096) {
      if (de->inode == 0)
        break;
      if (de->rec_len < 8 || de->rec_len > (4096 - offset))
        break;

      if (de->name_len > 0 && de->name_len == name_len) {
        if (memcmp(de->name, name, name_len) == 0) {
          *ino_out = de->inode;
          kfree(dir_buf);
          return 0;
        }
      }
      offset += de->rec_len;
      de = (struct ext4_dir_entry *)((uint8_t *)de + de->rec_len);
    }
    current_blk_off += 4096;
  }
  kfree(dir_buf);
  return -1;
}

/*
 * ext4_find_ino - resolve an absolute path to an inode number (walk from
 * the root inode, component by component).
 */
static int ext4_find_ino(struct ext4_fs *fs, const char *path,
                         uint32_t *ino_out) {
  uint32_t current_ino = EXT4_ROOT_INO;
  const char *p = path;

  if (*p == '/')
    p++;

  while (*p) {
    const char *start = p;
    while (*p && *p != '/')
      p++;
    uint32_t len = p - start;

    if (len > 0) {
      uint32_t next_ino;
      if (ext4_lookup_in_dir(fs, current_ino, start, len, &next_ino) != 0)
        return -1;
      current_ino = next_ino;
    }

    if (*p == '/')
      p++;
  }

  *ino_out = current_ino;
  return 0;
}

/* ------------------------------------------------------------------ */
/* VFS provider entry points (the fs_ops contract)                    */
/* ------------------------------------------------------------------ */

/*
 * ext4_mount - probe + mount the partition (fs_ops.mount).
 * Quiet on "not ext4" (the VFS probes every partition); loud on recognised-
 * but-unsupported images.  See the file header for the enforcement matrix.
 */
static int ext4_mount(struct vfs_mount *mnt, struct partition *p) {
  uint8_t *k_buf = kmalloc(4096);
  if (!k_buf)
    return -1;

  /* Superblock lives at byte offset 1024 = sector 2 of the partition. */
  if (block_read(k_buf, p->start_lba + 2, 2) != 0) {
    kfree(k_buf);
    return -1;
  }

  struct ext4_fs *fs = kmalloc(sizeof(struct ext4_fs));
  if (!fs) {
    kfree(k_buf);
    return -1;
  }
  memset(fs, 0, sizeof(*fs));
  memcpy(&fs->sb, k_buf, sizeof(fs->sb));

  if (fs->sb.s_magic != EXT4_MAGIC) {
    /* Not an ext4 partition: quiet probe failure. */
    kfree(k_buf);
    kfree(fs);
    return -1;
  }

  /* From here on the partition IS ext4: unsupported features fail loudly. */
  int reject = 0;

  if (fs->sb.s_log_block_size != 2) {
    pr_err("Ext4: unsupported block size (log=%d, only 4096 supported)\n",
           fs->sb.s_log_block_size);
    reject = 1;
  }

  uint32_t bad_incompat = fs->sb.s_feature_incompat & ~EXT4_INCOMPAT_SUPPORTED;
  if (!reject && bad_incompat) {
    pr_err("Ext4: unsupported INCOMPAT features 0x%x (supported mask 0x%x) - "
           "refusing to mount\n",
           bad_incompat, EXT4_INCOMPAT_SUPPORTED);
    reject = 1;
  }

  /* Single-group images only: group-0 GDT/bitmaps/inode-table are the only
   * metadata this driver reads (EXT4-12/13 enforced instead of misreading). */
  uint32_t bpg = fs->sb.s_blocks_per_group ? fs->sb.s_blocks_per_group : 32768;
  if (!reject &&
      (fs->sb.s_blocks_count_hi != 0 || fs->sb.s_blocks_count_lo > bpg)) {
    pr_err("Ext4: multi-group image (%d blocks, %d per group) unsupported\n",
           fs->sb.s_blocks_count_lo, bpg);
    reject = 1;
  }

  /* Inode record size: rev0 fixes it at 128; rev1+ reads s_inode_size.
   * Must be a multiple of 128 ≥ sizeof(struct ext4_inode) so table records
   * never straddle a 512-byte sector read (see get_inode_struct). */
  fs->inode_size = (fs->sb.s_rev_level == 0) ? 128 : fs->sb.s_inode_size;
  if (!reject &&
      (fs->inode_size < 128 || fs->inode_size > 4096 ||
       (fs->inode_size % 128) != 0)) {
    pr_err("Ext4: invalid inode size %d\n", fs->inode_size);
    reject = 1;
  }

  if (reject) {
    kfree(k_buf);
    kfree(fs);
    return -1;
  }

  uint32_t bad_rocompat =
      fs->sb.s_feature_ro_compat & ~EXT4_RO_COMPAT_WRITE_SAFE;
  if (bad_rocompat) {
    pr_warn("Ext4: RO_COMPAT features 0x%x not write-safe - mounting "
            "read-only\n",
            bad_rocompat);
    fs->read_only = 1;
  }

  /* Group descriptor 0: block 1 = byte 4096 = sector 8. */
  if (block_read(k_buf, p->start_lba + 8, 1) != 0) {
    pr_err("%s", "Ext4: Failed to read GDT\n");
    kfree(k_buf);
    kfree(fs);
    return -1;
  }
  memcpy(&fs->bg, k_buf, sizeof(fs->bg));
  kfree(k_buf);

  fs->part_start_lba = p->start_lba;
  spin_lock_init(&fs->lock);
  mnt->fs_private = fs;

  pr_info("Ext4: Mounted. Vol=%s, Inodes=%d, features incompat=0x%x%s\n",
          fs->sb.s_volume_name, fs->sb.s_inodes_count,
          fs->sb.s_feature_incompat, fs->read_only ? " (read-only)" : "");
  return 0;
}

/*
 * ext4_open - path → vfs_node (fs_ops.open).
 */
static int ext4_open(struct vfs_mount *mnt, const char *path,
                     struct vfs_node *out) {
  struct ext4_fs *fs = mnt->fs_private;
  uint32_t ino;
  struct ext4_inode inode;

  if (ext4_find_ino(fs, path, &ino) != 0)
    return -1;
  if (get_inode_struct(fs, ino, &inode) != 0)
    return -1;

  out->mnt = mnt;
  out->id = ino;
  out->size = inode.i_size_lo | ((uint64_t)inode.i_size_high << 32);
  out->type = ((inode.i_mode >> 12) == 4) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
  return 0;
}

/*
 * ext4_read - random-access read from an open node (fs_ops.read).
 */
static int ext4_read(struct vfs_node *node, uint64_t offset, void *buf,
                     uint32_t size) {
  struct ext4_fs *fs = node->mnt->fs_private;
  return read_inode_data(fs, (uint32_t)node->id, offset, buf, size);
}

/*
 * ext4_extent_can_append - can a new block mapping for 'logical' be added
 * to the inode's extent tree?  Conservative: only depth-0 roots with a free
 * record slot and logical positions at/after the current end are accepted
 * (mid-hole inserts would need record shifting; a full root would need tree
 * growth — both still EXT4-05 territory).
 */
static int ext4_extent_can_append(const struct ext4_inode *inode,
                                  uint32_t logical) {
  const struct ext4_extent_header *eh =
      (const struct ext4_extent_header *)inode->i_block;
  const struct ext4_extent *ex = (const struct ext4_extent *)(eh + 1);

  if (eh->eh_magic != EXT4_EXT_MAGIC || eh->eh_depth != 0)
    return 0;
  if (eh->eh_entries > 0) {
    const struct ext4_extent *last = &ex[eh->eh_entries - 1];
    uint32_t len = last->ee_len > EXT4_EXT_UNWRITTEN_LEN
                       ? last->ee_len - EXT4_EXT_UNWRITTEN_LEN
                       : last->ee_len;
    if (logical < last->ee_block + len)
      return 0; /* would be a mid-hole insert */
  }
  return eh->eh_entries < eh->eh_max;
}

/*
 * ext4_extent_do_append - record the mapping logical→phys in a depth-0 root
 * previously validated by ext4_extent_can_append.  Extends the last extent
 * when physically contiguous, else appends a new record (sorted order is
 * preserved because logical >= last end).
 */
static void ext4_extent_do_append(struct ext4_inode *inode, uint32_t logical,
                                  uint32_t phys) {
  struct ext4_extent_header *eh =
      (struct ext4_extent_header *)inode->i_block;
  struct ext4_extent *ex = (struct ext4_extent *)(eh + 1);

  if (eh->eh_entries > 0) {
    struct ext4_extent *last = &ex[eh->eh_entries - 1];
    if (last->ee_len < EXT4_EXT_UNWRITTEN_LEN - 1 &&
        logical == last->ee_block + last->ee_len &&
        phys == last->ee_start_lo + last->ee_len && last->ee_start_hi == 0) {
      last->ee_len++;
      return;
    }
  }
  ex[eh->eh_entries].ee_block = logical;
  ex[eh->eh_entries].ee_len = 1;
  ex[eh->eh_entries].ee_start_hi = 0;
  ex[eh->eh_entries].ee_start_lo = phys;
  eh->eh_entries++;
}

/*
 * ext4_write - path-based write (fs_ops.write; overwrite/append).
 *
 * Supported (EXT4-05 partially lifted):
 *   - overwrite of any already-mapped block (extent trees of any depth,
 *     legacy direct/indirect/double-indirect) — no metadata change;
 *   - growth on extent inodes with a depth-0 root and a free record slot
 *     (extend-if-contiguous, else new extent; sparse tail writes OK);
 *   - growth on legacy inodes through the single-indirect range
 *     (12+1024 blocks ≈ 4.2 MB), allocating the pointer block on demand.
 * Still rejected loudly: read-only mounts, extent-tree growth (depth>0 /
 * full root / mid-hole insert), double-indirect allocation, file creation.
 */
static int ext4_write(struct vfs_mount *mnt, const char *path,
                      uint64_t offset64, const void *vbuf, uint32_t size) {
  struct ext4_fs *fs = mnt->fs_private;
  const uint8_t *buf = vbuf;

  if (fs->read_only) {
    pr_err("%s", "Ext4: write rejected (read-only mount)\n");
    return -1;
  }

  uint32_t ino;
  if (ext4_find_ino(fs, path, &ino) != 0) {
    /* A missing file is a NORMAL negative result of a userland open(), not a
     * kernel error — log at debug so a process probing paths (or the stress
     * file lane) cannot spam the console (perf §1; Linux likewise never printk's
     * on ENOENT). */
    pr_debug("Ext4: File not found: %s\n", path);
    return -1;
  }

  struct ext4_inode inode;
  if (get_inode_struct(fs, ino, &inode) != 0)
    return -1;

  int is_extent = (inode.i_flags & EXT4_EXTENTS_FL) != 0;

  /* 32-bit write path; bound offset before narrowing. */
  if (offset64 > 0xFFFFFFFFULL ||
      offset64 > (uint64_t)inode.i_size_lo + 1048576) {
    pr_err("EXT4: Write offset 0x%lx exceeds reasonable bounds "
           "(i_size_lo=0x%x)\n",
           (unsigned long)offset64, inode.i_size_lo);
    return -1;
  }
  uint32_t offset = (uint32_t)offset64;

  uint32_t bytes_written = 0;
  uint32_t current_offset = offset;
  struct ext4_icache cache = {{0, 0}, {NULL, NULL}};

  uint8_t *block_buf = kmalloc(4096);
  if (!block_buf)
    return -1;

  while (bytes_written < size) {
    uint32_t block_idx = current_offset / 4096;
    uint32_t block_off = current_offset % 4096;
    uint32_t to_copy = 4096 - block_off;
    if (to_copy > (size - bytes_written))
      to_copy = size - bytes_written;

    uint64_t phys_block;
    if (ext4_bmap(fs, &inode, block_idx, &phys_block, &cache) != 0)
      goto fail;

    if (phys_block == 0) {
      /* Unmapped block: validate the mapping is recordable BEFORE
       * allocating, so a reject never leaks a bitmap bit. */
      if (is_extent && !ext4_extent_can_append(&inode, block_idx)) {
        pr_err("%s", "Ext4: extent-tree growth (depth>0, full root or "
                     "mid-hole insert) not supported yet\n");
        goto fail;
      }
      if (!is_extent && block_idx >= 12 + 1024) {
        pr_err("%s", "Ext4: double-indirect block write not supported yet\n");
        goto fail;
      }

      uint32_t nb = ext4_alloc_block(fs);
      if (nb == 0) {
        pr_err("%s", "Ext4: Block allocation failed\n");
        goto fail;
      }

      if (is_extent) {
        ext4_extent_do_append(&inode, block_idx, nb);
      } else if (block_idx < 12) {
        inode.i_block[block_idx] = nb;
      } else {
        /* Single-indirect: allocate the pointer block on demand (it comes
         * back zeroed from ext4_alloc_block = all slots NULL), then install
         * the new pointer with a sector-granular read-modify-write. */
        uint32_t ind = inode.i_block[12];
        if (ind == 0) {
          ind = ext4_alloc_block(fs);
          if (ind == 0) {
            pr_err("%s", "Ext4: Block allocation failed\n");
            goto fail;
          }
          inode.i_block[12] = ind;
          inode.i_blocks_lo += (4096 / 512);
        }
        uint64_t slot_off = (uint64_t)ind * 4096 + (block_idx - 12) * 4;
        uint64_t slot_sector = fs->part_start_lba + slot_off / 512;
        uint8_t *secbuf = kmalloc(512);
        if (!secbuf)
          goto fail;
        if (ext4_bread(slot_sector, 1, secbuf) != 0) {
          kfree(secbuf);
          goto fail;
        }
        *(uint32_t *)(secbuf + (slot_off % 512)) = nb;
        if (ext4_bwrite(slot_sector, 1, secbuf) != 0) {
          kfree(secbuf);
          goto fail;
        }
        kfree(secbuf);
        /* The pointer block changed on disk: drop cached copies. */
        icache_invalidate(&cache);
      }

      /* i_blocks_lo counts 512-byte sectors. */
      inode.i_blocks_lo += (4096 / 512);
      phys_block = nb;
    }

    /* Read-modify-write for partial blocks. */
    if (block_off != 0 || to_copy != 4096) {
      if (ext4_bread(fs->part_start_lba + (phys_block * 8), 8, block_buf) !=
          0)
        goto fail;
    }

    memcpy(block_buf + block_off, buf + bytes_written, to_copy);

    if (ext4_bwrite(fs->part_start_lba + (phys_block * 8), 8, block_buf) !=
        0)
      goto fail;

    bytes_written += to_copy;
    current_offset += to_copy;
  }

  kfree(block_buf);
  icache_release(&cache);

  if (current_offset > inode.i_size_lo)
    inode.i_size_lo = current_offset;

  if (ext4_update_inode(fs, ino, &inode) != 0) {
    pr_err("%s", "Ext4: Failed to update inode\n");
    return -1;
  }

  return bytes_written;

fail:
  kfree(block_buf);
  icache_release(&cache);
  return -1;
}

/*
 * ext4_list - directory listing as a space-separated string (fs_ops.list).
 * Returns the formatted length, -1 not found, -2 not a directory (EXT4-09).
 */
static int ext4_list(struct vfs_mount *mnt, const char *path, char *buf,
                     uint32_t size) {
  struct ext4_fs *fs = mnt->fs_private;
  uint32_t dir_ino;
  if (ext4_find_ino(fs, path, &dir_ino) != 0)
    return -1;

  struct ext4_inode inode;
  if (get_inode_struct(fs, dir_ino, &inode) != 0)
    return -1;

  if (!((inode.i_mode >> 12) == 4))
    return -2;

  uint32_t dir_size = inode.i_size_lo;
  uint32_t current_blk_off = 0;
  uint8_t *dir_buf = kmalloc(4096);
  if (!dir_buf)
    return -1;

  uint32_t buf_pos = 0;
  if (size > 0)
    buf[0] = '\0';

  while (current_blk_off < dir_size) {
    if (ext4_read_data(fs, &inode, current_blk_off, dir_buf, 4096) <= 0)
      break;

    struct ext4_dir_entry *de = (struct ext4_dir_entry *)dir_buf;
    uint32_t offset = 0;
    while (offset < 4096) {
      /* Two-sided rec_len guard, same as the lookup path: a corrupt rec_len
       * larger than the block remainder stops the walk (defence in depth;
       * the offset re-check below already prevented the OOB dereference). */
      if (de->inode == 0 || de->rec_len < 8 || de->rec_len > (4096 - offset))
        break;

      if (de->name_len > 0) {
        if (buf_pos + de->name_len + 1 < size) {
          memcpy(buf + buf_pos, de->name, de->name_len);
          buf_pos += de->name_len;
          buf[buf_pos++] = ' ';
          buf[buf_pos] = '\0';
        }
      }

      offset += de->rec_len;
      if (offset >= 4096)
        break;
      de = (struct ext4_dir_entry *)((uint8_t *)de + de->rec_len);
    }
    current_blk_off += 4096;
  }

  kfree(dir_buf);
  return (int)buf_pos;
}

/* ------------------------------------------------------------------ */
/* File/dir creation and removal (issue #126, NOTE(M4.5-FS-WRITE))    */
/*                                                                      */
/* Everything below mirrors the read/write/allocate patterns already   */
/* established above (ext4_alloc_block, ext4_lookup_in_dir, ext4_bmap, */
/* ext4_extent_lookup) instead of inventing new on-disk-format logic.  */
/* Scope: regular files only (VFS_TYPE_DIR creation / rmdir are out of */
/* scope for this pass - creating "." / ".." bootstrap adds directory- */
/* specific complexity nothing here needs yet).                        */
/* ------------------------------------------------------------------ */

/*
 * ext4_sync_bg_sb - write back the in-memory group descriptor + superblock.
 * Caller holds fs->lock (same write-back pattern as ext4_alloc_block).
 */
static void ext4_sync_bg_sb(struct ext4_fs *fs) {
  uint8_t *bg_buf = kmalloc(512);
  if (bg_buf && ext4_bread(fs->part_start_lba + 8, 1, bg_buf) == 0) {
    memcpy(bg_buf, &fs->bg, sizeof(fs->bg));
    ext4_bwrite(fs->part_start_lba + 8, 1, bg_buf);
  }
  if (bg_buf)
    kfree(bg_buf);

  uint8_t *sb_buf = kmalloc(4096);
  if (sb_buf && ext4_bread(fs->part_start_lba + 2, 2, sb_buf) == 0) {
    memcpy(sb_buf, &fs->sb, sizeof(fs->sb));
    ext4_bwrite(fs->part_start_lba + 2, 2, sb_buf);
  }
  if (sb_buf)
    kfree(sb_buf);
}

/*
 * ext4_alloc_inode - allocate one free inode from group 0's inode bitmap.
 * Mirrors ext4_alloc_block exactly, targeting the inode bitmap/table instead
 * of the block bitmap.  Bit i of the inode bitmap <-> inode number i+1
 * (standard ext4 convention; matches get_inode_struct's (ino-1) offset math).
 * Returns inode number (1-indexed) on success, 0 on failure.
 */
static uint32_t ext4_alloc_inode(struct ext4_fs *fs) {
  if (fs->bg.bg_free_inodes_count_lo == 0) {
    pr_err("%s", "Ext4: No free inodes in Group 0!\n");
    return 0;
  }

  uint64_t bitmap_blk = fs->bg.bg_inode_bitmap_lo;
  uint8_t *bitmap = kmalloc(4096);
  if (!bitmap)
    return 0;

  uint64_t lock_flags;
  spin_lock_irqsave(&fs->lock, &lock_flags);

  if (ext4_bread(fs->part_start_lba + (bitmap_blk * 8), 8, bitmap) != 0) {
    spin_unlock_irqrestore(&fs->lock, lock_flags);
    kfree(bitmap);
    return 0;
  }

  uint32_t max_bits = fs->sb.s_inodes_count;
  if (max_bits > 32768)
    max_bits = 32768;

  uint32_t inode_bit = 0;
  int found = 0;
  for (uint32_t i = 0; i < 4096 && !found; i++) {
    if (bitmap[i] == 0xFF)
      continue;
    for (int bit = 0; bit < 8; bit++) {
      uint32_t idx = i * 8 + (uint32_t)bit;
      if (idx >= max_bits)
        break;
      if (!((bitmap[i] >> bit) & 1)) {
        bitmap[i] |= (uint8_t)(1 << bit);
        inode_bit = idx;
        found = 1;
        break;
      }
    }
  }

  if (!found) {
    pr_err("%s", "Ext4: Inode bitmap check failed (inconsistent with "
                 "free_count)\n");
    spin_unlock_irqrestore(&fs->lock, lock_flags);
    kfree(bitmap);
    return 0;
  }

  if (ext4_bwrite(fs->part_start_lba + (bitmap_blk * 8), 8, bitmap) != 0) {
    pr_err("%s", "Ext4: Failed to update Inode Bitmap\n");
    spin_unlock_irqrestore(&fs->lock, lock_flags);
    kfree(bitmap);
    return 0;
  }
  kfree(bitmap);

  fs->bg.bg_free_inodes_count_lo--;
  fs->sb.s_free_inodes_count--;
  ext4_sync_bg_sb(fs);
  spin_unlock_irqrestore(&fs->lock, lock_flags);

  return inode_bit + 1;
}

/*
 * ext4_free_block - release one block back to group 0's bitmap (opposite of
 * ext4_alloc_block). A block already free (or out of range) is a no-op, so a
 * double-free never corrupts the free-count.
 */
static void ext4_free_block(struct ext4_fs *fs, uint32_t block_in_group) {
  if (block_in_group == 0)
    return;
  uint32_t byte = block_in_group / 8, bit = block_in_group % 8;
  if (byte >= 4096)
    return;

  uint64_t bitmap_blk = fs->bg.bg_block_bitmap_lo;
  uint8_t *bitmap = kmalloc(4096);
  if (!bitmap)
    return;

  uint64_t lock_flags;
  spin_lock_irqsave(&fs->lock, &lock_flags);
  if (ext4_bread(fs->part_start_lba + (bitmap_blk * 8), 8, bitmap) != 0) {
    spin_unlock_irqrestore(&fs->lock, lock_flags);
    kfree(bitmap);
    return;
  }
  if (!((bitmap[byte] >> bit) & 1)) {
    spin_unlock_irqrestore(&fs->lock, lock_flags); /* already free */
    kfree(bitmap);
    return;
  }
  bitmap[byte] &= (uint8_t)~(1 << bit);
  if (ext4_bwrite(fs->part_start_lba + (bitmap_blk * 8), 8, bitmap) != 0) {
    spin_unlock_irqrestore(&fs->lock, lock_flags);
    kfree(bitmap);
    return;
  }
  kfree(bitmap);

  fs->bg.bg_free_blocks_count_lo++;
  fs->sb.s_free_blocks_count_lo++;
  ext4_sync_bg_sb(fs);
  spin_unlock_irqrestore(&fs->lock, lock_flags);
}

/* ext4_free_inode - release one inode bit (opposite of ext4_alloc_inode). */
static void ext4_free_inode(struct ext4_fs *fs, uint32_t ino) {
  uint32_t bit = ino - 1;
  uint32_t byte = bit / 8, b = bit % 8;
  if (byte >= 4096)
    return;

  uint64_t bitmap_blk = fs->bg.bg_inode_bitmap_lo;
  uint8_t *bitmap = kmalloc(4096);
  if (!bitmap)
    return;

  uint64_t lock_flags;
  spin_lock_irqsave(&fs->lock, &lock_flags);
  if (ext4_bread(fs->part_start_lba + (bitmap_blk * 8), 8, bitmap) != 0) {
    spin_unlock_irqrestore(&fs->lock, lock_flags);
    kfree(bitmap);
    return;
  }
  if (!((bitmap[byte] >> b) & 1)) {
    spin_unlock_irqrestore(&fs->lock, lock_flags); /* already free */
    kfree(bitmap);
    return;
  }
  bitmap[byte] &= (uint8_t)~(1 << b);
  if (ext4_bwrite(fs->part_start_lba + (bitmap_blk * 8), 8, bitmap) != 0) {
    spin_unlock_irqrestore(&fs->lock, lock_flags);
    kfree(bitmap);
    return;
  }
  kfree(bitmap);

  fs->bg.bg_free_inodes_count_lo++;
  fs->sb.s_free_inodes_count++;
  ext4_sync_bg_sb(fs);
  spin_unlock_irqrestore(&fs->lock, lock_flags);
}

/*
 * ext4_extent_free_tree - recursively free every block an extent (sub)tree
 * references, INCLUDING interior index-node blocks themselves.  Mirrors
 * ext4_extent_lookup's descent (same depth bound: the format caps eh_depth
 * at 5, guarded here against a corrupt tree causing unbounded recursion).
 * 'node_buf' is the 60-byte root (i_block[]) at depth 0, or a full 4KB
 * interior/leaf block read by the caller for depth > 0.
 */
static void ext4_extent_free_tree(struct ext4_fs *fs, const uint8_t *node_buf,
                                  int depth_guard) {
  if (depth_guard > 6)
    return;
  const struct ext4_extent_header *eh =
      (const struct ext4_extent_header *)node_buf;
  if (eh->eh_magic != EXT4_EXT_MAGIC)
    return;

  if (eh->eh_depth > 0) {
    const struct ext4_extent_idx *ix =
        (const struct ext4_extent_idx *)(eh + 1);
    for (int i = 0; i < eh->eh_entries; i++) {
      uint64_t child =
          ix[i].ei_leaf_lo | ((uint64_t)ix[i].ei_leaf_hi << 32);
      uint8_t *cbuf = kmalloc(4096);
      if (cbuf) {
        if (ext4_bread(fs->part_start_lba + child * 8, 8, cbuf) == 0)
          ext4_extent_free_tree(fs, cbuf, depth_guard + 1);
        kfree(cbuf);
      }
      ext4_free_block(fs, (uint32_t)child);
    }
  } else {
    const struct ext4_extent *ex = (const struct ext4_extent *)(eh + 1);
    for (int i = 0; i < eh->eh_entries; i++) {
      int unwritten = ex[i].ee_len > EXT4_EXT_UNWRITTEN_LEN;
      uint32_t len =
          unwritten ? ex[i].ee_len - EXT4_EXT_UNWRITTEN_LEN : ex[i].ee_len;
      uint64_t start =
          ex[i].ee_start_lo | ((uint64_t)ex[i].ee_start_hi << 32);
      for (uint32_t b = 0; b < len; b++)
        ext4_free_block(fs, (uint32_t)(start + b));
    }
  }
}

/*
 * ext4_free_inode_blocks - free every data/metadata block an inode owns,
 * dispatching on EXT4_EXTENTS_FL exactly like ext4_bmap does for reads.
 */
static void ext4_free_inode_blocks(struct ext4_fs *fs,
                                   const struct ext4_inode *inode) {
  if (inode->i_flags & EXT4_EXTENTS_FL) {
    ext4_extent_free_tree(fs, (const uint8_t *)inode->i_block, 0);
    return;
  }

  for (int i = 0; i < 12; i++)
    if (inode->i_block[i])
      ext4_free_block(fs, inode->i_block[i]);

  if (inode->i_block[12]) {
    uint8_t *ind = kmalloc(4096);
    if (ind) {
      if (ext4_bread(fs->part_start_lba + (uint64_t)inode->i_block[12] * 8, 8,
                     ind) == 0) {
        const uint32_t *ptrs = (const uint32_t *)ind;
        for (int i = 0; i < 1024; i++)
          if (ptrs[i])
            ext4_free_block(fs, ptrs[i]);
      }
      kfree(ind);
    }
    ext4_free_block(fs, inode->i_block[12]);
  }

  if (inode->i_block[13]) {
    uint8_t *dbl = kmalloc(4096);
    if (dbl) {
      if (ext4_bread(fs->part_start_lba + (uint64_t)inode->i_block[13] * 8, 8,
                     dbl) == 0) {
        const uint32_t *ptrs = (const uint32_t *)dbl;
        for (int i = 0; i < 1024; i++) {
          if (!ptrs[i])
            continue;
          uint8_t *sub = kmalloc(4096);
          if (sub) {
            if (ext4_bread(fs->part_start_lba + (uint64_t)ptrs[i] * 8, 8,
                           sub) == 0) {
              const uint32_t *sptrs = (const uint32_t *)sub;
              for (int j = 0; j < 1024; j++)
                if (sptrs[j])
                  ext4_free_block(fs, sptrs[j]);
            }
            kfree(sub);
          }
          ext4_free_block(fs, ptrs[i]);
        }
      }
      kfree(dbl);
    }
    ext4_free_block(fs, inode->i_block[13]);
  }
}

/*
 * ext4_resolve_parent - split an absolute path into its parent directory's
 * inode and the final path component (name/name_len), resolving the parent
 * via the existing ext4_find_ino walk.  Used by create and unlink.
 */
static int ext4_resolve_parent(struct ext4_fs *fs, const char *path,
                               uint32_t *parent_ino_out, const char **name_out,
                               uint32_t *name_len_out) {
  const char *last_slash = NULL;
  for (const char *p = path; *p; p++)
    if (*p == '/')
      last_slash = p;

  const char *name = last_slash ? last_slash + 1 : path;
  uint32_t name_len = (uint32_t)strlen(name);
  if (name_len == 0 || name_len > 255)
    return -1;
  if ((name_len == 1 && name[0] == '.') ||
      (name_len == 2 && name[0] == '.' && name[1] == '.'))
    return -1; /* reject creating/removing "." or ".." */

  uint32_t parent_ino;
  if (!last_slash || last_slash == path) {
    parent_ino = EXT4_ROOT_INO;
  } else {
    char parent_path[256];
    uint32_t plen = (uint32_t)(last_slash - path);
    if (plen >= sizeof(parent_path))
      return -1;
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';
    if (ext4_find_ino(fs, parent_path, &parent_ino) != 0)
      return -1;
  }

  *parent_ino_out = parent_ino;
  *name_out = name;
  *name_len_out = name_len;
  return 0;
}

/* dirent_ideal_len - minimal 4-byte-aligned rec_len for a name of this
 * length (8-byte header + name, rounded up), matching tools/mkdisk.c's
 * '(8 + nlen + 3) & ~3' convention exactly. */
static inline uint16_t dirent_ideal_len(uint32_t name_len) {
  return (uint16_t)((8 + name_len + 3) & ~3u);
}

/* ext4_write_dir_block - write back one already-mapped logical block of a
 * directory's data (bmap to physical, then a plain block write). */
static int ext4_write_dir_block(struct ext4_fs *fs,
                                const struct ext4_inode *inode,
                                uint32_t block_idx, const uint8_t *buf) {
  uint64_t phys;
  struct ext4_icache c = {{0, 0}, {NULL, NULL}};
  int rc = ext4_bmap(fs, inode, block_idx, &phys, &c);
  icache_release(&c);
  if (rc != 0 || phys == 0)
    return -1;
  return ext4_bwrite(fs->part_start_lba + (phys * 8), 8, (void *)buf);
}

/*
 * ext4_dir_insert - insert one directory entry (name -> ino) into dir_ino's
 * data, following the exact same "split trailing slack, else append a new
 * whole-block entry" convention tools/mkdisk.c uses to build the initial
 * image (see its rec_len-to-end-of-block handling): the LAST entry in a
 * block always carries the block's remaining slack in its rec_len, so a
 * subsequent insert either splits that slack or, if none is left anywhere,
 * allocates a fresh block and seeds it with one whole-block entry (the new
 * "last entry") ready to be split by the next insert.
 *
 * A dirent with inode==0 is a freed slot (see ext4_dir_remove): its whole
 * rec_len is reusable, no split needed.
 *
 * Only depth-0 extent growth / legacy direct blocks (<12) are supported for
 * a NEW directory block, matching ext4_write's own EXT4-05 boundary -
 * directories in this driver are expected to stay small.
 */
static int ext4_dir_insert(struct ext4_fs *fs, uint32_t dir_ino,
                           const char *name, uint32_t name_len,
                           uint32_t new_ino, uint8_t file_type) {
  uint16_t need = dirent_ideal_len(name_len);

  struct ext4_inode dir_inode;
  if (get_inode_struct(fs, dir_ino, &dir_inode) != 0)
    return -1;

  int is_extent = (dir_inode.i_flags & EXT4_EXTENTS_FL) != 0;
  uint32_t dir_size = dir_inode.i_size_lo;
  uint8_t *blk_buf = kmalloc(4096);
  if (!blk_buf)
    return -1;

  for (uint32_t bidx = 0; bidx * 4096 < dir_size; bidx++) {
    if (ext4_read_data(fs, &dir_inode, (uint64_t)bidx * 4096, blk_buf, 4096) <=
        0)
      break;

    uint32_t off = 0;
    while (off < 4096) {
      struct ext4_dir_entry *de = (struct ext4_dir_entry *)(blk_buf + off);
      if (de->rec_len < 8 || de->rec_len > (4096 - off))
        break; /* corrupt/end-of-chain: stop scanning this block */

      uint16_t used = de->inode ? dirent_ideal_len(de->name_len) : 0;
      uint16_t slack = de->rec_len - used;
      if (slack >= need) {
        if (de->inode == 0) {
          de->inode = new_ino;
          de->name_len = (uint8_t)name_len;
          de->file_type = file_type;
          memcpy(de->name, name, name_len);
        } else {
          uint16_t old_rec_len = de->rec_len;
          de->rec_len = used;
          struct ext4_dir_entry *nde =
              (struct ext4_dir_entry *)(blk_buf + off + used);
          nde->inode = new_ino;
          nde->rec_len = old_rec_len - used;
          nde->name_len = (uint8_t)name_len;
          nde->file_type = file_type;
          memcpy(nde->name, name, name_len);
        }
        int rc = ext4_write_dir_block(fs, &dir_inode, bidx, blk_buf);
        kfree(blk_buf);
        return rc;
      }
      off += de->rec_len;
    }
  }

  /* No slack anywhere: append a new block, seeded with one whole-block
   * entry (the new "last entry", carrying all of the block's slack). */
  uint32_t new_blk_idx = (dir_size > 0) ? (dir_size - 1) / 4096 + 1 : 0;
  uint32_t nb = ext4_alloc_block(fs);
  if (nb == 0) {
    kfree(blk_buf);
    return -1;
  }

  if (is_extent) {
    if (!ext4_extent_can_append(&dir_inode, new_blk_idx)) {
      pr_err("%s", "Ext4: directory extent growth not supported yet\n");
      kfree(blk_buf);
      return -1;
    }
    ext4_extent_do_append(&dir_inode, new_blk_idx, nb);
  } else if (new_blk_idx < 12) {
    dir_inode.i_block[new_blk_idx] = nb;
  } else {
    pr_err("%s", "Ext4: directory indirect growth not supported yet\n");
    kfree(blk_buf);
    return -1;
  }
  dir_inode.i_blocks_lo += (4096 / 512);

  memset(blk_buf, 0, 4096);
  struct ext4_dir_entry *nde = (struct ext4_dir_entry *)blk_buf;
  nde->inode = new_ino;
  nde->rec_len = 4096;
  nde->name_len = (uint8_t)name_len;
  nde->file_type = file_type;
  memcpy(nde->name, name, name_len);

  int rc = ext4_bwrite(fs->part_start_lba + (nb * 8), 8, blk_buf);
  kfree(blk_buf);
  if (rc != 0)
    return -1;

  dir_inode.i_size_lo = (new_blk_idx + 1) * 4096;
  return ext4_update_inode(fs, dir_ino, &dir_inode);
}

/*
 * ext4_dir_remove - clear the directory entry named 'name' in dir_ino
 * (inode = 0, rec_len kept as-is so ext4_dir_insert can reclaim the slot -
 * the exact inverse of the "free slot" branch above). Returns 0 found and
 * cleared, -1 not found.
 */
static int ext4_dir_remove(struct ext4_fs *fs, uint32_t dir_ino,
                           const char *name, uint32_t name_len) {
  struct ext4_inode dir_inode;
  if (get_inode_struct(fs, dir_ino, &dir_inode) != 0)
    return -1;

  uint32_t dir_size = dir_inode.i_size_lo;
  uint8_t *blk_buf = kmalloc(4096);
  if (!blk_buf)
    return -1;

  for (uint32_t bidx = 0; bidx * 4096 < dir_size; bidx++) {
    if (ext4_read_data(fs, &dir_inode, (uint64_t)bidx * 4096, blk_buf, 4096) <=
        0)
      break;

    uint32_t off = 0;
    while (off < 4096) {
      struct ext4_dir_entry *de = (struct ext4_dir_entry *)(blk_buf + off);
      if (de->rec_len < 8 || de->rec_len > (4096 - off))
        break;

      if (de->inode != 0 && de->name_len == name_len &&
          memcmp(de->name, name, name_len) == 0) {
        de->inode = 0;
        int rc = ext4_write_dir_block(fs, &dir_inode, bidx, blk_buf);
        kfree(blk_buf);
        return rc;
      }
      off += de->rec_len;
    }
  }

  kfree(blk_buf);
  return -1;
}

/*
 * ext4_create - fs_ops.create.  Regular files only (VFS_TYPE_DIR is out of
 * scope, see file-header comment above this section).  New inodes are
 * legacy-mapped (i_flags=0, no extent header): ext4_write's existing
 * direct-block growth path (block_idx < 12) then allocates their first data
 * block the first time something is written, with zero new code needed here.
 * Rejects if the path already exists (caller must not race this, single
 * fs->lock-free window between the vfs_stat/ext4_find_ino checks above this
 * layer and here - same race class every VFS write already tolerates.)
 */
static int ext4_create(struct vfs_mount *mnt, const char *path,
                       uint32_t vfs_type) {
  struct ext4_fs *fs = mnt->fs_private;
  if (fs->read_only)
    return -1;
  if (vfs_type != VFS_TYPE_FILE) {
    pr_err("%s", "Ext4: directory creation not supported yet\n");
    return -1;
  }

  uint32_t existing;
  if (ext4_find_ino(fs, path, &existing) == 0)
    return -1; /* already exists */

  uint32_t parent_ino;
  const char *name;
  uint32_t name_len;
  if (ext4_resolve_parent(fs, path, &parent_ino, &name, &name_len) != 0)
    return -1;

  uint32_t new_ino = ext4_alloc_inode(fs);
  if (new_ino == 0)
    return -1;

  struct ext4_inode inode;
  memset(&inode, 0, sizeof(inode));
  inode.i_mode = 0x8000 | 0644; /* S_IFREG | rw-r--r-- */
  inode.i_links_count = 1;
  if (ext4_update_inode(fs, new_ino, &inode) != 0) {
    ext4_free_inode(fs, new_ino); /* undo: no dirent references it yet */
    return -1;
  }

  if (ext4_dir_insert(fs, parent_ino, name, name_len, new_ino,
                      EXT4_FT_REG_FILE) != 0) {
    ext4_free_inode(fs, new_ino); /* undo: still no dirent, safe to free */
    return -1;
  }

  return 0;
}

/*
 * ext4_unlink - fs_ops.unlink.  Regular files only (directories need '.'/
 * '..'/emptiness handling out of scope here). Order matters for crash
 * safety: remove the dirent FIRST (so a crash after this point leaves an
 * orphaned-but-allocated inode+blocks, recoverable, rather than a live
 * dirent pointing at freed blocks, which would corrupt a later read).
 */
static int ext4_unlink(struct vfs_mount *mnt, const char *path) {
  struct ext4_fs *fs = mnt->fs_private;
  if (fs->read_only)
    return -1;

  uint32_t ino;
  if (ext4_find_ino(fs, path, &ino) != 0)
    return -1;

  struct ext4_inode inode;
  if (get_inode_struct(fs, ino, &inode) != 0)
    return -1;
  if ((inode.i_mode >> 12) == 4) {
    pr_err("%s", "Ext4: unlink of a directory not supported yet\n");
    return -1;
  }

  uint32_t parent_ino;
  const char *name;
  uint32_t name_len;
  if (ext4_resolve_parent(fs, path, &parent_ino, &name, &name_len) != 0)
    return -1;

  if (ext4_dir_remove(fs, parent_ino, name, name_len) != 0)
    return -1;

  ext4_free_inode_blocks(fs, &inode);
  ext4_free_inode(fs, ino);
  return 0;
}

const struct fs_ops ext4_fs_ops = {
    .name = "ext4",
    .mount = ext4_mount,
    .open = ext4_open,
    .read = ext4_read,
    .write = ext4_write,
    .list = ext4_list,
    .unlink = ext4_unlink,
    .create = ext4_create,
};
