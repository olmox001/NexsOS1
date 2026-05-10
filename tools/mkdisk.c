/*
 * tools/mkdisk.c
 * Host tool to generate a GPT-partitioned disk image
 * Hardened and Standardized for Determinism
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SECTOR_SIZE 512
#define DISK_SIZE_MB 128
#define DISK_SIZE_BYTES (DISK_SIZE_MB * 1024 * 1024)
#define NUM_SECTORS (DISK_SIZE_BYTES / SECTOR_SIZE)

/* GPT Constants */
#define GPT_SIGNATURE 0x5452415020494645ULL
#define GPT_REVISION 0x00010000

/* Ext4 Constants */
#define EXT4_SUPERBLOCK_OFFSET 1024
#define EXT4_MAGIC 0xEF53
#define EXT4_BLOCK_SIZE 4096
#define EXT4_SECTORS_PER_BLOCK (EXT4_BLOCK_SIZE / SECTOR_SIZE)
#define EXT4_INODE_SIZE 256
#define EXT4_ROOT_INO 2

/* Layout Calculation */
/* Block 0: Boot + Superblock
 * Block 1: Group Descriptors
 * Block 2: Block Bitmap
 * Block 3: Inode Bitmap
 * Block 4..67: Inode Table (1024 inodes * 256 bytes = 256KB = 64 Blocks)
 * Block 68+: Data
 */
#define BLK_GRP_DESC 1
#define BLK_BLK_BITMAP 2
#define BLK_INODE_BITMAP 3
#define BLK_INODE_TABLE 4
#define INODE_TABLE_BLOCKS 64
#define BLK_DATA_START (BLK_INODE_TABLE + INODE_TABLE_BLOCKS)

/* Simplified GUID structure */
struct guid {
  uint32_t data1;
  uint16_t data2;
  uint16_t data3;
  uint8_t data4[8];
};

/* Basic GUIDs */
struct guid TYPE_BOOT = {
    0x21686148,
    0x6449,
    0x6E6F,
    {0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49}}; /* BIOS Boot */
struct guid TYPE_KERNEL = {
    0x0FC63DAF,
    0x8483,
    0x4772,
    {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}}; /* Linux Filesystem */
struct guid TYPE_DATA = {
    0x0FC63DAF,
    0x8483,
    0x4772,
    {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}}; /* Linux Filesystem */

struct gpt_header {
  uint64_t signature;
  uint32_t revision;
  uint32_t header_size;
  uint32_t header_crc32;
  uint32_t reserved1;
  uint64_t my_lba;
  uint64_t alternate_lba;
  uint64_t first_usable_lba;
  uint64_t last_usable_lba;
  struct guid disk_guid;
  uint64_t partition_entry_lba;
  uint32_t num_partition_entries;
  uint32_t partition_entry_size;
  uint32_t partition_entry_crc32;
} __attribute__((packed));

struct gpt_partition_entry {
  struct guid type_guid;
  struct guid unique_guid;
  uint64_t start_lba;
  uint64_t end_lba;
  uint64_t attributes;
  uint16_t partition_name[36];
} __attribute__((packed));

struct mbr_entry {
  uint8_t status;
  uint8_t chs_start[3];
  uint8_t type;
  uint8_t chs_end[3];
  uint32_t lba_start;
  uint32_t sectors;
} __attribute__((packed));

/* Simplified Ext4 Structures */
struct ext4_superblock {
  uint32_t s_inodes_count;
  uint32_t s_blocks_count_lo;
  uint32_t s_r_blocks_count_lo;
  uint32_t s_free_blocks_count_lo;
  uint32_t s_free_inodes_count;
  uint32_t s_first_data_block;
  uint32_t s_log_block_size;
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
  uint8_t padding[1024 - 364];
} __attribute__((packed));

struct ext4_group_desc {
  uint32_t bg_block_bitmap_lo;
  uint32_t bg_inode_bitmap_lo;
  uint32_t bg_inode_table_lo;
  uint16_t bg_free_blocks_count_lo;
  uint16_t bg_free_inodes_count_lo;
  uint16_t bg_used_dirs_count_lo;
  uint16_t bg_flags;
  uint8_t padding[14];
} __attribute__((packed));

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
  uint32_t i_block[15];
  uint8_t padding[256 - 102];
} __attribute__((packed));

struct ext4_dir_entry {
  uint32_t inode;
  uint16_t rec_len;
  uint8_t name_len;
  uint8_t file_type;
  char name[];
} __attribute__((packed));

/* Globals for Bitmaps */
static uint8_t *block_bitmap = NULL;
static uint8_t *inode_bitmap = NULL;
static uint32_t next_free_block = BLK_DATA_START;
static uint32_t current_free_inode = 11;
static uint32_t total_blocks = 0;
static uint32_t free_blocks_count = 0;
static uint32_t free_inodes_count = 1014; /* 1024 - 10 reserved */

/* CRC32 Implementation */
uint32_t crc32(const void *data, size_t n_bytes) {
  uint32_t crc = 0xFFFFFFFF;
  const uint8_t *p = data;
  for (size_t i = 0; i < n_bytes; i++) {
    crc ^= p[i];
    for (int j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
  }
  return ~crc;
}

/* Hardened File I/O Checkers */
void xseek(FILE *f, long offset, int whence) {
  if (fseek(f, offset, whence) != 0) {
    perror("fseek");
    exit(1);
  }
}

void xwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
  if (fwrite(ptr, size, nmemb, stream) != nmemb) {
    perror("fwrite");
    exit(1);
  }
}

void *xmalloc(size_t size) {
  void *p = malloc(size);
  if (!p) {
    perror("malloc");
    exit(1);
  }
  memset(p, 0, size);
  return p;
}

void mark_block_used(uint32_t block) {
  if (block >= 8192) {
    fprintf(stderr, "Error: Block %d out of range (max 8192)\n", block);
    exit(1);
  }
  int byte = block / 8;
  int bit = block % 8;
  block_bitmap[byte] |= (1 << bit);
  free_blocks_count--;
}

void mark_inode_used(uint32_t inode) {
  // Inodes are 1-indexed. Bit 0 is Inode 1.
  if (inode < 1 || inode > 1024) {
    fprintf(stderr, "Error: Inode %d out of range\n", inode);
    exit(1);
  }
  int byte = (inode - 1) / 8;
  int bit = (inode - 1) % 8;
  inode_bitmap[byte] |= (1 << bit);
  free_inodes_count--;
}

/* Helper: Add a file to the disk image */
void write_file_to_inode(FILE *f, uint64_t partition_offset_bytes,
                         uint32_t inode_num, const char *src_path);

void add_file_to_ext4(FILE *f, const char *src_path, const char *dest_name,
                      uint64_t partition_offset_bytes) {
  if (access(src_path, F_OK) == -1) {
    printf("Skipping %s (not found)\n", src_path);
    return;
  }
  printf("Adding %s to Ext4 as %s (Inode %d)...\n", src_path, dest_name,
         current_free_inode);

  write_file_to_inode(f, partition_offset_bytes, current_free_inode, src_path);
  mark_inode_used(current_free_inode);
  current_free_inode++;
}

void write_raw_to_partition(FILE *f, uint64_t start_lba, const char *src_path) {
  if (!src_path || strlen(src_path) == 0)
    return;

  FILE *src = fopen(src_path, "rb");
  if (!src) {
    printf("Warning: Could not open %s for raw writing\n", src_path);
    return;
  }

  fseek(src, 0, SEEK_END);
  long size = ftell(src);
  rewind(src);

  uint8_t *buf = xmalloc(size);
  if (fread(buf, 1, size, src) != (size_t)size) {
    perror("fread");
    exit(1);
  }
  fclose(src);

  xseek(f, start_lba * SECTOR_SIZE, SEEK_SET);
  xwrite(buf, 1, size, f);
  free(buf);

  printf("Ext4: Wrote %s to LBA %llu (%ld bytes)\n", src_path,
         (unsigned long long)start_lba, size);
}

void write_file_to_inode(FILE *f, uint64_t partition_offset_bytes,
                         uint32_t inode_num, const char *src_path) {
  uint64_t inode_offset = partition_offset_bytes + 4LL * EXT4_BLOCK_SIZE +
                          (uint64_t)(inode_num - 1) * EXT4_INODE_SIZE;

  struct ext4_inode file_inode = {0};
  file_inode.i_mode = 0x81C0; /* File | 644 */
  file_inode.i_links_count = 1;

  FILE *src = fopen(src_path, "rb");
  /* Src must exist checked by caller, but safety first */
  if (src) {
    fseek(src, 0, SEEK_END);
    long src_size = ftell(src);
    rewind(src);
    uint8_t *buf = xmalloc(src_size);
    if (fread(buf, 1, src_size, src) != (size_t)src_size) {
      perror("fread");
      exit(1);
    }
    fclose(src);

    file_inode.i_size_lo = src_size;
    int data_blocks = (src_size + EXT4_BLOCK_SIZE - 1) / EXT4_BLOCK_SIZE;
    int blocks_used = data_blocks;

    uint32_t indir_block = 0;
    if (data_blocks > 12) {
      indir_block = next_free_block + data_blocks;
      mark_block_used(indir_block);
      blocks_used++;
    }

    /* 512-byte sectors count for i_blocks */
    file_inode.i_blocks_lo = blocks_used * (EXT4_BLOCK_SIZE / 512);

    for (int i = 0; i < data_blocks && i < 12; i++) {
      file_inode.i_block[i] = next_free_block + i;
      mark_block_used(next_free_block + i);
    }

    if (data_blocks > 12) {
      file_inode.i_block[12] = indir_block;
      uint32_t *indirect_buf = (uint32_t *)xmalloc(EXT4_BLOCK_SIZE);
      for (int i = 12; i < data_blocks; i++) {
        indirect_buf[i - 12] = next_free_block + i;
        mark_block_used(next_free_block + i);
      }
      xseek(f, partition_offset_bytes + (uint64_t)indir_block * EXT4_BLOCK_SIZE,
            SEEK_SET);
      xwrite(indirect_buf, 1, EXT4_BLOCK_SIZE, f);
      free(indirect_buf);
    }

    xseek(f, inode_offset, SEEK_SET);
    xwrite(&file_inode, 1, sizeof(file_inode), f);

    xseek(f,
          partition_offset_bytes + (uint64_t)next_free_block * EXT4_BLOCK_SIZE,
          SEEK_SET);
    xwrite(buf, 1, src_size, f);
    free(buf);
    printf("Ext4: Added %s (Ino %d, %ld bytes, %d blocks)\n", src_path,
           inode_num, src_size, blocks_used);

    /* Advance Allocator to account for Data blocks AND Indirect block if used
     */
    next_free_block += blocks_used;
  }
}

void write_ext4_partition(FILE *f, uint64_t start_lba, uint64_t size_sectors) {
  uint64_t start_offset = start_lba * SECTOR_SIZE;
  uint64_t size_bytes = size_sectors * SECTOR_SIZE;
  total_blocks = size_bytes / EXT4_BLOCK_SIZE;
  free_blocks_count = total_blocks;

  /* Initialize Bitmaps */
  block_bitmap = (uint8_t *)xmalloc(EXT4_BLOCK_SIZE);
  inode_bitmap = (uint8_t *)xmalloc(EXT4_BLOCK_SIZE);

  printf("Ext4: Formatting partition at LBA %llu (Size: %d MB)\n",
         (unsigned long long)start_lba, (int)(size_bytes >> 20));

  /* Mark Metadata Blocks as used */
  /* Block 0 (SB), 1 (GDT), 2 (BMap), 3 (IMap) */
  for (int i = 0; i < 4; i++)
    mark_block_used(i);
  /* Mark Inode Table Blocks (4 to 67) */
  for (int i = 0; i < INODE_TABLE_BLOCKS; i++)
    mark_block_used(BLK_INODE_TABLE + i);

  /* Mark Reserved Inodes (1-10) */
  for (int i = 1; i <= 10; i++)
    mark_inode_used(i);

  /* Allocator start after ITABLE */
  next_free_block = BLK_DATA_START;

  /* Prepare Directory Data Buffer */
  uint8_t *dir_blk = (uint8_t *)xmalloc(EXT4_BLOCK_SIZE);
  int off = 0;

  /* ---------------- ROOT DIR (Inode 2) ---------------- */
  mark_inode_used(EXT4_ROOT_INO);
  /* Root Dir Data at next_free_block */
  uint32_t root_data_block = next_free_block++;
  mark_block_used(root_data_block);

  /* "." */
  struct ext4_dir_entry *de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = 2;
  de->rec_len = 12;
  de->name_len = 1;
  de->file_type = 2;
  memcpy(de->name, ".", 1);
  off += de->rec_len;
  /* ".." */
  de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = 2;
  de->rec_len = 12;
  de->name_len = 2;
  de->file_type = 2;
  memcpy(de->name, "..", 2);
  off += de->rec_len;
  /* "bin" (Inode 22) */
  de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = 22;
  de->rec_len = 12;
  de->name_len = 3;
  de->file_type = 2;
  memcpy(de->name, "bin", 3);
  off += de->rec_len;
  /* "etc" (Inode 23) */
  de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = 23;
  de->rec_len = EXT4_BLOCK_SIZE - off;
  de->name_len = 3;
  de->file_type = 2;
  memcpy(de->name, "etc", 3);

  /* Write Root Dir Data */
  xseek(f, start_offset + (uint64_t)root_data_block * EXT4_BLOCK_SIZE,
        SEEK_SET);
  xwrite(dir_blk, 1, EXT4_BLOCK_SIZE, f);

  /* Write Root Inode */
  struct ext4_inode root = {0};
  root.i_mode = 0x41ED;   /* Directory | 755 */
  root.i_links_count = 3; // . .. bin etc
  root.i_size_lo = 4096;
  root.i_blocks_lo = 8;
  root.i_block[0] = root_data_block;
  xseek(f, start_offset + 4 * EXT4_BLOCK_SIZE + (2 - 1) * EXT4_INODE_SIZE,
        SEEK_SET);
  xwrite(&root, 1, sizeof(root), f);

  /* ---------------- BIN DIR (Inode 22) ---------------- */
  mark_inode_used(22);
  uint32_t bin_data_block = next_free_block++;
  mark_block_used(bin_data_block);

  memset(dir_blk, 0, EXT4_BLOCK_SIZE);
  off = 0;
  /* "." */
  de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = 22;
  de->rec_len = 12;
  de->name_len = 1;
  de->file_type = 2;
  memcpy(de->name, ".", 1);
  off += de->rec_len;
  /* ".." */
  de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = 2;
  de->rec_len = 12;
  de->name_len = 2;
  de->file_type = 2;
  memcpy(de->name, "..", 2);
  off += de->rec_len;

  const char *bin_files[] = {"init",           "counter",  "shell",
                             "demo3d",         "ipc_recv", "ipc_send",
                             "notify_srv.elf", "regedit",  "writetest.elf"};
  uint32_t bin_inodes[] = {11, 12, 13, 14, 15, 16, 17, 18, 19};
  int num_bin = 9;

  for (int i = 0; i < num_bin; i++) {
    de = (struct ext4_dir_entry *)&dir_blk[off];
    de->inode = bin_inodes[i];
    int nlen = (int)strlen(bin_files[i]);
    de->rec_len = (8 + nlen + 3) & ~3;
    if (i == num_bin - 1)
      de->rec_len = EXT4_BLOCK_SIZE - off;
    de->name_len = nlen;
    de->file_type = 1;
    memcpy(de->name, bin_files[i], nlen);
    off += de->rec_len;
  }
  xseek(f, start_offset + (uint64_t)bin_data_block * EXT4_BLOCK_SIZE, SEEK_SET);
  xwrite(dir_blk, 1, EXT4_BLOCK_SIZE, f);

  /* Write Bin Inode */
  struct ext4_inode bin_inode = {0};
  bin_inode.i_mode = 0x41ED;
  bin_inode.i_links_count = 2;
  bin_inode.i_size_lo = 4096;
  bin_inode.i_blocks_lo = 8;
  bin_inode.i_block[0] = bin_data_block;
  xseek(f, start_offset + 4 * EXT4_BLOCK_SIZE + (22 - 1) * EXT4_INODE_SIZE,
        SEEK_SET);
  xwrite(&bin_inode, 1, sizeof(bin_inode), f);

  /* ---------------- ETC DIR (Inode 23) ---------------- */
  mark_inode_used(23);
  uint32_t etc_data_block = next_free_block++;
  mark_block_used(etc_data_block);

  memset(dir_blk, 0, EXT4_BLOCK_SIZE);
  off = 0;
  /* "." */
  de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = 23;
  de->rec_len = 12;
  de->name_len = 1;
  de->file_type = 2;
  memcpy(de->name, ".", 1);
  off += de->rec_len;
  /* ".." */
  de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = 2;
  de->rec_len = 12;
  de->name_len = 2;
  de->file_type = 2;
  memcpy(de->name, "..", 2);
  off += de->rec_len;
  /* "init.cfg" (Inode 20) */
  de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = 20;
  de->rec_len = EXT4_BLOCK_SIZE - off;
  de->name_len = 8;
  de->file_type = 1;
  memcpy(de->name, "init.cfg", 8);

  xseek(f, start_offset + (uint64_t)etc_data_block * EXT4_BLOCK_SIZE, SEEK_SET);
  xwrite(dir_blk, 1, EXT4_BLOCK_SIZE, f);

  /* Write Etc Inode */
  struct ext4_inode etc_inode = {0};
  etc_inode.i_mode = 0x41ED;
  etc_inode.i_links_count = 2;
  etc_inode.i_size_lo = 4096;
  etc_inode.i_blocks_lo = 8;
  etc_inode.i_block[0] = etc_data_block;
  xseek(f, start_offset + 4 * EXT4_BLOCK_SIZE + (23 - 1) * EXT4_INODE_SIZE,
        SEEK_SET);
  xwrite(&etc_inode, 1, sizeof(etc_inode), f);

  free(dir_blk);

  /* ---------------- WRITE FILES ---------------- */
  current_free_inode = 11;
  add_file_to_ext4(f, "build/init.elf", "init", start_offset);
  add_file_to_ext4(f, "build/counter.elf", "counter", start_offset);
  add_file_to_ext4(f, "build/shell.elf", "shell", start_offset);
  add_file_to_ext4(f, "build/demo3d.elf", "demo3d", start_offset);
  add_file_to_ext4(f, "build/ipc_recv.elf", "ipc_recv", start_offset);
  add_file_to_ext4(f, "build/ipc_send.elf", "ipc_send", start_offset);
  add_file_to_ext4(f, "build/notification_server.elf", "notify_srv.elf",
                   start_offset);
  add_file_to_ext4(f, "build/regedit.elf", "regedit", start_offset);
  add_file_to_ext4(f, "build/writetest.elf", "writetest.elf", start_offset);
  add_file_to_ext4(f, "user/bin/init.cfg", "init.cfg", start_offset);

  /* ---------------- FINALIZE METADATA ---------------- */
  /* Superblock */
  xseek(f, start_offset + EXT4_SUPERBLOCK_OFFSET, SEEK_SET);
  struct ext4_superblock sb = {0};
  sb.s_inodes_count = 1024;
  sb.s_blocks_count_lo = total_blocks;
  sb.s_free_blocks_count_lo = free_blocks_count;
  sb.s_free_inodes_count = free_inodes_count;
  sb.s_first_data_block = 0;
  sb.s_log_block_size = 2; /* 4KB */
  sb.s_blocks_per_group = 8192;
  sb.s_clusters_per_group = 8192;
  sb.s_inodes_per_group = 1024;
  sb.s_magic = EXT4_MAGIC;
  sb.s_state = 1;
  sb.s_rev_level = 1;
  sb.s_first_ino = 11;
  sb.s_inode_size = EXT4_INODE_SIZE;
  xwrite(&sb, 1, sizeof(sb), f);

  /* Group Descriptor */
  xseek(f, start_offset + EXT4_BLOCK_SIZE, SEEK_SET);
  struct ext4_group_desc bg = {0};
  bg.bg_block_bitmap_lo = BLK_BLK_BITMAP;
  bg.bg_inode_bitmap_lo = BLK_INODE_BITMAP;
  bg.bg_inode_table_lo = BLK_INODE_TABLE;
  bg.bg_free_blocks_count_lo = free_blocks_count;
  bg.bg_free_inodes_count_lo = free_inodes_count;
  bg.bg_used_dirs_count_lo = 3; /* Root, bin, etc */
  xwrite(&bg, 1, sizeof(bg), f);

  /* Block Bitmap */
  xseek(f, start_offset + BLK_BLK_BITMAP * EXT4_BLOCK_SIZE, SEEK_SET);
  xwrite(block_bitmap, 1, EXT4_BLOCK_SIZE, f);
  free(block_bitmap);

  /* Inode Bitmap */
  xseek(f, start_offset + BLK_INODE_BITMAP * EXT4_BLOCK_SIZE, SEEK_SET);
  xwrite(inode_bitmap, 1, EXT4_BLOCK_SIZE, f);
  free(inode_bitmap);

  printf("Ext4: Filesystem created. Free Blocks: %d, Free Inodes: %d\n",
         free_blocks_count, free_inodes_count);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <output_file> [bootloader.bin] [kernel.bin]\n",
            argv[0]);
    return 1;
  }
  const char *bootloader_path = (argc > 2) ? argv[2] : NULL;
  const char *kernel_path = (argc > 3) ? argv[3] : NULL;

  FILE *f = fopen(argv[1], "wb+");
  if (!f) {
    perror("fopen");
    return 1;
  }

  /* 1. Zero out disk */
  printf("Creating %dMB disk image...\n", DISK_SIZE_MB);
  xseek(f, DISK_SIZE_BYTES - 1, SEEK_SET);
  fputc(0, f);
  rewind(f);

  /* 2. Protective MBR (LBA 0) */
  uint8_t mbr[SECTOR_SIZE] = {0};
  mbr[510] = 0x55;
  mbr[511] = 0xAA;
  struct mbr_entry *entry = (struct mbr_entry *)&mbr[446];
  entry->status = 0x00;
  entry->type = 0xEE; /* GPT Protective */
  entry->lba_start = 1;
  entry->sectors = NUM_SECTORS - 1;
  xwrite(mbr, 1, SECTOR_SIZE, f);

  /* 3. Prepare Partition Entries */
  uint8_t *entries = xmalloc(128 * 128);
  struct gpt_partition_entry *e = (struct gpt_partition_entry *)entries;

  /* Partition 1: Boot (1MB) LBA 34 to 2081 */
  e[0].type_guid = TYPE_BOOT;
  e[0].start_lba = 34;
  e[0].end_lba = 2081;

  /* Partition 2: Kernel (16MB) LBA 2082 to 34849 */
  e[1].type_guid = TYPE_KERNEL;
  e[1].start_lba = 2082;
  e[1].end_lba = 34849;

  /* Partition 3: Userland (Rest) */
  e[2].type_guid = TYPE_DATA;
  e[2].start_lba = 34850;
  e[2].end_lba = NUM_SECTORS - 34;

  uint32_t entries_crc = crc32(entries, 128 * 128);

  /* 4. Prepare GPT Header (LBA 1) */
  struct gpt_header h = {0};
  h.signature = GPT_SIGNATURE;
  h.revision = GPT_REVISION;
  h.header_size = 92;
  h.my_lba = 1;
  h.alternate_lba = NUM_SECTORS - 1;
  h.first_usable_lba = 34;
  h.last_usable_lba = NUM_SECTORS - 34;
  h.partition_entry_lba = 2;
  h.num_partition_entries = 128;
  h.partition_entry_size = 128;
  h.partition_entry_crc32 = entries_crc;
  h.header_crc32 = 0;
  h.header_crc32 = crc32(&h, 92);

  /* Write GPT Header (LBA 1) */
  xwrite(&h, 1, sizeof(h), f);
  uint8_t pad[SECTOR_SIZE - sizeof(h)];
  memset(pad, 0, sizeof(pad));
  xwrite(pad, 1, sizeof(pad), f);

  /* Write Partition Entries (LBA 2 onwards) */
  xwrite(entries, 1, 128 * 128, f);
  free(entries);

  printf("Disk image created successfully: %s\n", argv[1]);

  /* Format Partition 3 as Ext4 */
  write_ext4_partition(f, e[2].start_lba, e[2].end_lba - e[2].start_lba + 1);

  /* Write raw binaries */
  if (bootloader_path)
    write_raw_to_partition(f, e[0].start_lba, bootloader_path);
  if (kernel_path)
    write_raw_to_partition(f, e[1].start_lba, kernel_path);

  fclose(f);
  return 0;
}
