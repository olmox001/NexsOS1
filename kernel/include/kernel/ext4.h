/*
 * kernel/include/kernel/ext4.h
 * Simplified Ext4 Filesystem Definitions
 */
#ifndef _KERNEL_EXT4_H
#define _KERNEL_EXT4_H

#include <kernel/types.h>

#define EXT4_SUPERBLOCK_OFFSET 1024
#define EXT4_MAGIC 0xEF53
#define EXT4_BLOCK_SIZE 4096
#define EXT4_SECTORS_PER_BLOCK 8 /* 4096 / 512 */
#define EXT4_INODE_SIZE 256
#define EXT4_ROOT_INO 2

/* Simplified Ext4 Superblock */
struct ext4_superblock {
  uint32_t s_inodes_count;
  uint32_t s_blocks_count_lo;
  uint32_t s_r_blocks_count_lo; /* Reserved blocks */
  uint32_t s_free_blocks_count_lo;
  uint32_t s_free_inodes_count;
  uint32_t s_first_data_block;
  uint32_t s_log_block_size; /* 0=1KB, 1=2KB, 2=4KB */
  uint32_t s_log_cluster_size;
  uint32_t s_blocks_per_group;
  uint32_t s_clusters_per_group;
  uint32_t s_inodes_per_group;
  uint32_t s_mtime;
  uint32_t s_wtime;
  uint16_t s_mnt_count;
  uint16_t s_max_mnt_count;
  uint16_t s_magic;
  uint16_t s_state;
  uint16_t s_errors;
  uint16_t s_minor_rev_level;
  uint32_t s_lastcheck;
  uint32_t s_checkinterval;
  uint32_t s_creator_os;
  uint32_t s_rev_level;
  uint16_t s_def_resuid;
  uint16_t s_def_resgid;
  uint32_t s_first_ino;
  uint16_t s_inode_size;
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
  /* ... padding to 1024 bytes ... */
  uint8_t padding[1024 - 364];
} __attribute__((packed));

/* Group Descriptor (32-bit simplified) */
struct ext4_group_desc {
  uint32_t bg_block_bitmap_lo;
  uint32_t bg_inode_bitmap_lo;
  uint32_t bg_inode_table_lo;
  uint16_t bg_free_blocks_count_lo;
  uint16_t bg_free_inodes_count_lo;
  uint16_t bg_used_dirs_count_lo;
  uint16_t bg_flags;
  uint8_t padding[12]; /* EXT4-04: 20 named + 12 = 32 bytes, the on-disk GDT entry size */
} __attribute__((packed));

/* Inode */
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
  uint32_t i_blocks_lo;
  uint32_t i_flags;
  uint32_t i_osd1;
  uint32_t i_block[15]; /* Pointers */
  uint32_t i_generation;
  uint32_t i_file_acl_lo;
  uint32_t i_size_high;
  uint32_t i_obso_faddr;
  uint8_t pad[12];
} __attribute__((packed));

/* Directory Entry 2 */
struct ext4_dir_entry {
  uint32_t inode;
  uint16_t rec_len;
  uint8_t name_len;
  uint8_t file_type;
  char name[];
} __attribute__((packed));

/* File Types */
#define EXT4_FT_UNKNOWN 0
#define EXT4_FT_REG_FILE 1
#define EXT4_FT_DIR 2

/* API */
void ext4_init(void);

/* Search (simplified: single level /filename) */
int ext4_find_inode(const char *path, uint32_t *ino_out);

/* Read Inode Data (Random Access) */
/* buf: destination buffer
   ino: inode number
   offset: byte offset in file
   size: bytes to read */
int ext4_read_inode(uint32_t ino, uint32_t offset, uint8_t *buf, uint32_t size);

/* Legacy wrapper (reads from count 0) */
int ext4_read_file(const char *path, uint8_t *buf, uint32_t size,
                   uint32_t offset);

/* Write Data to File (Overwrite/Append) */
int ext4_write_file(const char *path, const uint8_t *buf, uint32_t size,
                    uint32_t offset);

/* List directory contents */
int ext4_list_dir(const char *path, char *buf, uint32_t size);

#endif /* _KERNEL_EXT4_H */
