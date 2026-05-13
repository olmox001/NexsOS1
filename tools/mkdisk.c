/*
 * tools/mkdisk.c
 * Host tool to generate a GPT-partitioned disk image
 * Hardened and Standardized for Determinism
 * Recursive RootFS Support
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#define SECTOR_SIZE 512
#define DISK_SIZE_MB 96
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
struct guid TYPE_BOOT = {0x21686148, 0x6449, 0x6E6F, {0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49}};
struct guid TYPE_KERNEL = {0x0FC63DAF, 0x8483, 0x4772, {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}};
struct guid TYPE_DATA = {0x0FC63DAF, 0x8483, 0x4772, {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}};

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

static uint8_t *block_bitmap = NULL;
static uint8_t *inode_bitmap = NULL;
static uint32_t next_free_block = BLK_DATA_START;
static uint32_t current_free_inode = 11;
static uint32_t total_blocks = 0;
static uint32_t free_blocks_count = 0;
static uint32_t free_inodes_count = 1014;

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

void xseek(FILE *f, long offset, int whence) {
  if (fseek(f, offset, whence) != 0) { perror("fseek"); exit(1); }
}

void xwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
  if (fwrite(ptr, size, nmemb, stream) != nmemb) { perror("fwrite"); exit(1); }
}

void *xmalloc(size_t size) {
  void *p = calloc(1, size);
  if (!p) { perror("malloc"); exit(1); }
  return p;
}

void mark_block_used(uint32_t block) {
  int byte = block / 8;
  int bit = block % 8;
  block_bitmap[byte] |= (1 << bit);
  free_blocks_count--;
}

void mark_inode_used(uint32_t inode) {
  int byte = (inode - 1) / 8;
  int bit = (inode - 1) % 8;
  inode_bitmap[byte] |= (1 << bit);
  free_inodes_count--;
}

void write_file_to_inode(FILE *f, uint64_t partition_offset_bytes, uint32_t inode_num, const char *src_path) {
  uint64_t inode_offset = partition_offset_bytes + 4LL * EXT4_BLOCK_SIZE + (uint64_t)(inode_num - 1) * EXT4_INODE_SIZE;
  struct ext4_inode file_inode = {0};
  file_inode.i_mode = 0x81C0;
  file_inode.i_links_count = 1;

  FILE *src = fopen(src_path, "rb");
  if (!src) return;
  fseek(src, 0, SEEK_END);
  long src_size = ftell(src);
  rewind(src);
  uint8_t *buf = xmalloc(src_size);
  if (fread(buf, 1, src_size, src) != (size_t)src_size) { perror("fread"); exit(1); }
  fclose(src);

  file_inode.i_size_lo = src_size;
  int data_blocks = (src_size + EXT4_BLOCK_SIZE - 1) / EXT4_BLOCK_SIZE;

  for (int i = 0; i < data_blocks && i < 12; i++) {
    uint32_t b = next_free_block++;
    mark_block_used(b);
    file_inode.i_block[i] = b;
    xseek(f, partition_offset_bytes + (uint64_t)b * EXT4_BLOCK_SIZE, SEEK_SET);
    uint32_t to_write = (i == data_blocks - 1 && src_size % EXT4_BLOCK_SIZE) ? (src_size % EXT4_BLOCK_SIZE) : EXT4_BLOCK_SIZE;
    xwrite(buf + i * EXT4_BLOCK_SIZE, 1, to_write, f);
  }

  if (data_blocks > 12) {
    uint32_t indir_blk = next_free_block++;
    mark_block_used(indir_blk);
    file_inode.i_block[12] = indir_blk;
    uint32_t *indir_buf = xmalloc(EXT4_BLOCK_SIZE);
    for (int i = 12; i < data_blocks; i++) {
      uint32_t b = next_free_block++;
      mark_block_used(b);
      indir_buf[i - 12] = b;
      xseek(f, partition_offset_bytes + (uint64_t)b * EXT4_BLOCK_SIZE, SEEK_SET);
      uint32_t to_write = (i == data_blocks - 1 && src_size % EXT4_BLOCK_SIZE) ? (src_size % EXT4_BLOCK_SIZE) : EXT4_BLOCK_SIZE;
      xwrite(buf + i * EXT4_BLOCK_SIZE, 1, to_write, f);
    }
    xseek(f, partition_offset_bytes + (uint64_t)indir_blk * EXT4_BLOCK_SIZE, SEEK_SET);
    xwrite(indir_buf, 1, EXT4_BLOCK_SIZE, f);
    free(indir_buf);
    file_inode.i_blocks_lo = (data_blocks + 1) * (EXT4_BLOCK_SIZE / 512);
  } else {
    file_inode.i_blocks_lo = data_blocks * (EXT4_BLOCK_SIZE / 512);
  }

  xseek(f, inode_offset, SEEK_SET);
  xwrite(&file_inode, 1, sizeof(file_inode), f);
  free(buf);
  printf("Ext4: Added %s (Ino %d, %ld bytes, %d blocks)\n", src_path, inode_num, src_size, data_blocks);
}

void write_directory_inode(FILE *f, uint64_t partition_offset_bytes, uint32_t inode_num, uint32_t data_block) {
  uint64_t inode_offset = partition_offset_bytes + 4LL * EXT4_BLOCK_SIZE + (uint64_t)(inode_num - 1) * EXT4_INODE_SIZE;
  struct ext4_inode inode = {0};
  inode.i_mode = 0x41ED;
  inode.i_links_count = 2;
  inode.i_size_lo = 4096;
  inode.i_blocks_lo = 8;
  inode.i_block[0] = data_block;
  xseek(f, inode_offset, SEEK_SET);
  xwrite(&inode, 1, sizeof(inode), f);
}

uint8_t get_ext4_type(mode_t mode) {
    if (S_ISREG(mode)) return 1;
    if (S_ISDIR(mode)) return 2;
    return 1;
}

void populate_directory(FILE *f, const char *host_path, uint32_t dir_inode, uint32_t parent_inode, uint64_t partition_offset_bytes) {
  DIR *dir = opendir(host_path);
  if (!dir) return;

  uint32_t data_blk_num = next_free_block++;
  mark_block_used(data_blk_num);
  write_directory_inode(f, partition_offset_bytes, dir_inode, data_blk_num);

  uint8_t *dir_blk = xmalloc(EXT4_BLOCK_SIZE);
  int off = 0;

  struct ext4_dir_entry *de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = dir_inode; de->rec_len = 12; de->name_len = 1; de->file_type = 2; memcpy(de->name, ".", 1); off += 12;
  de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = parent_inode; de->rec_len = 12; de->name_len = 2; de->file_type = 2; memcpy(de->name, "..", 2); off += 12;

  struct entry { char name[256]; uint32_t inode; uint8_t type; char path[1024]; } entries[64];
  int count = 0;
  struct dirent *ent;
  while ((ent = readdir(dir)) && count < 64) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 || ent->d_name[0] == '.') continue;
    char p[1024]; snprintf(p, 1024, "%s/%s", host_path, ent->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    uint32_t ino = current_free_inode++;
    mark_inode_used(ino);
    strcpy(entries[count].name, ent->d_name);
    entries[count].inode = ino;
    entries[count].type = get_ext4_type(st.st_mode);
    strcpy(entries[count].path, p);

    de = (struct ext4_dir_entry *)&dir_blk[off];
    de->inode = ino;
    int nlen = strlen(ent->d_name);
    de->rec_len = (8 + nlen + 3) & ~3;
    de->name_len = nlen;
    de->file_type = entries[count].type;
    memcpy(de->name, ent->d_name, nlen);
    off += de->rec_len;
    count++;
  }
  if (off > 24) {
      int last_off = 0, cur = 0;
      while (cur < off) { last_off = cur; cur += ((struct ext4_dir_entry *)&dir_blk[cur])->rec_len; }
      ((struct ext4_dir_entry *)&dir_blk[last_off])->rec_len = EXT4_BLOCK_SIZE - last_off;
  } else {
      ((struct ext4_dir_entry *)&dir_blk[12])->rec_len = EXT4_BLOCK_SIZE - 12;
  }

  xseek(f, partition_offset_bytes + (uint64_t)data_blk_num * EXT4_BLOCK_SIZE, SEEK_SET);
  xwrite(dir_blk, 1, EXT4_BLOCK_SIZE, f);
  free(dir_blk);
  closedir(dir);

  for (int i = 0; i < count; i++) {
    if (entries[i].type == 2) populate_directory(f, entries[i].path, entries[i].inode, dir_inode, partition_offset_bytes);
    else write_file_to_inode(f, partition_offset_bytes, entries[i].inode, entries[i].path);
  }
}

void write_ext4_partition(FILE *f, uint64_t start_lba, uint64_t size_sectors, const char *root_host) {
  uint64_t start_off = start_lba * SECTOR_SIZE;
  total_blocks = (size_sectors * SECTOR_SIZE) / EXT4_BLOCK_SIZE;
  free_blocks_count = total_blocks;
  block_bitmap = xmalloc(EXT4_BLOCK_SIZE);
  inode_bitmap = xmalloc(EXT4_BLOCK_SIZE);

  for (int i = 0; i < 4; i++) mark_block_used(i);
  for (int i = 0; i < INODE_TABLE_BLOCKS; i++) mark_block_used(BLK_INODE_TABLE + i);
  for (int i = 1; i <= 10; i++) mark_inode_used(i);

  mark_inode_used(2);
  populate_directory(f, root_host, 2, 2, start_off);

  xseek(f, start_off + EXT4_SUPERBLOCK_OFFSET, SEEK_SET);
  struct ext4_superblock sb = {0};
  sb.s_inodes_count = 1024; sb.s_blocks_count_lo = total_blocks; sb.s_free_blocks_count_lo = free_blocks_count;
  sb.s_free_inodes_count = free_inodes_count; sb.s_log_block_size = 2; sb.s_magic = EXT4_MAGIC;
  sb.s_state = 1; sb.s_rev_level = 1; sb.s_first_ino = 11; sb.s_inode_size = EXT4_INODE_SIZE;
  xwrite(&sb, 1, sizeof(sb), f);

  xseek(f, start_off + EXT4_BLOCK_SIZE, SEEK_SET);
  struct ext4_group_desc bg = {0};
  bg.bg_block_bitmap_lo = BLK_BLK_BITMAP; bg.bg_inode_bitmap_lo = BLK_INODE_BITMAP;
  bg.bg_inode_table_lo = BLK_INODE_TABLE; bg.bg_free_blocks_count_lo = free_blocks_count;
  bg.bg_free_inodes_count_lo = free_inodes_count;
  xwrite(&bg, 1, sizeof(bg), f);

  xseek(f, start_off + BLK_BLK_BITMAP * EXT4_BLOCK_SIZE, SEEK_SET); xwrite(block_bitmap, 1, EXT4_BLOCK_SIZE, f);
  xseek(f, start_off + BLK_INODE_BITMAP * EXT4_BLOCK_SIZE, SEEK_SET); xwrite(inode_bitmap, 1, EXT4_BLOCK_SIZE, f);
}

int main(int argc, char *argv[]) {
  if (argc < 4) { fprintf(stderr, "Usage: %s <img.img> <boot.bin> <kernel.bin> <root_dir>\n", argv[0]); return 1; }
  const char *boot_path = argv[2], *kern_path = argv[3], *root_dir = argv[4];

  FILE *f = fopen(argv[1], "wb+");
  xseek(f, DISK_SIZE_BYTES - 1, SEEK_SET); fputc(0, f); rewind(f);

  uint8_t mbr[SECTOR_SIZE] = {0}; mbr[510] = 0x55; mbr[511] = 0xAA;
  struct mbr_entry *me = (struct mbr_entry *)&mbr[446];
  me->type = 0xEE; me->lba_start = 1; me->sectors = NUM_SECTORS - 1;
  xwrite(mbr, 1, SECTOR_SIZE, f);

  uint8_t *entries = xmalloc(128 * 128);
  struct gpt_partition_entry *e = (struct gpt_partition_entry *)entries;
  e[0].type_guid = TYPE_BOOT; e[0].start_lba = 34; e[0].end_lba = 2081;
  e[1].type_guid = TYPE_KERNEL; e[1].start_lba = 2082; e[1].end_lba = 34849;
  e[2].type_guid = TYPE_DATA; e[2].start_lba = 34850; e[2].end_lba = NUM_SECTORS - 34;

  struct gpt_header h = {0}; h.signature = GPT_SIGNATURE; h.revision = GPT_REVISION; h.header_size = 92;
  h.my_lba = 1; h.alternate_lba = NUM_SECTORS - 1; h.first_usable_lba = 34; h.last_usable_lba = NUM_SECTORS - 34;
  h.partition_entry_lba = 2; h.num_partition_entries = 128; h.partition_entry_size = 128;
  h.partition_entry_crc32 = crc32(entries, 128 * 128); h.header_crc32 = crc32(&h, 92);
  xwrite(&h, 1, sizeof(h), f);
  uint8_t pad[SECTOR_SIZE - sizeof(h)] = {0}; xwrite(pad, 1, sizeof(pad), f);
  xwrite(entries, 1, 128 * 128, f);

  write_ext4_partition(f, e[2].start_lba, e[2].end_lba - e[2].start_lba + 1, root_dir);
  if (strcmp(boot_path, "none") != 0) {
      FILE *bs = fopen(boot_path, "rb");
      if (bs) {
          fseek(bs, 0, SEEK_END); long sz = ftell(bs); rewind(bs);
          uint8_t *bb = xmalloc(sz); fread(bb, 1, sz, bs); fclose(bs);
          xseek(f, e[0].start_lba * SECTOR_SIZE, SEEK_SET); xwrite(bb, 1, sz, f); free(bb);
      }
  }
  FILE *ks = fopen(kern_path, "rb");
  if (ks) {
      fseek(ks, 0, SEEK_END); long sz = ftell(ks); rewind(ks);
      uint8_t *kb = xmalloc(sz); fread(kb, 1, sz, ks); fclose(ks);
      xseek(f, e[1].start_lba * SECTOR_SIZE, SEEK_SET); xwrite(kb, 1, sz, f); free(kb);
  }
  fclose(f); return 0;
}
