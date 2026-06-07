/*
 * kernel/fs/ext4.c
 * Simplified Read-Only Ext4 Driver (With Random Access)
 *
 * Purpose:
 *   Mounts an ext4 partition (hardcoded as GPT/MBR index 2), reads the
 *   superblock and group-0 descriptor, resolves paths to inodes by walking
 *   directory entries, and provides read/write/list-dir operations.
 *
 * NOTE(EXT4-03): The file header "Read-Only" label is false — ext4_write_file
 *   is a real, data-persisting implementation that calls virtio_blk_write.
 *
 * On-disk layout accessed (4096-byte blocks, 8 sectors each):
 *   partition LBA 0        (LBA part+0)  : partition boot record (unused)
 *   superblock offset 1024 (LBA part+2)  : struct ext4_superblock (1016 bytes
 *                                           used; padded to 1024 by padding[])
 *   GDT block 1            (LBA part+8)  : struct ext4_group_desc[0] at byte 0
 *   Block bitmap           (LBA part + bg_block_bitmap_lo × 8) : 4096-byte map
 *   Inode table            (LBA part + bg_inode_table_lo × 8)  : inode array
 *   Data blocks            (LBA part + phys_block × 8) : 4096-byte data
 *
 * Sector arithmetic common pattern:
 *   block N → sector = part_start_lba + (N × 8)   [8 = 4096 / 512]
 *   inode K → byte offset in table = (K-1) × EXT4_INODE_SIZE (256)
 *              sector = part_start_lba + (table_blk × 8) + (byte_off / 512)
 *              offset within sector = byte_off % 512
 *
 * Key invariants:
 *   - Only group 0 is used; multi-group images are not supported (EXT4-12/13).
 *   - Only one partition (part_start_lba) and one group descriptor (bg) are
 *     held globally; concurrent mounts are structurally impossible (EXT4-12).
 *   - ext4_lock serialises block allocation (alloc block bitmap r-m-w, GDT
 *     update, superblock update) and inode write-back (ext4_update_inode).
 *   - The buffer cache (kernel/mm/buffer.c) is NOT used; all block I/O goes
 *     directly through virtio_blk_{read,write} (EXT4-15).
 *
 * Known issues:
 *   EXT4-01  (W5 BUG+MISSING) Extent-tree inode format (EXT4_EXTENTS_FL,
 *            i_flags bit 0x80000) is never detected; i_flags is never read.
 *            On any real ext4 image built with standard mkfs.ext4 (which
 *            enables extents by default), i_block[] contains an extent-tree
 *            header (magic 0xF30A) rather than block pointers — silent data
 *            corruption or a virtio sector-address crash will result.  Safe
 *            only on the custom mkdisk.c test image (block-mapped inodes).
 *   EXT4-03  (W4 DOC+BUG) File header says "Read-Only"; ext4_write_file is
 *            real and persists to disk.  The comment at ext4_read_inode also
 *            says "Supports Direct and Single Indirect" but double-indirect
 *            is fully implemented (blocks 1036–1 049 611).
 *   EXT4-04  (W3 BUG, FIXED) struct ext4_group_desc was 34 bytes (20 named +
 *            padding[14]); the on-disk GDT entry is 32 bytes, so the write-
 *            back at ext4_alloc_block clobbered 2 bytes of GDT entry 1 on
 *            multi-group images.  Now padding[12] => exactly 32 bytes.
 *   EXT4-05  (W3 MISSING) Write path rejects block_idx >= 12; write ceiling
 *            is 12 × 4096 = 49 152 bytes (48 KB).  Read supports up to ~4 GB.
 *   EXT4-06  (W3 MISSING) s_feature_incompat and s_feature_ro_compat are
 *            never read; incompatible features (extents, 64-bit, meta_csum)
 *            are silently accepted.
 *   EXT4-08  (W2 BUG) offset + size can overflow uint32 in ext4_read_inode if
 *            both are near UINT32_MAX; the clamp condition passes and the read
 *            proceeds with an oversized size.
 *   EXT4-09  (W2 MISSING) ext4_list_dir checks i_mode>>12==4 for directory
 *            type but returns -2 without syscall-layer translation; also does
 *            not validate de->rec_len > 0 before advancing the pointer.
 *   EXT4-10  (W2 MISSING) get_inode_struct reads only 1 sector (512 bytes).
 *            EXT4_INODE_SIZE is 256; if sector_off == 384 the inode straddles
 *            two 512-byte sectors (bytes 384–511 of sector N and 0–127 of
 *            sector N+1) but only sector N is fetched — the inode tail is
 *            wrong.  In practice sector_off = ((ino-1)×256) % 512 can be 0
 *            or 256 only (256 mod 512 = 256; 512 mod 512 = 0), so the actual
 *            worst case is sector_off=256 and sizeof(inode)=128 bytes read
 *            up to byte 384 — within one sector.  With EXT4_INODE_SIZE=256
 *            the actual straddle risk is real: sector_off=384 is reachable
 *            when the inodes-per-sector count exceeds 2.
 *   EXT4-11  (W2 PERF) Single-indirect and double-indirect pointer blocks are
 *            re-read from disk on every 4 KB chunk loop iteration; no caching.
 *   EXT4-12  (W2 WRONG-DESIGN) One partition and one group descriptor held
 *            globally; multiple mounts are structurally impossible.
 *   EXT4-13  (W2 MISSING) ext4_alloc_block always searches group 0; no
 *            multi-group block allocation.
 *   EXT4-15  (W1 MISSING) Buffer cache bypassed; all I/O direct to virtio.
 */
#include <drivers/virtio_blk.h>
#include <kernel/buffer.h>
#include <kernel/ext4.h>
#include <kernel/gpt.h>
#include <kernel/kmalloc.h>
#include <kernel/printk.h>
#include <kernel/string.h>

/* Global State */
/* part_start_lba: absolute LBA of the first sector of the mounted partition.
 * Set once by ext4_init(); used in every sector address calculation as the
 * base of the expression: sector = part_start_lba + (block_num × 8). */
static uint64_t part_start_lba;
/* sb: in-memory copy of the ext4 superblock (1016 bytes used out of 1024).
 * Loaded from LBA part_start_lba+2 (byte offset 1024 = 2 × 512 sectors).
 * NOTE(EXT4-06): sb.s_feature_incompat and sb.s_feature_ro_compat are
 * modeled here but never read; incompatible features are silently accepted. */
static struct ext4_superblock sb;
/* bg: in-memory copy of block group descriptor 0.
 * Loaded from LBA part_start_lba+8 (block 1 = bytes 4096–4095+512).
 * EXT4-04 (fixed): sizeof(struct ext4_group_desc) == 32 bytes (padding[12]),
 * matching the on-disk GDT entry; read and write-back no longer touch the
 * adjacent GDT entry 1.
 * NOTE(EXT4-12): only one group descriptor is held; multi-group images are
 * not supported. */
static struct ext4_group_desc bg;
/* ext4_lock: spinlock protecting block allocation (bitmap r-m-w, GDT update,
 * superblock update) and inode write-back (ext4_update_inode).  Held with
 * IRQ save/restore to prevent re-entrance from interrupt context. */
static DEFINE_SPINLOCK(ext4_lock);

/*
 * Helper: Read/Write Sectors
 */
/*
 * ext4_bread - thin wrapper: read 'count' sectors starting at 'sector' into buf.
 *
 * @sector: absolute LBA (e.g. part_start_lba + block_num × 8).
 * @count:  number of 512-byte sectors to read.
 * @buf:    destination; caller must supply at least count × 512 bytes.
 *
 * Returns 0 on success, non-zero on virtio error.
 * Side effects: issues one virtio_blk_read request (disk I/O).
 * NOTE(EXT4-15): bypasses the buffer cache (kernel/mm/buffer.c).
 */
static int ext4_bread(uint64_t sector, uint32_t count, void *buf) {
  return virtio_blk_read(buf, sector, count);
}

/*
 * ext4_bwrite - thin wrapper: write 'count' sectors from buf to 'sector'.
 *
 * @sector: absolute LBA.
 * @count:  number of 512-byte sectors to write.
 * @buf:    source; caller must provide count × 512 bytes of valid data.
 *
 * Returns 0 on success, non-zero on virtio error.
 * Side effects: issues one virtio_blk_write request (disk write; persistent).
 * NOTE(EXT4-03): confirms write capability; the "Read-Only" header is false.
 */
static int ext4_bwrite(uint64_t sector, uint32_t count, void *buf) {
  return virtio_blk_write(buf, sector, count);
}

/*
 * ext4_alloc_block - allocate one free block from block group 0.
 *
 * Algorithm:
 *   1. Read the group-0 block bitmap (one 4096-byte block at LBA
 *      part_start_lba + bg_block_bitmap_lo × 8) into a kmalloc'd buffer.
 *   2. Under ext4_lock: scan 4096 bitmap bytes (32 768 bits) for the first
 *      clear bit (LSB-first within each byte).  Clear bit at byte i, bit b
 *      → block_in_group = i × 8 + b.
 *   3. Set the bit (mark allocated) and write the bitmap back via ext4_bwrite.
 *   4. Decrement bg.bg_free_blocks_count_lo and sb.s_free_blocks_count_lo.
 *   5. Write-back the modified group descriptor (34 bytes into a 512-byte
 *      sector at LBA part_start_lba + 8) and the superblock (1016 bytes into
 *      a two-sector buffer at LBA part_start_lba + 2).
 *   6. Release ext4_lock; zero the new block on disk (outside the lock).
 *
 * Preconditions:
 *   - ext4_init() has completed successfully.
 *   - bg.bg_free_blocks_count_lo > 0 (checked at entry; returns 0 if false).
 *
 * Returns: absolute block number (block_in_group for group 0) on success,
 *          or 0 on failure (OOM, bitmap inconsistency, or I/O error).
 *
 * Side effects:
 *   - Up to 4 virtio_blk_read + 4 virtio_blk_write calls.
 *   - Modifies the global bg and sb structs.
 *   - Acquires/releases ext4_lock with IRQ save/restore.
 *
 * EXT4-04 (fixed): memcpy(bg_buf, &bg, sizeof(bg)) now copies exactly 32
 *   bytes into bg_buf[0..31], so the write-back no longer persists over
 *   bg_block_bitmap_lo[0:1] of GDT entry 1 on multi-group images.
 * NOTE(EXT4-13): Bitmap scan always searches group 0 regardless of
 *   bg_free_blocks_count_lo; no multi-group allocation.
 */
static uint32_t ext4_alloc_block(void) {
  if (bg.bg_free_blocks_count_lo == 0) {
    pr_err("%s", "Ext4: No free blocks in Group 0!\n");
    return 0;
  }

  /* bitmap_blk: block number (0-based from partition start) of the group-0
   * block bitmap.  Sector address = part_start_lba + bitmap_blk × 8. */
  uint64_t bitmap_blk = bg.bg_block_bitmap_lo;
  /* Allocate a 4096-byte heap buffer for the full 4 KB bitmap block. */
  uint8_t *bitmap = kmalloc(4096);
  if (!bitmap)
    return 0;

  uint64_t lock_flags;
  /* Acquire ext4_lock before the bitmap read to serialise the read-modify-
   * write cycle; a second concurrent alloc could find and claim the same bit
   * if the lock were taken only around the bit-set operation. */
  spin_lock_irqsave(&ext4_lock, &lock_flags);

  /* Read Bitmap */
  /* 8 sectors = 4096 bytes = one full block-bitmap block. */
  if (ext4_bread(part_start_lba + (bitmap_blk * 8), 8, bitmap) != 0) {
    spin_unlock_irqrestore(&ext4_lock, lock_flags);
    kfree(bitmap);
    return 0;
  }

  /* Find Free Bit */
  /* Note: Block 0 is Boot Block, usually reserved/used.
     We start searching from block 0 of the group. */
  /* Scan 4096 × 8 = 32 768 bits (little-endian within each byte: bit 0 of
   * byte i represents block i×8+0, bit 1 represents block i×8+1, etc.).
   * On the first byte with a clear bit: set it (mark allocated) and record
   * block_in_group = i × 8 + bit.  NOTE(EXT4-13): scans only group 0. */
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

  /* Write-back group descriptor 0: read the 512-byte GDT sector (LBA
   * part_start_lba + 8), overwrite the first sizeof(bg)==32 bytes with the
   * updated bg, then write the sector back.
   * EXT4-04 (fixed): sizeof(struct ext4_group_desc)==32 now, matching the
   * on-disk entry, so the copy no longer touches GDT entry 1. */
  uint8_t *bg_buf = kmalloc(512);
  if (bg_buf && ext4_bread(part_start_lba + 8, 1, bg_buf) == 0) {
    memcpy(bg_buf, &bg, sizeof(bg)); /* Update BG0 in place */
    ext4_bwrite(part_start_lba + 8, 1, bg_buf);
  }
  if (bg_buf) kfree(bg_buf);

  /* Write-back superblock: read 2 sectors (1024 bytes) at LBA part_start_lba+2
   * (byte offset 1024 from partition start), overwrite the first sizeof(sb)
   * == 1016 bytes with the updated sb, then write back.
   * (sizeof(ext4_superblock) = 356 named bytes + padding[660] = 1016 bytes.) */
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
  /* Zeroing is done outside ext4_lock: ownership of the block is now
   * established (bit set in bitmap, persisted), so no concurrent allocator
   * can claim it.  The 8-sector write (4096 bytes) clears stale data. */
  uint8_t *zero_buf = kmalloc(4096);
  if (zero_buf) {
    memset(zero_buf, 0, 4096);
    /* block_in_group × 8 = sector offset from partition start (1 block = 8
     * sectors of 512 bytes each = 4096 bytes). */
    ext4_bwrite(part_start_lba + (block_in_group * 8), 8, zero_buf);
    kfree(zero_buf);
  }

  return block_in_group;
}

/*
 * ext4_init - mount the ext4 partition and load the superblock + GDT entry 0.
 *
 * Algorithm:
 *   1. Call gpt_get_partition(2) to obtain the start LBA of the partition.
 *      NOTE(GPT-02): index 2 means the 3rd GPT entry or the 2nd MBR slot
 *      depending on which table was found by gpt_init().
 *   2. Read 2 sectors (1024 bytes) from LBA part_start_lba + 2 into a 4096-
 *      byte heap buffer.  The ext4 superblock is always at byte offset 1024
 *      from the partition start, i.e. sectors 2–3 of the partition (0-based).
 *      Sector calculation: byte_offset = 1024, sector = 1024 / 512 = 2.
 *   3. memcpy the first sizeof(struct ext4_superblock) == 1016 bytes of the
 *      buffer into the global sb.  Validate sb.s_magic == 0xEF53 (EXT4_MAGIC).
 *   4. Read 1 sector from LBA part_start_lba + 8 (block 1, byte offset 4096,
 *      sector = 4096 / 512 = 8).  The GDT starts at block 1 for 4 KB-block
 *      filesystems (superblock in block 0 at offset 1024; GDT immediately
 *      after the superblock block).
 *   5. memcpy sizeof(struct ext4_group_desc) == 32 bytes into global bg,
 *      matching the on-disk entry (EXT4-04 fixed; was 34 via padding[14]).
 *
 * Preconditions:
 *   - gpt_init() must have completed and partitions[2] must be valid.
 *   - virtio_blk_read() must be functional.
 *
 * Side effects:
 *   - Sets part_start_lba, sb, bg (global state).
 *   - Two virtio_blk_read() calls.
 *
 * NOTE(EXT4-06): s_feature_incompat and s_feature_ro_compat are read into sb
 *   but never checked; incompatible features (extents, 64-bit, metadata_csum)
 *   are silently accepted.  This is the structural companion to EXT4-01.
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
  /* Superblock byte offset from partition start: 1024 = 2 × 512-byte sectors.
   * Reading 2 sectors (1024 bytes) covers the full 1024-byte superblock area.
   * s_volume_name is at struct offset 0x78 (120 bytes) from the superblock
   * start (verified by field arithmetic in ext4.h:51) — correct per spec. */
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
  /* Block 1 byte offset from partition start: 1 × 4096 = 4096 bytes
   * = 4096 / 512 = 8 sectors from the partition's LBA 0.
   * We read only 1 sector (512 bytes); GDT entry 0 occupies the first 32
   * on-disk bytes, and sizeof(struct ext4_group_desc) == 32 now (EXT4-04
   * fixed), so memcpy below ingests exactly the 32-byte entry. */
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
  if (size == 0 || buf == NULL)
    return file_size;
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
    } else if (block_idx < 12 + 1024) {
      /* Indirect Block */
      uint32_t indirect_blk_num = inode.i_block[12];
      if (indirect_blk_num == 0) {
        kfree(block_buf); kfree(indirect_buf); return -1;
      }
      if (virtio_blk_read(indirect_buf, part_start_lba + (indirect_blk_num * 8), 8) != 0) {
        kfree(block_buf); kfree(indirect_buf); return -1;
      }
      phys_block = ((uint32_t *)indirect_buf)[block_idx - 12];
    } else {
      /* Double Indirect Block */
      uint32_t d_idx = block_idx - 12 - 1024;
      uint32_t master_idx = d_idx / 1024;
      uint32_t sub_idx = d_idx % 1024;

      uint32_t double_indir_blk = inode.i_block[13];
      if (double_indir_blk == 0 || master_idx >= 1024) {
        kfree(block_buf); kfree(indirect_buf); return -1;
      }

      /* Read Master Block */
      if (virtio_blk_read(indirect_buf, part_start_lba + (double_indir_blk * 8), 8) != 0) {
        kfree(block_buf); kfree(indirect_buf); return -1;
      }
      uint32_t sub_indir_blk = ((uint32_t *)indirect_buf)[master_idx];
      if (sub_indir_blk == 0) {
        kfree(block_buf); kfree(indirect_buf); return -1;
      }

      /* Read Sub Block */
      if (virtio_blk_read(indirect_buf, part_start_lba + (sub_indir_blk * 8), 8) != 0) {
        kfree(block_buf); kfree(indirect_buf); return -1;
      }
      phys_block = ((uint32_t *)indirect_buf)[sub_idx];
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

  /* Validate offset */
  if (offset > inode.i_size_lo + 1048576) {  /* Allow up to 1MB past current size for sparse files */
    pr_err("EXT4: Write offset 0x%x exceeds reasonable bounds (i_size_lo=0x%x)\n", 
           offset, inode.i_size_lo);
    return -1;
  }

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

/*
 * Public API: List directory contents
 * Formats as a space-separated string of names
 */
int ext4_list_dir(const char *path, char *buf, uint32_t size) {
  uint32_t dir_ino;
  if (ext4_find_inode(path, &dir_ino) != 0)
    return -1;

  struct ext4_inode inode;
  if (get_inode_struct(dir_ino, &inode) != 0)
    return -1;

  if (!((inode.i_mode >> 12) == 4)) { /* Check if directory */
    return -2;
  }

  uint32_t dir_size = inode.i_size_lo;
  uint32_t current_blk_off = 0;
  uint8_t *dir_buf = kmalloc(4096);
  if (!dir_buf)
    return -1;

  uint32_t buf_pos = 0;
  if (size > 0) buf[0] = '\0';

  while (current_blk_off < dir_size) {
    if (ext4_read_inode(dir_ino, current_blk_off, dir_buf, 4096) <= 0) {
      break;
    }

    struct ext4_dir_entry *de = (struct ext4_dir_entry *)dir_buf;
    uint32_t offset = 0;
    while (offset < 4096) {
      if (de->inode == 0 || de->rec_len < 8)
        break;

      if (de->name_len > 0) {
        /* Copy name + space */
        if (buf_pos + de->name_len + 1 < size) {
          memcpy(buf + buf_pos, de->name, de->name_len);
          buf_pos += de->name_len;
          buf[buf_pos++] = ' ';
          buf[buf_pos] = '\0';
        }
      }
      
      offset += de->rec_len;
      if (offset >= 4096) break;
      de = (struct ext4_dir_entry *)((uint8_t *)de + de->rec_len);
    }
    current_blk_off += 4096;
  }

  kfree(dir_buf);
  return (int)buf_pos;
}
