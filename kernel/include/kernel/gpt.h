/*
 * kernel/include/kernel/gpt.h
 * GUID Partition Table (GPT) Definitions
 */
#ifndef _KERNEL_GPT_H
#define _KERNEL_GPT_H

#include <kernel/types.h>

#define GPT_SIGNATURE 0x5452415020494645ULL /* "EFI PART" */
#define SECTOR_SIZE 512

/* GPT Header (LBA 1) */
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
  uint8_t disk_guid[16];
  uint64_t partition_entry_lba;
  uint32_t num_partition_entries;
  uint32_t partition_entry_size;
  uint32_t partition_entry_crc32;
} __attribute__((packed));

struct gpt_partition_entry {
  uint8_t type_guid[16];
  uint8_t unique_guid[16];
  uint64_t start_lba;
  uint64_t end_lba;
  uint64_t attributes;
  uint16_t partition_name[36]; /* UTF-16LE */
} __attribute__((packed));

/* Legacy MBR Definitions */
struct mbr_entry {
  uint8_t status;
  uint8_t chs_start[3];
  uint8_t type;
  uint8_t chs_end[3];
  uint32_t lba_start;
  uint32_t sectors;
} __attribute__((packed));

struct mbr {
  uint8_t boot_code[446];
  struct mbr_entry partitions[4];
  uint16_t signature;
} __attribute__((packed));

#define MBR_SIGNATURE 0xAA55

/* In-memory partition info */
struct partition {
  uint64_t start_lba;
  uint64_t end_lba;
  uint64_t size_sectors;
  uint32_t index;
  uint8_t type_guid[16];
};

/*
 * NEXS partition ROLES (F1 design doc D1/D10, F3).
 *
 * A partition declares what it IS in its type GUID, so the kernel selects
 * partitions by role and never by table index.  That is what retires GPT-02
 * ("changing the disk image layout will silently mount the wrong partition")
 * and what lets an installer re-partition without the kernel guessing.
 *
 * The GUID is 4E455853-RRRR-4E58-9C00-4E4558534F53, where data1 is "NEXS" in
 * ASCII and RRRR is the role below — legible in a hex dump, and impossible to
 * confuse with the EFI/Linux well-known types.  (The previous constants were
 * the *Linux filesystem data* GUID, used for two different roles at once.)
 *
 * MUST MATCH tools/mkdisk.c, which writes them.  mkdisk is a host tool and
 * cannot include kernel headers, so it mirrors these the same way it already
 * mirrors the ext4 on-disk structs; a mismatch means the wrong partition is
 * mounted, so the two lists are kept adjacent in review.
 */
#define NEXS_ROLE_META 0x0001     /* P0  Merkle roots, install marker, version */
#define NEXS_ROLE_KEYSTORE 0x0002 /* PK  secrets; kernel-only, never mounted   */
#define NEXS_ROLE_KERNEL_A 0x0003 /* P1a boot chain slot A                     */
#define NEXS_ROLE_KERNEL_B 0x0004 /* P1b boot chain slot B                     */
#define NEXS_ROLE_ROOT 0x0005     /* P2  "/"                                   */
#define NEXS_ROLE_MACHINE 0x0006  /* P3  /system                               */
#define NEXS_ROLE_USR 0x0007      /* P4+ per-user                              */

/* Global partition table */
#define MAX_PARTITIONS 16
extern struct partition partitions[MAX_PARTITIONS];
extern int num_partitions;

/* API */
void gpt_init(void);
struct partition *gpt_get_partition(int index);
/* partition_role - the NEXS role in a partition's type GUID, or 0 if the GUID
 * is not a NEXS role GUID (a foreign or legacy partition). */
uint16_t partition_role(const struct partition *p);
/* partition_find_by_role - the first partition carrying `role`, or NULL.
 * Roles are unique per disk except USR, where the caller iterates by index. */
struct partition *partition_find_by_role(uint16_t role);

#endif /* _KERNEL_GPT_H */
