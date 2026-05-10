/*
 * kernel/fs/gpt.c
 * GPT Partition Table Parser
 */
#include <drivers/virtio_blk.h>
#include <kernel/gpt.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>

struct partition partitions[MAX_PARTITIONS];
int num_partitions = 0;

void gpt_init(void) {
  pr_info("%s", "GPT: Initializing...\n");

  /* Allocate buffer for reading sectors */
  uint8_t *buf = (uint8_t *)pmm_alloc_page();
  if (!buf) {
    pr_info("%s", "GPT: Failed to allocate buffer\n");
    return;
  }

  /* 1. Read GPT Header (LBA 1) */
  /* LBA 0 is MBR, LBA 1 is GPT Header */
  if (virtio_blk_read(buf, 1, 1) != 0) {
    pr_info("%s", "GPT: Failed to read GPT header\n");
    pmm_free_page(buf);
    return;
  }

  struct gpt_header *header = (struct gpt_header *)buf;

  /* 2. Check Signature */
  if (header->signature != GPT_SIGNATURE) {
    pr_info("GPT: Invalid signature: 0x%016lx\n", header->signature);
    pmm_free_page(buf);
    return;
  }

  pr_info("GPT: Valid signature found. Entries: %d @ LBA %ld\n",
          header->num_partition_entries, header->partition_entry_lba);

  /* 3. Read Partition Entries */
  /*
   * Usually entries start at LBA 2.
   * Entry size is usually 128 bytes.
   * We need to read enough sectors to cover all entries we care about.
   * Let's say we read 4 sectors (2048 bytes) -> 16 entries (128 bytes each).
   */
  /* 3. Read Partition Entries */
  uint64_t entries_lba = header->partition_entry_lba;
  uint32_t entry_size = header->partition_entry_size;
  uint32_t num_entries = header->num_partition_entries;

  /* Reuse buffer to read entries */
  /* We assume entries fit in one page (4096 bytes / 128 = 32 entries) */
  /* Read 8 sectors = 4096 bytes */
  if (virtio_blk_read(buf, entries_lba, 8) != 0) {
    pr_info("%s", "GPT: Failed to read partition entries\n");
    pmm_free_page(buf);
    return;
  }

  /* Parse entries */
  num_partitions = 0;

  for (uint32_t i = 0; i < 32 && i < num_entries; i++) {
    struct gpt_partition_entry *entry =
        (struct gpt_partition_entry *)(buf + i * entry_size);

    /* Check if entry is used (Type GUID != 0) */
    int is_unused = 1;
    for (int k = 0; k < 16; k++) {
      if (entry->type_guid[k] != 0) {
        is_unused = 0;
        break;
      }
    }

    if (is_unused)
      continue;

    if (num_partitions >= MAX_PARTITIONS)
      break;

    struct partition *p = &partitions[num_partitions];
    p->index = num_partitions;
    p->start_lba = entry->start_lba;
    p->end_lba = entry->end_lba;
    p->size_sectors = entry->end_lba - entry->start_lba + 1;
    memcpy(p->type_guid, entry->type_guid, 16);

    pr_info("GPT: Partition %d: Start=%ld, Size=%ld sectors\n", i, p->start_lba,
            p->size_sectors);

    num_partitions++;
  }

  pmm_free_page(buf);
  pr_info("GPT: Found %d partitions\n", num_partitions);
}

struct partition *gpt_get_partition(int index) {
  if (index < 0 || index >= num_partitions)
    return NULL;
  return &partitions[index];
}
