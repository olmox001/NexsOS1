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

static void mbr_init(uint8_t *buf) {
  struct mbr *m = (struct mbr *)buf;
  if (m->signature != MBR_SIGNATURE) {
    pr_err("MBR: Invalid signature: 0x%04x\n", m->signature);
    return;
  }

  pr_info("%s", "MBR: Valid signature found. Scanning partitions...\n");
  num_partitions = 0;

  for (int i = 0; i < 4; i++) {
    struct mbr_entry *e = &m->partitions[i];
    if (e->type == 0)
      continue;

    /* Map MBR partition 1..4 to our internal index 1..4 (or 0..3)
     * For compatibility with ext4_init(2), we use the MBR slot index + 1. */
    int idx = i + 1;
    if (idx >= MAX_PARTITIONS)
      continue;

    struct partition *p = &partitions[idx];
    p->index = idx;
    p->start_lba = e->lba_start;
    p->size_sectors = e->sectors;
    p->end_lba = p->start_lba + p->size_sectors - 1;
    memset(p->type_guid, 0, 16);
    p->type_guid[0] = e->type;

    pr_info("MBR: Partition %d: Type=0x%02x, Start=%ld, Size=%ld sectors\n", idx,
            e->type, p->start_lba, p->size_sectors);

    if (idx >= num_partitions)
      num_partitions = idx + 1;
  }
}

void gpt_init(void) {
  pr_info("%s", "Partition: Initializing...\n");

  /* Allocate buffer for reading sectors */
  uint8_t *buf = (uint8_t *)pmm_alloc_page();
  if (!buf) {
    pr_info("%s", "Partition: Failed to allocate buffer\n");
    return;
  }

  /* 1. Try reading GPT Header (LBA 1) */
  if (virtio_blk_read(buf, 1, 1) != 0) {
    pr_info("%s", "Partition: Failed to read LBA 1\n");
    pmm_free_page(buf);
    return;
  }

  struct gpt_header *header = (struct gpt_header *)buf;

  /* 2. Check GPT Signature. If not found, try MBR on LBA 0. */
  if (header->signature != GPT_SIGNATURE) {
    pr_info("%s", "GPT: Invalid signature. Falling back to MBR...\n");
    if (virtio_blk_read(buf, 0, 1) != 0) {
      pr_err("%s", "Partition: Failed to read LBA 0 (MBR)\n");
      pmm_free_page(buf);
      return;
    }
    mbr_init(buf);
    pmm_free_page(buf);
    return;
  }

  /* 2.1 Verify Header CRC32 */
  uint32_t orig_crc = header->header_crc32;
  header->header_crc32 = 0;
  uint32_t calc_crc = crc32(header, header->header_size);
  header->header_crc32 = orig_crc;

  if (calc_crc != orig_crc) {
    pr_err("GPT: Header CRC mismatch! (calc: 0x%08x, orig: 0x%08x). Falling back to MBR.\n", calc_crc,
           orig_crc);

           if (virtio_blk_read(buf, 0, 1) != 0) {
      pr_err("%s", "Partition: Failed to read LBA 0 (MBR fallback)\n");
      pmm_free_page(buf);
      return;
    }
    mbr_init(buf);
    pmm_free_page(buf);
    return; 
  }  

  pr_info("GPT: Valid signature found. Entries: %d @ LBA %ld\n",
          header->num_partition_entries, header->partition_entry_lba);

  /* 3. Read Partition Entries */
  uint64_t entries_lba = header->partition_entry_lba;
  uint32_t entry_size = header->partition_entry_size;
  uint32_t num_entries = header->num_partition_entries;
  uint32_t entries_crc = header->partition_entry_crc32;

  /* Calculate total size and pages needed */
  uint32_t total_entries_size = num_entries * entry_size;
  uint32_t num_pages = (total_entries_size + PAGE_SIZE - 1) / PAGE_SIZE;
  uint32_t sectors_to_read = (total_entries_size + 511) / 512;

  /* Allocate buffer for all partition entries if needed */
  uint8_t *entries_buf = buf;
  if (num_pages > 1) {
    /* Allocate additional pages for partition entries */
    entries_buf = (uint8_t *)pmm_alloc_pages(num_pages);
    if (!entries_buf) {
      pr_warn("GPT: Cannot allocate %u pages for entries, limiting to 32\n", num_pages);
      total_entries_size = 32 * entry_size;
      sectors_to_read = (total_entries_size + 511) / 512;
      entries_buf = buf;
    }
  }

  if (virtio_blk_read(entries_buf, entries_lba, sectors_to_read) != 0) {
    pr_info("%s", "GPT: Failed to read partition entries\n");
    if (entries_buf != buf) pmm_free_pages(entries_buf, num_pages);
    pmm_free_page(buf);
    return;
  }

  /* 3.1 Verify Partition Entries CRC32 */
  uint32_t calc_entries_crc = crc32(entries_buf, total_entries_size);
  if (calc_entries_crc != entries_crc) {
    pr_err("GPT: Partition Entries CRC mismatch! (calc: 0x%08x, orig: 0x%08x)\n",
           calc_entries_crc, entries_crc);
  }

  /* Parse entries */
  num_partitions = 0;
  uint32_t entries_to_read = total_entries_size / entry_size;

  for (uint32_t i = 0; i < entries_to_read; i++) {
    struct gpt_partition_entry *entry =
        (struct gpt_partition_entry *)(entries_buf + i * entry_size);

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

  /* Cleanup allocated buffers */
  if (entries_buf != buf) pmm_free_pages(entries_buf, num_pages);
  pmm_free_page(buf);
  pr_info("GPT: Found %d partitions\n", num_partitions);
}

struct partition *gpt_get_partition(int index) {
  if (index < 0 || index >= num_partitions)
    return NULL;
  return &partitions[index];
}
