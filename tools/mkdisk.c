/*
 * tools/mkdisk.c
 * Host tool to generate a GPT-partitioned disk image
 * Hardened and Standardized for Determinism
 * Recursive RootFS Support
 */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SECTOR_SIZE 512
/* The image must be sized from the rootfs, not from a stale fixed number.  A
 * rootfs larger than the declared GPT/ext4 partition used to make stdio's
 * first runtime allocation address a sector beyond the device, which VirtIO
 * correctly rejected with VIRTIO_BLK_S_IOERR. */
#define GPT_NONPARTITION_SECTORS 67 /* LBA 0..33 + backup GPT at the tail */
#define BLOCKS_PER_MIB (1024 * 1024 / EXT4_BLOCK_SIZE)
#define MIN_PARTITION_BLOCKS (432 * BLOCKS_PER_MIB)
/* Room for savegames, editor files and other runtime-created data after the
 * image's static rootfs.  The VFS owns policy; this is only image capacity. */
#define RUNTIME_RESERVE_BLOCKS (8 * BLOCKS_PER_MIB)
#define EXT4_SINGLE_GROUP_MAX_BLOCKS 110592

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
struct guid TYPE_BOOT = {0x21686148,
                         0x6449,
                         0x6E6F,
                         {0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49}};

/*
 * NEXS ROLE GUIDs (F2, design doc D10).
 *
 * Every partition declares its ROLE in its type GUID, so the kernel identifies
 * partitions by what they ARE and never by table index (D1) — which is what
 * retires GPT-02 ("changing the disk image layout will silently mount the wrong
 * partition") and what lets vfs_init() stop mounting whatever happens to probe
 * first.
 *
 * These REPLACE the previous TYPE_KERNEL/TYPE_DATA, which were a defect on two
 * counts: they were BYTE-IDENTICAL to each other (so they could not distinguish
 * anything), and their value was 0FC63DAF-8483-4772-8E79-3D69D8477DE4 — the
 * standard *Linux filesystem data* GUID.  Claiming to be Linux partitions
 * invites another OS's installer to treat them as its own.
 *
 * data1 is 0x4E455853 = "NEXS" in ASCII, so the role is legible in a hex dump
 * and cannot collide with the EFI/Linux well-known types.  data2 is the role.
 * TYPE_BOOT above is left standard on purpose: where a BIOS boot partition is
 * genuinely required, it must carry the GUID firmware looks for.
 *
 * CANONICAL DEFINITION: kernel/include/kernel/gpt.h (NEXS_ROLE_*).  This is a
 * host tool and cannot include kernel headers, so it mirrors them — the same
 * convention this file already follows for the ext4 on-disk structs.  A
 * mismatch does not fail the build, it mounts the wrong partition, so the two
 * lists are reviewed together.
 */
#define NEXS_GUID(role_id)                                                     \
  {                                                                            \
    0x4E455853, (role_id), 0x4E58, {                                           \
      0x9C, 0x00, 0x4E, 0x45, 0x58, 0x53, 0x4F, 0x53                           \
    }                                                                          \
  }
struct guid TYPE_NEXS_META = NEXS_GUID(0x0001);     /* P0  Merkle roots, marker */
struct guid TYPE_NEXS_KEYSTORE = NEXS_GUID(0x0002); /* PK  secrets, kernel-only */
struct guid TYPE_NEXS_KERNEL_A = NEXS_GUID(0x0003); /* P1a boot chain slot A    */
struct guid TYPE_NEXS_KERNEL_B = NEXS_GUID(0x0004); /* P1b boot chain slot B    */
struct guid TYPE_NEXS_ROOT = NEXS_GUID(0x0005);     /* P2  "/"                  */
struct guid TYPE_NEXS_MACHINE = NEXS_GUID(0x0006);  /* P3  /system              */
struct guid TYPE_NEXS_USR = NEXS_GUID(0x0007);      /* P4+ per-user             */

/* ---------------------------------------------------------------------------
 * SHA-256 (F2, design doc §4bis) — host side.
 *
 * FIPS 180-4.  The kernel gets the SAME implementation (kernel/lib/sha256.c) in
 * F3: a digest is only meaningful if both sides compute it identically, so the
 * two must be one algorithm, not two transcriptions of a spec.
 *
 * This is the leaf primitive for the Merkle tree (D13): the image is hashed per
 * 4 KiB block (Q8), leaves are hashed pairwise up to a root, and the root goes
 * in P0.  Verification then costs a full read at boot but a write costs only
 * log2(n) rehashes — which is what makes "verify ROOT at every boot" survive a
 * writable filesystem.
 * ------------------------------------------------------------------------- */
struct sha256_ctx {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  size_t buflen;
};

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define SHA256_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(struct sha256_ctx *c, const uint8_t *p) {
  uint32_t w[64], a, b, cc, d, e, f, g, h;
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = SHA256_ROR(w[i - 15], 7) ^ SHA256_ROR(w[i - 15], 18) ^
                  (w[i - 15] >> 3);
    uint32_t s1 =
        SHA256_ROR(w[i - 2], 17) ^ SHA256_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
  e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = SHA256_ROR(e, 6) ^ SHA256_ROR(e, 11) ^ SHA256_ROR(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
    uint32_t S0 = SHA256_ROR(a, 2) ^ SHA256_ROR(a, 13) ^ SHA256_ROR(a, 22);
    uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + mj;
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(struct sha256_ctx *c) {
  c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85; c->h[2] = 0x3c6ef372;
  c->h[3] = 0xa54ff53a; c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
  c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
  c->len = 0;
  c->buflen = 0;
}

static void sha256_update(struct sha256_ctx *c, const void *data, size_t n) {
  const uint8_t *p = (const uint8_t *)data;
  c->len += n;
  while (n) {
    size_t take = 64 - c->buflen;
    if (take > n)
      take = n;
    memcpy(c->buf + c->buflen, p, take);
    c->buflen += take;
    p += take;
    n -= take;
    if (c->buflen == 64) {
      sha256_block(c, c->buf);
      c->buflen = 0;
    }
  }
}

static void sha256_final(struct sha256_ctx *c, uint8_t out[32]) {
  uint64_t bits = c->len * 8;
  uint8_t pad = 0x80;
  sha256_update(c, &pad, 1);
  uint8_t zero = 0;
  while (c->buflen != 56)
    sha256_update(c, &zero, 1);
  uint8_t lenbe[8];
  for (int i = 0; i < 8; i++)
    lenbe[i] = (uint8_t)(bits >> (56 - i * 8));
  sha256_update(c, lenbe, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(c->h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)c->h[i];
  }
}

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

/* Extent tree on-disk format (mirrors kernel/include/kernel/ext4.h) */
#define EXT4_EXT_MAGIC 0xF30A
#define EXT4_EXTENTS_FL 0x00080000
#define EXT4_FEATURE_INCOMPAT_FILETYPE 0x0002
#define EXT4_FEATURE_INCOMPAT_EXTENTS 0x0040

struct ext4_extent_header {
  uint16_t eh_magic;
  uint16_t eh_entries;
  uint16_t eh_max;
  uint16_t eh_depth;
  uint32_t eh_generation;
} __attribute__((packed));

struct ext4_extent_idx {
  uint32_t ei_block;
  uint32_t ei_leaf_lo;
  uint16_t ei_leaf_hi;
  uint16_t ei_unused;
} __attribute__((packed));

struct ext4_extent {
  uint32_t ee_block;
  uint16_t ee_len;
  uint16_t ee_start_hi;
  uint32_t ee_start_lo;
} __attribute__((packed));

static uint8_t *block_bitmap = NULL;
static uint8_t *inode_bitmap = NULL;
static uint32_t next_free_block = BLK_DATA_START;
static uint32_t current_free_inode = 11;
static uint32_t total_blocks = 0;
static uint32_t free_blocks_count = 0;
static uint32_t free_inodes_count = 1014;
/* Inode layout: 1 = extent trees (mkfs.ext4 default, what the kernel must
 * handle on real images), 0 = legacy direct/indirect pointers (--legacy). */
static int use_extents = 1;

/* image_plan mirrors exactly the allocations performed by populate_directory:
 * one inode + one data block per directory; one inode plus rounded-up data
 * blocks per regular file; and, for a large extent file, one leaf block per
 * 340 extent records.  It lets us reserve a valid single-group ext4 partition
 * before writing the first byte, rather than relying on host-file sparseness.
 */
struct image_plan {
  uint64_t inodes;
  uint64_t data_blocks;
  uint64_t extent_leaf_blocks;
};

static void plan_extent_leaves(struct image_plan *plan, uint64_t data_blocks) {
  if (!use_extents || data_blocks <= 32)
    return; /* depth-0 root fits inline in i_block[] */
  uint64_t extents = (data_blocks + 7) / 8; /* build_extent_tree's cap */
  plan->extent_leaf_blocks += (extents + 339) / 340;
}

/* plan_rootfs recursively counts the finite rootfs allocation set.  mkdisk
 * already requires ordinary files/directories, so fail loudly for an input it
 * could not faithfully encode instead of producing a subtly undersized image.
 */
static void plan_rootfs(const char *host_path, struct image_plan *plan) {
  struct stat st;
  if (lstat(host_path, &st) != 0) {
    perror(host_path);
    exit(1);
  }
  if (S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(host_path);
    if (!dir) {
      perror(host_path);
      exit(1);
    }
    plan->inodes++;
    plan->data_blocks++;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        continue;
      char child[1024];
      int n = snprintf(child, sizeof(child), "%s/%s", host_path, ent->d_name);
      if (n < 0 || (size_t)n >= sizeof(child)) {
        fprintf(stderr, "mkdisk: path too long below %s\n", host_path);
        closedir(dir);
        exit(1);
      }
      plan_rootfs(child, plan);
    }
    closedir(dir);
    return;
  }
  if (!S_ISREG(st.st_mode)) {
    fprintf(stderr, "mkdisk: unsupported rootfs entry %s\n", host_path);
    exit(1);
  }
  if (st.st_size < 0) {
    fprintf(stderr, "mkdisk: negative size for %s\n", host_path);
    exit(1);
  }
  uint64_t blocks =
      ((uint64_t)st.st_size + EXT4_BLOCK_SIZE - 1) / EXT4_BLOCK_SIZE;
  plan->inodes++;
  plan->data_blocks += blocks;
  plan_extent_leaves(plan, blocks);
}

static uint64_t plan_partition_blocks(const char *root_host) {
  struct image_plan plan = {0};
  plan_rootfs(root_host, &plan);

  if (plan.inodes > 1014) {
    fprintf(stderr, "mkdisk: rootfs needs %llu inodes; image supports 1014\n",
            (unsigned long long)plan.inodes);
    exit(1);
  }

  uint64_t blocks = BLK_DATA_START + plan.data_blocks +
                    plan.extent_leaf_blocks + RUNTIME_RESERVE_BLOCKS;
  if (blocks < MIN_PARTITION_BLOCKS)
    blocks = MIN_PARTITION_BLOCKS;
  /* Round to a MiB: a stable image size while preserving exact 4 KiB blocks. */
  blocks = ((blocks + BLOCKS_PER_MIB - 1) / BLOCKS_PER_MIB) * BLOCKS_PER_MIB;
  if (blocks > EXT4_SINGLE_GROUP_MAX_BLOCKS) {
    fprintf(stderr,
            "mkdisk: rootfs + %u MiB runtime reserve needs %llu blocks; "
            "the single-group ext4 driver supports at most %u\n",
            RUNTIME_RESERVE_BLOCKS / BLOCKS_PER_MIB, (unsigned long long)blocks,
            EXT4_SINGLE_GROUP_MAX_BLOCKS);
    exit(1);
  }

  printf("mkdisk: rootfs plan = %llu inodes, %llu data blocks, "
         "%llu extent leaves; partition = %llu MiB (+%u MiB runtime)\n",
         (unsigned long long)plan.inodes, (unsigned long long)plan.data_blocks,
         (unsigned long long)plan.extent_leaf_blocks,
         (unsigned long long)(blocks / BLOCKS_PER_MIB),
         RUNTIME_RESERVE_BLOCKS / BLOCKS_PER_MIB);
  return blocks;
}

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
  void *p = calloc(1, size);
  if (!p) {
    perror("malloc");
    exit(1);
  }
  return p;
}

void mark_block_used(uint32_t block) {
  if (block >= total_blocks) {
    fprintf(stderr,
            "mkdisk: internal error: block %u exceeds partition (%u blocks)\n",
            block, total_blocks);
    exit(1);
  }
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

/*
 * build_extent_tree - describe the contiguous run [first_block,
 * first_block+nblocks) as an extent tree rooted in i_block[].
 *
 * Extent length is capped at 8 blocks for files larger than 32 blocks so
 * big files (ELFs, the doom WAD) get enough extents to need a depth-1 tree
 * — this makes the kernel's index-node walk a tested path instead of dead
 * code.  Small files stay depth 0 with a single inline extent.
 */
void build_extent_tree(FILE *f, uint64_t partition_offset_bytes,
                       struct ext4_inode *ino, uint32_t first_block,
                       uint32_t nblocks, uint32_t *meta_blocks) {
  struct ext4_extent_header *eh = (struct ext4_extent_header *)ino->i_block;
  uint32_t cap = (nblocks > 32) ? 8 : 32768;
  uint32_t n_ext = (nblocks + cap - 1) / cap;

  ino->i_flags |= EXT4_EXTENTS_FL;
  eh->eh_magic = EXT4_EXT_MAGIC;
  eh->eh_max = 4;
  eh->eh_generation = 0;

  if (n_ext <= 4) {
    /* Depth 0: extents inline in the inode. */
    eh->eh_entries = n_ext;
    eh->eh_depth = 0;
    struct ext4_extent *ex = (struct ext4_extent *)(eh + 1);
    for (uint32_t i = 0; i < n_ext; i++) {
      ex[i].ee_block = i * cap;
      ex[i].ee_len = (i == n_ext - 1) ? (nblocks - i * cap) : cap;
      ex[i].ee_start_hi = 0;
      ex[i].ee_start_lo = first_block + i * cap;
    }
    return;
  }

  /* Depth 1: the inode root holds index records pointing at leaf blocks. */
  uint32_t per_leaf = (EXT4_BLOCK_SIZE - sizeof(struct ext4_extent_header)) /
                      sizeof(struct ext4_extent); /* 340 */
  uint32_t n_leaf = (n_ext + per_leaf - 1) / per_leaf;
  if (n_leaf > 4) {
    fprintf(stderr, "mkdisk: file needs %u extent leaves (max 4)\n", n_leaf);
    exit(1);
  }
  eh->eh_entries = n_leaf;
  eh->eh_depth = 1;
  struct ext4_extent_idx *ix = (struct ext4_extent_idx *)(eh + 1);

  uint32_t ei = 0;
  for (uint32_t l = 0; l < n_leaf; l++) {
    uint32_t leaf_blk = next_free_block++;
    mark_block_used(leaf_blk);
    (*meta_blocks)++;

    uint8_t *lb = xmalloc(EXT4_BLOCK_SIZE);
    struct ext4_extent_header *lh = (struct ext4_extent_header *)lb;
    uint32_t count = (n_ext - ei < per_leaf) ? (n_ext - ei) : per_leaf;
    lh->eh_magic = EXT4_EXT_MAGIC;
    lh->eh_entries = count;
    lh->eh_max = per_leaf;
    lh->eh_depth = 0;
    lh->eh_generation = 0;

    ix[l].ei_block = ei * cap;
    ix[l].ei_leaf_lo = leaf_blk;
    ix[l].ei_leaf_hi = 0;
    ix[l].ei_unused = 0;

    struct ext4_extent *ex = (struct ext4_extent *)(lh + 1);
    for (uint32_t k = 0; k < count; k++, ei++) {
      ex[k].ee_block = ei * cap;
      ex[k].ee_len = (ei == n_ext - 1) ? (nblocks - ei * cap) : cap;
      ex[k].ee_start_hi = 0;
      ex[k].ee_start_lo = first_block + ei * cap;
    }

    xseek(f, partition_offset_bytes + (uint64_t)leaf_blk * EXT4_BLOCK_SIZE,
          SEEK_SET);
    xwrite(lb, 1, EXT4_BLOCK_SIZE, f);
    free(lb);
  }
}

void write_file_to_inode(FILE *f, uint64_t partition_offset_bytes,
                         uint32_t inode_num, const char *src_path) {
  uint64_t inode_offset = partition_offset_bytes + 4LL * EXT4_BLOCK_SIZE +
                          (uint64_t)(inode_num - 1) * EXT4_INODE_SIZE;
  struct ext4_inode file_inode = {0};
  file_inode.i_mode = 0x81C0;
  file_inode.i_links_count = 1;

  FILE *src = fopen(src_path, "rb");
  if (!src)
    return;
  fseek(src, 0, SEEK_END);
  long src_size = ftell(src);
  rewind(src);
  uint8_t *buf = xmalloc(src_size);
  if (fread(buf, 1, src_size, src) != (size_t)src_size) {
    perror("fread");
    exit(1);
  }
  fclose(src);

  file_inode.i_size_lo = (uint32_t)src_size;
  uint32_t data_blocks = (src_size + EXT4_BLOCK_SIZE - 1) / EXT4_BLOCK_SIZE;
  uint32_t total_meta_blocks = 0;

  uint32_t *indir1 = NULL;
  uint32_t *indir2 = NULL;
  uint32_t *indir2_subs[1024] = {NULL};

  if (use_extents) {
    /* Extent layout: write the data as one contiguous run, then describe it
     * with an extent tree in i_block[] (no indirect pointer blocks). */
    uint32_t first_block = next_free_block;
    for (uint32_t i = 0; i < data_blocks; i++) {
      uint32_t b = next_free_block++;
      mark_block_used(b);
      xseek(f, partition_offset_bytes + (uint64_t)b * EXT4_BLOCK_SIZE,
            SEEK_SET);
      uint32_t to_write = (i == data_blocks - 1 && src_size % EXT4_BLOCK_SIZE)
                              ? (src_size % EXT4_BLOCK_SIZE)
                              : EXT4_BLOCK_SIZE;
      xwrite(buf + i * EXT4_BLOCK_SIZE, 1, to_write, f);
    }
    build_extent_tree(f, partition_offset_bytes, &file_inode, first_block,
                      data_blocks, &total_meta_blocks);

    file_inode.i_blocks_lo =
        (data_blocks + total_meta_blocks) * (EXT4_BLOCK_SIZE / 512);
    xseek(f, inode_offset, SEEK_SET);
    xwrite(&file_inode, 1, sizeof(file_inode), f);
    free(buf);
    printf("Ext4: Added %s (Ino %d, %ld bytes, %d data, %d meta blocks, "
           "extents)\n",
           src_path, inode_num, src_size, data_blocks, total_meta_blocks);
    return;
  }

  for (uint32_t i = 0; i < data_blocks; i++) {
    uint32_t b = next_free_block++;
    mark_block_used(b);

    /* Write data block */
    xseek(f, partition_offset_bytes + (uint64_t)b * EXT4_BLOCK_SIZE, SEEK_SET);
    uint32_t to_write = (i == data_blocks - 1 && src_size % EXT4_BLOCK_SIZE)
                            ? (src_size % EXT4_BLOCK_SIZE)
                            : EXT4_BLOCK_SIZE;
    xwrite(buf + i * EXT4_BLOCK_SIZE, 1, to_write, f);

    if (i < 12) {
      file_inode.i_block[i] = b;
    } else if (i < 12 + 1024) {
      if (!indir1) {
        file_inode.i_block[12] = next_free_block++;
        mark_block_used(file_inode.i_block[12]);
        total_meta_blocks++;
        indir1 = xmalloc(EXT4_BLOCK_SIZE);
      }
      indir1[i - 12] = b;
    } else {
      uint32_t d_idx = i - 12 - 1024;
      uint32_t master_idx = d_idx / 1024;
      uint32_t sub_idx = d_idx % 1024;

      if (!indir2) {
        file_inode.i_block[13] = next_free_block++;
        mark_block_used(file_inode.i_block[13]);
        total_meta_blocks++;
        indir2 = xmalloc(EXT4_BLOCK_SIZE);
      }
      if (!indir2_subs[master_idx]) {
        indir2[master_idx] = next_free_block++;
        mark_block_used(indir2[master_idx]);
        total_meta_blocks++;
        indir2_subs[master_idx] = xmalloc(EXT4_BLOCK_SIZE);
      }
      indir2_subs[master_idx][sub_idx] = b;
    }
  }

  /* Flush Metadata Blocks */
  if (indir1) {
    xseek(f,
          partition_offset_bytes +
              (uint64_t)file_inode.i_block[12] * EXT4_BLOCK_SIZE,
          SEEK_SET);
    xwrite(indir1, 1, EXT4_BLOCK_SIZE, f);
    free(indir1);
  }
  if (indir2) {
    for (int i = 0; i < 1024; i++) {
      if (indir2_subs[i]) {
        xseek(f, partition_offset_bytes + (uint64_t)indir2[i] * EXT4_BLOCK_SIZE,
              SEEK_SET);
        xwrite(indir2_subs[i], 1, EXT4_BLOCK_SIZE, f);
        free(indir2_subs[i]);
      }
    }
    xseek(f,
          partition_offset_bytes +
              (uint64_t)file_inode.i_block[13] * EXT4_BLOCK_SIZE,
          SEEK_SET);
    xwrite(indir2, 1, EXT4_BLOCK_SIZE, f);
    free(indir2);
  }

  file_inode.i_blocks_lo =
      (data_blocks + total_meta_blocks) * (EXT4_BLOCK_SIZE / 512);

  xseek(f, inode_offset, SEEK_SET);
  xwrite(&file_inode, 1, sizeof(file_inode), f);
  free(buf);
  printf("Ext4: Added %s (Ino %d, %ld bytes, %d data, %d meta blocks)\n",
         src_path, inode_num, src_size, data_blocks, total_meta_blocks);
}

void write_directory_inode(FILE *f, uint64_t partition_offset_bytes,
                           uint32_t inode_num, uint32_t data_block) {
  uint64_t inode_offset = partition_offset_bytes + 4LL * EXT4_BLOCK_SIZE +
                          (uint64_t)(inode_num - 1) * EXT4_INODE_SIZE;
  struct ext4_inode inode = {0};
  inode.i_mode = 0x41ED;
  inode.i_links_count = 2;
  inode.i_size_lo = 4096;
  inode.i_blocks_lo = 8;
  if (use_extents) {
    uint32_t meta = 0;
    build_extent_tree(f, partition_offset_bytes, &inode, data_block, 1, &meta);
  } else {
    inode.i_block[0] = data_block;
  }
  xseek(f, inode_offset, SEEK_SET);
  xwrite(&inode, 1, sizeof(inode), f);
}

uint8_t get_ext4_type(mode_t mode) {
  if (S_ISREG(mode))
    return 1;
  if (S_ISDIR(mode))
    return 2;
  return 1;
}

void populate_directory(FILE *f, const char *host_path, uint32_t dir_inode,
                        uint32_t parent_inode,
                        uint64_t partition_offset_bytes) {
  DIR *dir = opendir(host_path);
  if (!dir)
    return;

  uint32_t data_blk_num = next_free_block++;
  mark_block_used(data_blk_num);
  write_directory_inode(f, partition_offset_bytes, dir_inode, data_blk_num);

  uint8_t *dir_blk = xmalloc(EXT4_BLOCK_SIZE);
  int off = 0;

  struct ext4_dir_entry *de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = dir_inode;
  de->rec_len = 12;
  de->name_len = 1;
  de->file_type = 2;
  memcpy(de->name, ".", 1);
  off += 12;
  de = (struct ext4_dir_entry *)&dir_blk[off];
  de->inode = parent_inode;
  de->rec_len = 12;
  de->name_len = 2;
  de->file_type = 2;
  memcpy(de->name, "..", 2);
  off += 12;

  struct entry {
    char name[256];
    uint32_t inode;
    uint8_t type;
    char path[1024];
  } entries[64];
  int count = 0;
  struct dirent *ent;
  while ((ent = readdir(dir)) && count < 64) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0 ||
        ent->d_name[0] == '.')
      continue;
    char p[1024];
    snprintf(p, 1024, "%s/%s", host_path, ent->d_name);
    struct stat st;
    if (stat(p, &st) != 0)
      continue;
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
    while (cur < off) {
      last_off = cur;
      cur += ((struct ext4_dir_entry *)&dir_blk[cur])->rec_len;
    }
    ((struct ext4_dir_entry *)&dir_blk[last_off])->rec_len =
        EXT4_BLOCK_SIZE - last_off;
  } else {
    ((struct ext4_dir_entry *)&dir_blk[12])->rec_len = EXT4_BLOCK_SIZE - 12;
  }

  xseek(f, partition_offset_bytes + (uint64_t)data_blk_num * EXT4_BLOCK_SIZE,
        SEEK_SET);
  xwrite(dir_blk, 1, EXT4_BLOCK_SIZE, f);
  free(dir_blk);
  closedir(dir);

  for (int i = 0; i < count; i++) {
    if (entries[i].type == 2)
      populate_directory(f, entries[i].path, entries[i].inode, dir_inode,
                         partition_offset_bytes);
    else
      write_file_to_inode(f, partition_offset_bytes, entries[i].inode,
                          entries[i].path);
  }
}

void write_ext4_partition(FILE *f, uint64_t start_lba, uint64_t size_sectors,
                          const char *root_host) {
  uint64_t start_off = start_lba * SECTOR_SIZE;

  /* Reset the allocator state so this function can be called MORE THAN ONCE
   * (F2: one call per ext4 partition).  total_blocks/free_blocks_count were
   * already re-initialised here, but next_free_block, current_free_inode and
   * free_inodes_count are file-scope and were only ever initialised at load
   * time — a second partition would have continued allocating from where the
   * first one stopped, producing a filesystem whose bitmaps and superblock
   * disagree.  Latent until now because exactly one partition was ever
   * written; fixed before it can bite. */
  next_free_block = BLK_DATA_START;
  current_free_inode = 11;
  free_inodes_count = 1014;
  free(block_bitmap);
  free(inode_bitmap);

  total_blocks = (size_sectors * SECTOR_SIZE) / EXT4_BLOCK_SIZE;
  free_blocks_count = total_blocks;
  block_bitmap = xmalloc(EXT4_BLOCK_SIZE);
  inode_bitmap = xmalloc(EXT4_BLOCK_SIZE);

  for (int i = 0; i < 4; i++)
    mark_block_used(i);
  for (int i = 0; i < INODE_TABLE_BLOCKS; i++)
    mark_block_used(BLK_INODE_TABLE + i);
  for (int i = 1; i <= 10; i++)
    mark_inode_used(i);

  mark_inode_used(2);
  populate_directory(f, root_host, 2, 2, start_off);

  xseek(f, start_off + EXT4_SUPERBLOCK_OFFSET, SEEK_SET);
  struct ext4_superblock sb = {0};
  sb.s_inodes_count = 1024;
  sb.s_blocks_count_lo = total_blocks;
  sb.s_free_blocks_count_lo = free_blocks_count;
  sb.s_free_inodes_count = free_inodes_count;
  sb.s_log_block_size = 2;
  sb.s_magic = EXT4_MAGIC;
  sb.s_blocks_per_group = total_blocks;
  sb.s_inodes_per_group = 1024;
  sb.s_state = 1;
  sb.s_rev_level = 1;
  sb.s_first_ino = 11;
  sb.s_inode_size = EXT4_INODE_SIZE;
  /* Declare what the image actually uses so the kernel's INCOMPAT whitelist
   * is a tested path (extent inodes + typed directory entries). */
  if (use_extents)
    sb.s_feature_incompat =
        EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS;
  xwrite(&sb, 1, sizeof(sb), f);

  xseek(f, start_off + EXT4_BLOCK_SIZE, SEEK_SET);
  struct ext4_group_desc bg = {0};
  bg.bg_block_bitmap_lo = BLK_BLK_BITMAP;
  bg.bg_inode_bitmap_lo = BLK_INODE_BITMAP;
  bg.bg_inode_table_lo = BLK_INODE_TABLE;
  bg.bg_free_blocks_count_lo = free_blocks_count;
  bg.bg_free_inodes_count_lo = free_inodes_count;
  xwrite(&bg, 1, sizeof(bg), f);

  xseek(f, start_off + BLK_BLK_BITMAP * EXT4_BLOCK_SIZE, SEEK_SET);
  xwrite(block_bitmap, 1, EXT4_BLOCK_SIZE, f);
  xseek(f, start_off + BLK_INODE_BITMAP * EXT4_BLOCK_SIZE, SEEK_SET);
  xwrite(inode_bitmap, 1, EXT4_BLOCK_SIZE, f);
}

int main(int argc, char *argv[]) {
  if (argc < 5) {
    fprintf(stderr,
            "Usage: %s <img.img> <boot.bin> <kernel.bin> <root_dir> "
            "[--legacy|--extents]\n",
            argv[0]);
    return 1;
  }
  const char *boot_path = argv[2], *kern_path = argv[3], *root_dir = argv[4];
  if (argc > 5 && strcmp(argv[5], "--legacy") == 0)
    use_extents = 0;
  printf("mkdisk: inode layout = %s\n",
         use_extents ? "extents" : "legacy (indirect blocks)");
  uint64_t partition_blocks = plan_partition_blocks(root_dir);
  uint64_t disk_sectors =
      partition_blocks * EXT4_SECTORS_PER_BLOCK + GPT_NONPARTITION_SECTORS;
  uint64_t disk_size_bytes = disk_sectors * SECTOR_SIZE;

  FILE *f = fopen(argv[1], "wb+");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  xseek(f, (long)disk_size_bytes - 1, SEEK_SET);
  fputc(0, f);
  rewind(f);

  uint8_t mbr[SECTOR_SIZE] = {0};
  mbr[510] = 0x55;
  mbr[511] = 0xAA;
  struct mbr_entry *me = (struct mbr_entry *)&mbr[446];
  me->type = 0xEE;
  me->lba_start = 1;
  me->sectors = (uint32_t)disk_sectors - 1;
  xwrite(mbr, 1, SECTOR_SIZE, f);

  /*
   * Partition emission, F2.
   *
   * The image still contains exactly ONE ext4 partition, and it still occupies
   * the whole usable range, so the on-disk layout is unchanged and `make run`
   * boots the same image as before.  What changes is that the partition now
   * DECLARES ITS ROLE (TYPE_NEXS_ROOT) instead of claiming to be a Linux
   * filesystem, and it is emitted through a role TABLE rather than by writing
   * e[0] by hand — which is the seam the rest of the partition set (P0 META,
   * PK KEYSTORE, P1 KERNEL A/B, P3 MACHINE, P4 USR) drops into once F3 teaches
   * the kernel to mount BY ROLE.
   *
   * Deliberately staged: adding the other partitions before the kernel can
   * identify them by role would hit exactly the failure the design doc records
   * — vfs_init() mounts the first partition any driver accepts, so a second
   * ext4 could silently become "/".
   *
   * boot_path/kern_path stay accepted-and-ignored for Makefile compatibility;
   * the KERNEL partition returns as P1 A/B in the same step that gives it a
   * reader.
   */
  (void)boot_path;
  (void)kern_path;

  struct part_spec {
    struct guid type;
    uint64_t start_lba;
    uint64_t end_lba;
    const char *label;
  } specs[128];
  int nspecs = 0;

  specs[nspecs].type = TYPE_NEXS_ROOT;
  specs[nspecs].start_lba = 34;
  specs[nspecs].end_lba = disk_sectors - 34;
  specs[nspecs].label = "NEXS-ROOT";
  nspecs++;

  uint8_t *entries = xmalloc(128 * 128);
  struct gpt_partition_entry *e = (struct gpt_partition_entry *)entries;
  for (int i = 0; i < nspecs; i++) {
    e[i].type_guid = specs[i].type;
    e[i].start_lba = specs[i].start_lba;
    e[i].end_lba = specs[i].end_lba;
    /* UTF-16LE partition name, ASCII subset — legible in any partition tool. */
    for (int k = 0; specs[i].label[k] && k < 35; k++)
      e[i].partition_name[k] = (uint16_t)specs[i].label[k];
  }

  struct gpt_header h = {0};
  h.signature = GPT_SIGNATURE;
  h.revision = GPT_REVISION;
  h.header_size = 92;
  h.my_lba = 1;
  h.alternate_lba = disk_sectors - 1;
  h.first_usable_lba = 34;
  h.last_usable_lba = disk_sectors - 34;
  h.partition_entry_lba = 2;
  h.num_partition_entries = 128;
  h.partition_entry_size = 128;
  h.partition_entry_crc32 = crc32(entries, 128 * 128);
  h.header_crc32 = crc32(&h, 92);
  xwrite(&h, 1, sizeof(h), f);
  uint8_t pad[SECTOR_SIZE - sizeof(h)] = {0};
  xwrite(pad, 1, sizeof(pad), f);
  xwrite(entries, 1, 128 * 128, f);

  write_ext4_partition(f, e[0].start_lba, e[0].end_lba - e[0].start_lba + 1,
                       root_dir);

  /*
   * Merkle root over the ROOT partition (§4bis, D13).
   *
   * Computed AFTER the filesystem is written, over the partition's blocks in
   * order: one SHA-256 leaf per 4 KiB block (Q8), then leaves hashed pairwise
   * up to a single root.  A lone leaf at an odd level is promoted unchanged
   * rather than paired with a duplicate of itself — self-pairing is the classic
   * Merkle malleability bug (CVE-2012-2459 in Bitcoin), where two different
   * block sequences can produce the same root.
   *
   * For now the root is REPORTED, not stored: P0 NEXS-META does not exist until
   * the partition set lands, and writing the digest somewhere the kernel cannot
   * yet find would be a value nobody reads.  Printing it makes the build
   * reproducible-checkable today and gives F3 a known-good expected value.
   */
  {
    uint64_t first = e[0].start_lba, last = e[0].end_lba;
    uint64_t nblocks =
        ((last - first + 1) * SECTOR_SIZE) / EXT4_BLOCK_SIZE;
    uint8_t *level = xmalloc((size_t)nblocks * 32);
    uint8_t *blk = xmalloc(EXT4_BLOCK_SIZE);
    for (uint64_t i = 0; i < nblocks; i++) {
      xseek(f, first * SECTOR_SIZE + i * EXT4_BLOCK_SIZE, SEEK_SET);
      if (fread(blk, 1, EXT4_BLOCK_SIZE, f) != EXT4_BLOCK_SIZE)
        memset(blk, 0, EXT4_BLOCK_SIZE); /* tail past EOF reads as zeroes */
      struct sha256_ctx c;
      sha256_init(&c);
      sha256_update(&c, blk, EXT4_BLOCK_SIZE);
      sha256_final(&c, level + i * 32);
    }
    uint64_t n = nblocks;
    while (n > 1) {
      uint64_t out = 0;
      for (uint64_t i = 0; i < n; i += 2, out++) {
        if (i + 1 == n) {
          memmove(level + out * 32, level + i * 32, 32); /* promote, not pair */
        } else {
          struct sha256_ctx c;
          sha256_init(&c);
          sha256_update(&c, level + i * 32, 64);
          sha256_final(&c, level + out * 32);
        }
      }
      n = out;
    }
    printf("mkdisk: NEXS-ROOT merkle sha256 = ");
    for (int i = 0; i < 32 && nblocks; i++)
      printf("%02x", level[i]);
    printf(" (%llu blocks of %d)\n", (unsigned long long)nblocks,
           EXT4_BLOCK_SIZE);
    free(level);
    free(blk);
  }

  fclose(f);
  return 0;
}
