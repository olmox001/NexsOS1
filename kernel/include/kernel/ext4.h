/*
 * kernel/include/kernel/ext4.h
 * Simplified Ext4 Filesystem Definitions
 *
 * The driver is a VFS provider (see <kernel/vfs.h>): the only kernel-facing
 * symbol is ext4_fs_ops, registered from the composition root (main.c).
 * Everything else here is on-disk format shared with tools/mkdisk.c — if you
 * change a struct here, mkdisk.c's mirrored copy must change identically or
 * the two will silently disagree about layout.
 *
 * ---------------------------------------------------------------------
 * Revision note (this pass): reordered for readability (constants, then
 * on-disk structures in the order they appear on disk, then the runtime
 * contract), commented every structure and constant, added compile-time
 * layout guarantees (_Static_assert) for every packed on-disk structure,
 * and fixed one real bug found while doing so:
 *
 *   EXT4-SB-SIZE-01 (FIXED): struct ext4_superblock's tail padding was
 *   computed as `1024 - 364`, but the fields preceding it only sum to
 *   356 bytes, not 364. The struct was therefore 1016 bytes, not the
 *   1024 (2 sectors) every read/write site assumes. This was silently
 *   harmless as long as every superblock access went through a
 *   read-modify-write of a pre-existing on-disk image (the missing 8
 *   bytes at the tail were never touched, just never modeled) — but it
 *   is exactly the kind of latent bug that turns into real corruption
 *   the moment any code writes a freshly allocated buffer without first
 *   reading it back (see ext4_sync_bg_sb in kernel/fs/ext4.c, rewritten
 *   in this pass to skip that now-provably-unnecessary read for
 *   performance). Padding is now `1024 - 356`, and a _Static_assert
 *   pins the total at exactly 1024 bytes so this class of bug cannot
 *   silently recur.
 * ---------------------------------------------------------------------
 */
#ifndef _KERNEL_EXT4_H
#define _KERNEL_EXT4_H

#include <kernel/types.h>
#include <kernel/vfs.h>

/* ------------------------------------------------------------------ */
/* On-disk geometry constants                                          */
/* ------------------------------------------------------------------ */

#define EXT4_SUPERBLOCK_OFFSET 1024 /* byte offset of the superblock */
#define EXT4_MAGIC 0xEF53
#define EXT4_BLOCK_SIZE 4096
#define EXT4_SECTORS_PER_BLOCK 8 /* 4096 / 512 */
#define EXT4_INODE_SIZE 256      /* rev1+ default; rev0 images use 128 */
#define EXT4_ROOT_INO 2

/* Hard ceiling on the number of block groups this (single-block-group-
 * oriented) driver and tools/mkdisk.c will ever address. One 4 KiB GDT
 * block holds exactly EXT4_MAX_GROUPS descriptors (128 * 32 = 4096), so
 * the whole group-descriptor table always fits in one block with no
 * remainder — see ext4_sync_bg_sb(). Multi-group images larger than
 * this are refused at mount (EXT4-GROW-01: runtime partition resize and
 * true multi-group allocation are out of scope for this driver). */
#define EXT4_MAX_GROUPS 128

/* Incompat features (s_feature_incompat). Mounting an image with any bit
 * outside EXT4_INCOMPAT_SUPPORTED must FAIL LOUDLY (EXT4-06). */
#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x0002
#define EXT4_FEATURE_INCOMPAT_RECOVER 0x0004 /* dirty journal: refuse */
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT 0x0080 /* 64-byte GDT entries: refuse */
#define EXT4_FEATURE_INCOMPAT_FLEX_BG 0x0200
#define EXT4_INCOMPAT_SUPPORTED                                                \
  (EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS |            \
   EXT4_FEATURE_INCOMPAT_FLEX_BG)

/* RO-compat features (s_feature_ro_compat). Bits outside this set force a
 * read-only mount: our writer maintains no checksums/metadata for them. */
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE 0x0002
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK 0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE 0x0040
#define EXT4_RO_COMPAT_WRITE_SAFE                                              \
  (EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT4_FEATURE_RO_COMPAT_LARGE_FILE |   \
   EXT4_FEATURE_RO_COMPAT_DIR_NLINK | EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE)

/* Inode flag: i_block[] holds an extent tree, not block pointers (EXT4-01) */
#define EXT4_EXTENTS_FL 0x00080000

/* ------------------------------------------------------------------ */
/* On-disk structures, in on-disk order                                 */
/* ------------------------------------------------------------------ */

/* Simplified Ext4 Superblock (occupies bytes 1024..2047 of the partition,
 * i.e. sectors part_start+2 and part_start+3). Only the fields this driver
 * actually reads or writes are named individually; everything else is
 * accounted for by the trailing `padding` so the struct's total size
 * matches the real on-disk layout exactly (see _Static_assert below). */
struct ext4_superblock {
  uint32_t s_inodes_count;
  uint32_t s_blocks_count_lo;
  uint32_t s_r_blocks_count_lo; /* reserved blocks */
  uint32_t s_free_blocks_count_lo;
  uint32_t s_free_inodes_count;
  uint32_t s_first_data_block;
  uint32_t s_log_block_size; /* 0=1KB, 1=2KB, 2=4KB (only 2 is supported) */
  uint32_t s_log_cluster_size;
  uint32_t s_blocks_per_group;
  uint32_t s_clusters_per_group;
  uint32_t s_inodes_per_group;
  uint32_t s_mtime;
  uint32_t s_wtime;
  uint16_t s_mnt_count;
  uint16_t s_max_mnt_count;
  uint16_t s_magic; /* must equal EXT4_MAGIC */
  uint16_t s_state;
  uint16_t s_errors;
  uint16_t s_minor_rev_level;
  uint32_t s_lastcheck;
  uint32_t s_checkinterval;
  uint32_t s_creator_os;
  uint32_t s_rev_level; /* 0 => inode_size fixed at 128, ignore s_inode_size */
  uint16_t s_def_resuid;
  uint16_t s_def_resgid;
  uint32_t s_first_ino;
  uint16_t s_inode_size; /* on-disk inode record stride, rev1+ only */
  uint16_t s_block_group_nr;
  uint32_t s_feature_compat;
  uint32_t s_feature_incompat;
  uint32_t s_feature_ro_compat;
  uint8_t s_uuid[16];
  char s_volume_name[16];
  char s_last_mounted[64];
  uint32_t s_algorithm_usage_bitmap;
  uint8_t s_prealloc_blocks;
  uint8_t s_prealloc_dir_blocks;
  uint16_t s_reserved_gdt_blocks;
  uint8_t s_journal_uuid[16];
  uint32_t s_journal_inum;
  uint32_t s_journal_dev;
  uint32_t s_last_orphan;
  uint32_t s_hash_seed[4];
  uint8_t s_def_hash_version;
  uint8_t s_jnl_backup_type;
  uint16_t s_desc_size;
  uint32_t s_default_mount_opts;
  uint32_t s_first_meta_bg;
  uint32_t s_mkfs_time;
  uint32_t s_jnl_blocks[17];
  uint32_t s_blocks_count_hi;
  uint32_t s_r_blocks_count_hi;
  uint32_t s_free_blocks_count_hi;
  uint16_t s_min_extra_isize;
  uint16_t s_want_extra_isize;
  uint32_t s_flags;
  /* Named fields above sum to exactly 356 bytes; pad to the true 1024-byte
   * on-disk superblock size (EXT4-SB-SIZE-01, see file header note). */
  uint8_t padding[1024 - 356];
} __attribute__((packed));
_Static_assert(sizeof(struct ext4_superblock) == 1024,
               "ext4_superblock must be exactly 1024 bytes (2 sectors) -- "
               "on-disk layout contract with tools/mkdisk.c");

/* Group Descriptor (32-bit simplified; one entry per block group). The GDT
 * lives at block 1 (sectors part_start+8..+15) and holds up to
 * EXT4_MAX_GROUPS of these back to back, filling the block exactly. */
struct ext4_group_desc {
  uint32_t bg_block_bitmap_lo;
  uint32_t bg_inode_bitmap_lo;
  uint32_t bg_inode_table_lo;
  uint16_t bg_free_blocks_count_lo;
  uint16_t bg_free_inodes_count_lo;
  uint16_t bg_used_dirs_count_lo;
  uint16_t bg_flags;
  uint8_t padding[12]; /* 20 named bytes + 12 padding = 32-byte GDT entry */
} __attribute__((packed));
_Static_assert(sizeof(struct ext4_group_desc) == 32,
               "ext4_group_desc must be exactly 32 bytes -- on-disk GDT "
               "entry size contract with tools/mkdisk.c");
_Static_assert(EXT4_MAX_GROUPS * sizeof(struct ext4_group_desc) ==
                   EXT4_BLOCK_SIZE,
               "the GDT must fill exactly one block with no remainder -- "
               "ext4_sync_bg_sb relies on this to write the GDT as a single "
               "block with no partial-block tail to preserve");

/* On-disk inode record (the first sizeof(struct ext4_inode) == 128 bytes of
 * an s_inode_size-wide slot; this simplified driver never uses the "extra"
 * space rev1+ formats reserve beyond 128 bytes for nanosecond timestamps or
 * inline extended attributes). */
struct ext4_inode {
  uint16_t i_mode;
  uint16_t i_uid;
  uint32_t i_size_lo;
  uint32_t i_atime;
  uint32_t i_ctime;
  uint32_t i_mtime;
  uint32_t i_dtime;
  uint16_t i_gid;
  uint16_t i_links_count;
  uint32_t i_blocks_lo; /* count of 512-byte sectors owned by this file,
                         * including indirect/extent metadata blocks */
  uint32_t i_flags;     /* EXT4_EXTENTS_FL and friends */
  uint32_t i_osd1;
  uint32_t i_block[15]; /* legacy pointers OR an inline extent tree root */
  uint32_t i_generation;
  uint32_t i_file_acl_lo;
  uint32_t i_size_high;
  uint32_t i_obso_faddr;
  uint8_t pad[12];
} __attribute__((packed));
_Static_assert(sizeof(struct ext4_inode) == 128,
               "ext4_inode must be exactly 128 bytes -- get_inode_struct's "
               "sector-straddle-free guarantee (kernel/fs/ext4.c) depends "
               "on every on-disk inode record stride being a multiple of "
               "128, which in turn depends on this struct being 128 bytes");

/* Directory Entry 2 (variable length: 8-byte header + name, no NUL
 * terminator, rec_len rounded up to a 4-byte boundary). A live entry has
 * inode != 0; ext4_dir_remove() clears inode to 0 to free the slot for
 * ext4_dir_insert() to reclaim, WITHOUT touching rec_len -- readers must
 * skip inode==0 slots by rec_len rather than treating them as end-of-block
 * (see kernel/fs/ext4.c, EXT4-DIRENT-01). */
struct ext4_dir_entry {
  uint32_t inode; /* 0 == freed slot, not end-of-block */
  uint16_t rec_len;
  uint8_t name_len;
  uint8_t file_type;
  char name[]; /* name_len bytes, no NUL terminator */
} __attribute__((packed));
_Static_assert(sizeof(struct ext4_dir_entry) == 8,
               "ext4_dir_entry's fixed header must be exactly 8 bytes -- "
               "dirent_ideal_len()'s '(8 + name_len + 3) & ~3' formula and "
               "every rec_len >= 8 sanity check assume this");

/* Extent tree (lives in i_block[] when EXT4_EXTENTS_FL is set).
 * A node is: one ext4_extent_header followed by eh_entries records --
 * ext4_extent_idx if eh_depth > 0 (interior node), ext4_extent if
 * eh_depth == 0 (leaf). The root node sits in the 60 bytes of i_block[]
 * (12-byte header + up to 4 12-byte records); non-root nodes fill a
 * 4096-byte block (12-byte header + up to 340 12-byte records). The
 * on-disk format caps eh_depth at 5; this driver only ever CREATES trees
 * of depth 0 or 1 (see ext4_extent_grow in kernel/fs/ext4.c) but must be
 * able to READ deeper trees produced by another implementation. */
#define EXT4_EXT_MAGIC 0xF30A
/* ee_len above this marks an unwritten (preallocated) extent: the blocks
 * are mapped but must read as zeros; actual length = ee_len - 32768. */
#define EXT4_EXT_UNWRITTEN_LEN 32768

struct ext4_extent_header {
  uint16_t eh_magic; /* must equal EXT4_EXT_MAGIC */
  uint16_t eh_entries;
  uint16_t eh_max;
  uint16_t eh_depth; /* 0 = leaf (eh+1 is ext4_extent[]); >0 = interior
                      * (eh+1 is ext4_extent_idx[]) */
  uint32_t eh_generation;
} __attribute__((packed));
_Static_assert(sizeof(struct ext4_extent_header) == 12,
               "ext4_extent_header must be exactly 12 bytes -- the 4-record "
               "root capacity (60-byte i_block[]) and the 340-record leaf "
               "capacity ((4096-12)/12) both depend on this");

struct ext4_extent_idx {
  uint32_t ei_block;   /* first logical block covered by the child */
  uint32_t ei_leaf_lo; /* child node's physical block, low 32 bits */
  uint16_t ei_leaf_hi; /* child node's physical block, high 16 bits */
  uint16_t ei_unused;
} __attribute__((packed));
_Static_assert(sizeof(struct ext4_extent_idx) == 12,
               "ext4_extent_idx must be exactly 12 bytes (same record size "
               "as ext4_extent -- code that computes eh_max once and reuses "
               "it for either record type depends on this)");

struct ext4_extent {
  uint32_t ee_block;    /* first logical block */
  uint16_t ee_len;      /* count (see EXT4_EXT_UNWRITTEN_LEN) */
  uint16_t ee_start_hi; /* physical block, high 16 bits */
  uint32_t ee_start_lo; /* physical block, low 32 bits */
} __attribute__((packed));
_Static_assert(sizeof(struct ext4_extent) == 12,
               "ext4_extent must be exactly 12 bytes -- see ext4_extent_idx "
               "assertion above");

/* File Types (ext4_dir_entry.file_type) */
#define EXT4_FT_UNKNOWN 0
#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR 2

/* ------------------------------------------------------------------ */
/* Runtime contract                                                     */
/* ------------------------------------------------------------------ */

/* The VFS provider. Register with vfs_register_fs() before vfs_init(). */
extern const struct fs_ops ext4_fs_ops;

#endif /* _KERNEL_EXT4_H */