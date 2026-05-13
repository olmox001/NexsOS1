/*
 * kernel/fs/ext4.c
 * Simplified Read-Only Ext4 Driver (With Random Access)
 */
#include <drivers/virtio_blk.h>
#include <kernel/buffer.h>
#include <kernel/ext4.h>
#include <kernel/gpt.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>
#include <kernel/string.h>

/* Global State */
static uint64_t part_start_lba;
static struct ext4_superblock sb;
static struct ext4_group_desc bg;
static DEFINE_SPINLOCK(ext4_lock);

/*
 * Helper: Read/Write Sectors
 */
static int ext4_bread(uint64_t sector, uint32_t count, void *buf) {
  return virtio_blk_read(buf, sector, count);
}

static int ext4_bwrite(uint64_t sector, uint32_t count, void *buf) {
  return virtio_blk_write(buf, sector, count);
}

/*
 * Allocator: Allocate a new block from Group 0
 * Returns Block Number (0 on failure)
 */
static uint32_t ext4_alloc_block(void) {
  if (bg.bg_free_blocks_count_lo == 0) {
    pr_err("%s", "Ext4: No free blocks in Group 0!\n");
    return 0;
  }

  uint64_t bitmap_blk = bg.bg_block_bitmap_lo;
  uint8_t *bitmap = kmalloc(4096);
  if (!bitmap)
    return 0;

  uint64_t lock_flags;
  spin_lock_irqsave(&ext4_lock, &lock_flags);

  /* Read Bitmap */
  if (ext4_bread(part_start_lba + (bitmap_blk * 8), 8, bitmap) != 0) {
    spin_unlock_irqrestore(&ext4_lock, lock_flags);
    kfree(bitmap);
    return 0;
  }

  /* Find Free Bit */
  /* Note: Block 0 is Boot Block, usually reserved/used.
     We start searching from block 0 of the group. */
  uint32_t block_in_group = 0;
  int found = 0;
  for (int i = 0; i < 4096; i++) {
    if (bitmap[i] != 0xFF) {
      for (int bit = 0; bit < 8; bit++) {
        if (!((bitmap[i] >> bit) & 1)) {
          /* Found free bit */
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
    spin_unlock_irqrestore(&ext4_lock, lock_flags);
    kfree(bitmap);
    return 0;
  }

  /* Write Bitmap Back */
  if (ext4_bwrite(part_start_lba + (bitmap_blk * 8), 8, bitmap) != 0) {
    pr_err("%s", "Ext4: Failed to update Block Bitmap\n");
    spin_unlock_irqrestore(&ext4_lock, lock_flags);
    kfree(bitmap);
    return 0;
  }
  kfree(bitmap);

  /* Update Descriptor & Superblock */
  bg.bg_free_blocks_count_lo--;
  sb.s_free_blocks_count_lo--;

  uint8_t *bg_buf = kmalloc(512);
  if (bg_buf && ext4_bread(part_start_lba + 8, 1, bg_buf) == 0) {
    memcpy(bg_buf, &bg, sizeof(bg)); /* Update BG0 in place */
    ext4_bwrite(part_start_lba + 8, 1, bg_buf);
  }
  if (bg_buf) kfree(bg_buf);

  uint8_t *sb_buf = kmalloc(4096);
  if (sb_buf && ext4_bread(part_start_lba + 2, 2, sb_buf) == 0) {
    memcpy(sb_buf, &sb, sizeof(sb));
    ext4_bwrite(part_start_lba + 2, 2, sb_buf);
  }
  if (sb_buf) kfree(sb_buf);
  spin_unlock_irqrestore(&ext4_lock, lock_flags);

  /* Return Absolute Block Number */
  /* For Group 0, this is just block_in_group */
  /* Actually first blocks are metadata. The bitmap tracks them as used. */
  /* So block_in_group IS the absolute block number for Group 0. */

  /* Zero out the new block content before returning? Security/Cleanliness. */
  uint8_t *zero_buf = kmalloc(4096);
  if (zero_buf) {
    memset(zero_buf, 0, 4096);
    ext4_bwrite(part_start_lba + (block_in_group * 8), 8, zero_buf);
    kfree(zero_buf);
  }

  return block_in_group;
}

/*
 * Initialize Ext4
 */
void ext4_init(void) {
  /* 1. Find Userland Partition (Index 2) */
  struct partition *p = gpt_get_partition(2);
  if (!p) {
    pr_err("%s", "Ext4: Partition 2 not found!\n");
    return;
  }
  part_start_lba = p->start_lba;
  pr_info("Ext4: Found partition at LBA %ld\n", part_start_lba);

  /* 2. Read Superblock (Offset 1024) - LBA + 2 */
  uint8_t *k_buf = kmalloc(4096);
  if (!k_buf) {
    pr_err("%s", "Ext4: OOM allocating SB buffer\n");
    return;
  }
  if (virtio_blk_read(k_buf, part_start_lba + 2, 2) != 0) {
    pr_err("%s", "Ext4: Failed to read Superblock\n");
    kfree(k_buf);
    return;
  }
  memcpy(&sb, k_buf, sizeof(sb));

  if (sb.s_magic != EXT4_MAGIC) {
    pr_err("Ext4: Invalid Magic: 0x%04x at LBA %ld\n", sb.s_magic,
           part_start_lba + 2);
    kfree(k_buf);
    return;
  }

  pr_info("Ext4: Mounted. Vol=%s, Inodes=%d\n", sb.s_volume_name,
          sb.s_inodes_count);

  /* 3. Read Block Group Descriptor 0 (Block 1) */
  /* GDT starts at Block 1 (Bytes 4096). 8 Sectors from start. */
  if (virtio_blk_read(k_buf, part_start_lba + 8, 1) != 0) {
    pr_err("%s", "Ext4: Failed to read GDT\n");
    kfree(k_buf);
    return;
  }
  memcpy(&bg, k_buf, sizeof(bg));
  kfree(k_buf);

  pr_info("Ext4: Group 0: Inode Table at Block %d\n", bg.bg_inode_table_lo);
}

/*
 * Helper: Read an Inode Structure from Table
 */
static int get_inode_struct(uint32_t ino, struct ext4_inode *inode_out) {
  /* Inode Table Block */
  uint64_t table_blk = bg.bg_inode_table_lo;
  uint64_t table_byte_offset = table_blk * 4096;
  uint64_t inode_offset = table_byte_offset + (ino - 1) * EXT4_INODE_SIZE;

  uint64_t sector = part_start_lba + (inode_offset / 512);
  uint32_t sector_off = inode_offset % 512;

  uint8_t *k_buf = kmalloc(512);
  if (!k_buf)
    return -1;
  if (virtio_blk_read(k_buf, sector, 1) != 0) {
    kfree(k_buf);
    return -1;
  }

  memcpy(inode_out, k_buf + sector_off, sizeof(struct ext4_inode));
  kfree(k_buf);
  return 0;
}

/*
 * Helper: Write an Inode Structure to Table
 */
static int ext4_update_inode(uint32_t ino, struct ext4_inode *inode) {
  uint64_t table_blk = bg.bg_inode_table_lo;
  uint64_t table_byte_offset = table_blk * 4096;
  uint64_t inode_offset = table_byte_offset + (ino - 1) * EXT4_INODE_SIZE;

  uint64_t sector = part_start_lba + (inode_offset / 512);
  uint32_t sector_off = inode_offset % 512;

  /* Read-Modify-Write */
  uint8_t *k_buf = kmalloc(512);
  if (!k_buf)
    return -1;

  uint64_t lock_flags;
  spin_lock_irqsave(&ext4_lock, &lock_flags);
  if (virtio_blk_read(k_buf, sector, 1) != 0) {
    spin_unlock_irqrestore(&ext4_lock, lock_flags);
    kfree(k_buf);
    return -1;
  }

  memcpy(k_buf + sector_off, inode, sizeof(struct ext4_inode));
  int ret = virtio_blk_write(k_buf, sector, 1);
  spin_unlock_irqrestore(&ext4_lock, lock_flags);
  kfree(k_buf);
  return ret;
}

/*
 * Public API: Read Data from Inode (Random Access)
 * Supports Direct Blocks (0-11) and Single Indirect Blocks (12).
 */
int ext4_read_inode(uint32_t ino, uint32_t offset, uint8_t *buf,
                    uint32_t size) {
  struct ext4_inode inode;
  if (get_inode_struct(ino, &inode) != 0) {
    pr_err("Ext4: Failed to get inode struct for ino %d\n", ino);
    return -1;
  }

  uint32_t file_size = inode.i_size_lo;
  if (offset >= file_size)
    return 0;
  if (offset + size > file_size)
    size = file_size - offset;

  uint32_t bytes_read = 0;
  uint32_t current_offset = offset;

  /* Allocate buffers on heap to save stack space */
  uint8_t *block_buf = kmalloc(4096);
  uint8_t *indirect_buf = kmalloc(4096);

  if (!block_buf || !indirect_buf) {
    if (block_buf)
      kfree(block_buf);
    if (indirect_buf)
      kfree(indirect_buf);
    return -1;
  }

  while (bytes_read < size) {
    /* Calculate Block Index in File */
    uint32_t block_idx = current_offset / 4096;
    uint32_t block_off = current_offset % 4096;
    uint32_t to_copy = 4096 - block_off;
    if (to_copy > (size - bytes_read))
      to_copy = size - bytes_read;

    /* Get Physical Block Number */
    uint32_t phys_block = 0;

    if (block_idx < 12) {
      /* Direct Block */
      phys_block = inode.i_block[block_idx];
    } else {
      /* Indirect Block (Index 12 points to block of pointers) */
      uint32_t indirect_blk_num = inode.i_block[12];

      if (indirect_blk_num == 0) {
        pr_err("%s", "Ext4: Indirect block not allocated!\n");
        kfree(block_buf);
        kfree(indirect_buf);
        return -1;
      }

      /* Read Indirect Block */
      uint64_t sector_indir = part_start_lba + (indirect_blk_num * 8);
      if (virtio_blk_read(indirect_buf, sector_indir, 8) != 0) {
        pr_err("Ext4: Failed to read Indirect Block at %d\n", indirect_blk_num);
        kfree(block_buf);
        kfree(indirect_buf);
        return -1;
      }

      uint32_t *pointers = (uint32_t *)indirect_buf;
      uint32_t indirect_idx = block_idx - 12;

      if (indirect_idx >= 1024) { /* 4096 / 4 = 1024 pointers */
        pr_err("Ext4: Double Indirect Blocks not supported (Index %d)\n",
               block_idx);
        kfree(block_buf);
        kfree(indirect_buf);
        return -1;
      }

      phys_block = pointers[indirect_idx];
    }

    /* Read the Block */
    if (phys_block == 0) {
      /* Sparse hole */
      memset(block_buf, 0, 4096);
    } else {
      uint64_t sector = part_start_lba + (phys_block * 8);
      if (virtio_blk_read(block_buf, sector, 8) != 0) {
        kfree(block_buf);
        kfree(indirect_buf);
        return -1;
      }
    }

    /* Copy Data */
    memcpy(buf + bytes_read, block_buf + block_off, to_copy);

    bytes_read += to_copy;
    current_offset += to_copy;
  }

  kfree(block_buf);
  kfree(indirect_buf);
  return bytes_read;
}

/*
 * Public API: Find Inode by Path
 */
/*
 * Helper: Find entry in a directory inode
 */
static int ext4_lookup_in_dir(uint32_t dir_ino, const char *name,
                              uint32_t name_len, uint32_t *ino_out) {
  struct ext4_inode inode;
  if (get_inode_struct(dir_ino, &inode) != 0)
    return -1;

  uint32_t dir_size = inode.i_size_lo;
  uint32_t current_blk_off = 0;
  uint8_t *dir_buf = kmalloc(4096);
  if (!dir_buf)
    return -1;

  while (current_blk_off < dir_size) {
    if (ext4_read_inode(dir_ino, current_blk_off, dir_buf, 4096) <= 0) {
      break;
    }

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
 * Public API: Find Inode by Path (Recursive)
 */
int ext4_find_inode(const char *path, uint32_t *ino_out) {
  uint32_t current_ino = 2; /* Start at Root */
  const char *p = path;

  /* Skip leading slash */
  if (*p == '/')
    p++;

  while (*p) {
    /* Find next segment */
    const char *start = p;
    while (*p && *p != '/')
      p++;
    uint32_t len = p - start;

    if (len > 0) {
      uint32_t next_ino;
      if (ext4_lookup_in_dir(current_ino, start, len, &next_ino) != 0) {
        return -1;
      }
      current_ino = next_ino;
    }

    /* Skip slash */
    if (*p == '/')
      p++;
  }

  *ino_out = current_ino;
  return 0;
}

/*
 * Legacy Wrapper
 */
/*
 * Legacy Wrapper
 */
int ext4_read_file(const char *path, uint8_t *buf, uint32_t size,
                   uint32_t offset) {
  uint32_t ino;
  if (ext4_find_inode(path, &ino) != 0) {
    pr_err("Ext4: File not found: %s\n", path);
    return -1;
  }
  return ext4_read_inode(ino, offset, buf, size);
}

/*
 * Public API: Write Data to File (Overwrite/Append)
 */
/*
 * Public API: Write Data to File (Overwrite/Append)
 */
int ext4_write_file(const char *path, const uint8_t *buf, uint32_t size,
                    uint32_t offset) {
  uint32_t ino;
  if (ext4_find_inode(path, &ino) != 0) {
    pr_err("Ext4: File not found: %s\n", path);
    return -1;
  }

  struct ext4_inode inode;
  if (get_inode_struct(ino, &inode) != 0)
    return -1;

  uint32_t bytes_written = 0;

  /* Use provided offset. If offset is valid, start there. */
  /* Note: sparse files not fully supported, so gaps might trigger errors or be
   * zeroed if we had that logic. */
  /* For now, just trust offset is reasonable (<= size or slightly past for
   * simple expansion) */

  uint32_t current_offset = offset;

  /* Check if we need to append (current_offset + size > i_size) */

  /* Buffer allocation */
  uint8_t *block_buf = kmalloc(4096);
  if (!block_buf)
    return -1;

  while (bytes_written < size) {
    uint32_t block_idx = current_offset / 4096;
    uint32_t block_off = current_offset % 4096;
    uint32_t to_copy = 4096 - block_off;
    if (to_copy > (size - bytes_written))
      to_copy = size - bytes_written;

    /* Check/Alloc Block */
    if (block_idx >= 12) {
      pr_err("%s", "Ext4: Indirect block write not supported yet\n");
      kfree(block_buf);
      return -1;
    }

    uint32_t phys_block = inode.i_block[block_idx];
    if (phys_block == 0) {
      /* Allocate new block */
      phys_block = ext4_alloc_block();
      if (phys_block == 0) {
        pr_err("%s", "Ext4: Block allocation failed\n");
        kfree(block_buf);
        return -1;
      }
      inode.i_block[block_idx] = phys_block;

      /* Calculate 512-byte sectors used */
      /* Ext4 i_blocks is in 512-bit sectors? Yes usually. */
      inode.i_blocks_lo += (4096 / 512);

      /* Clear new block on disk was done in alloc */
    }

    /* Read Block (for partial write) */
    if (block_off != 0 || to_copy != 4096) {
      if (ext4_bread(part_start_lba + (phys_block * 8), 8, block_buf) != 0) {
        kfree(block_buf);
        return -1;
      }
    }

    /* Modify Buffer */
    memcpy(block_buf + block_off, buf + bytes_written, to_copy);

    /* Write Block Back */
    if (ext4_bwrite(part_start_lba + (phys_block * 8), 8, block_buf) != 0) {
      kfree(block_buf);
      return -1;
    }

    bytes_written += to_copy;
    current_offset += to_copy;
  }

  kfree(block_buf);

  /* Update File Size if grew */
  if (current_offset > inode.i_size_lo) {
    inode.i_size_lo = current_offset;
  }

  /* Persist Inode */
  if (ext4_update_inode(ino, &inode) != 0) {
    pr_err("%s", "Ext4: Failed to update inode\n");
    return -1;
  }

  return bytes_written;
}
