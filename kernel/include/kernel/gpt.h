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

/* Global partition table */
#define MAX_PARTITIONS 16
extern struct partition partitions[MAX_PARTITIONS];
extern int num_partitions;

/* API */
void gpt_init(void);
struct partition *gpt_get_partition(int index);

#endif /* _KERNEL_GPT_H */
