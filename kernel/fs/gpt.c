/*
 * kernel/fs/gpt.c
 * GPT Partition Table Parser
 *
 * Purpose:
 *   Reads and validates the GUID Partition Table (GPT) from the block device,
 *   populates the global partitions[] array, and exposes gpt_get_partition() so
 *   ext4_init() can locate its partition by index.  Falls back to the legacy
 *   Master Boot Record (MBR) format if the GPT signature or header CRC32 fails.
 *
 * On-disk layout accessed:
 *   LBA 0  (512 bytes) : MBR (fallback; boot code + 4 × mbr_entry + signature)
 *   LBA 1  (512 bytes) : GPT header (struct gpt_header, 92 bytes used)
 *   LBA 2+ (variable)  : GPT partition entries at header->partition_entry_lba;
 *                        typically 128 entries × 128 bytes = 16 384 bytes = 32
 *                        sectors starting at LBA 2.
 *
 * Key invariants:
 *   - partitions[] is indexed 0-based by GPT path; 1-based by MBR path.
 *     NOTE(GPT-02): ext4_init() calls gpt_get_partition(2), meaning index 2
 *     of partitions[].  On GPT that is the third parsed entry (GPT entries are
 *     0-indexed into partitions[]).  On MBR that is MBR slot 1 (0-indexed MBR
 *     slot i fills partitions[i+1]).  The asymmetry is undocumented; changing
 *     the disk image layout will silently mount the wrong partition.
 *   - CRC32 uses the reflected IEEE-802.3 / PKZIP polynomial (0xEDB88320),
 *     init 0xFFFFFFFF, final XOR 0xFFFFFFFF — identical to mkdisk.c.
 *
 * Known issues:
 *   GPT-01  (W3 REFINE, FIXED) Partition-entry CRC mismatch now aborts to the
 *           MBR parser (matching header-CRC behaviour), instead of parsing
 *           entries[] that may contain corrupt start/end LBAs.
 *   GPT-02  (W2 BAD-IMPL) MBR path fills partitions[i+1] (1-based); GPT path
 *           fills partitions[num_partitions] (0-based).  ext4_init(2) gets
 *           different physical partitions depending on the table type.
 *   GPT-03  (W1 REFINE) When num_pages > 1 and pmm_alloc_pages() fails, the
 *           fallback rewrites total_entries_size and sectors_to_read for 32
 *           entries (32 × entry_size bytes), which fits in the single-page
 *           buf[] (4096 bytes) only when entry_size ≤ 128.  Standard GPT uses
 *           entry_size == 128, so 32 × 128 = 4096, which is exactly the page
 *           boundary.  For non-standard larger entry sizes the fallback would
 *           read more bytes than buf[] holds.
 */
#include <kernel/block.h>
#include <kernel/gpt.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>

/* partitions[]: in-memory table populated by gpt_init() or mbr_init().
 * Indexed 0-based on the GPT path, 1-based on the MBR path (GPT-02). */
struct partition partitions[MAX_PARTITIONS];
/* num_partitions: count of valid entries written into partitions[]. */
int num_partitions = 0;

/*
 * mbr_init - parse a legacy MBR from an already-read 512-byte sector buffer.
 *
 * @buf: pointer to a 512-byte buffer containing LBA 0 (the MBR sector).
 *
 * Validates the MBR signature (0xAA55 at bytes 510–511), then iterates the
 * four 16-byte MBR partition entries at offset 446 (bytes 446–509).  Each
 * non-empty slot (type != 0) is mapped to partitions[i+1], where i is the
 * 0-based MBR slot index (0..3).  This 1-based mapping leaves partitions[0]
 * unset when called from the MBR path.
 *
 * On-disk MBR layout (struct mbr, gpt.h):
 *   bytes   0–445: boot code
 *   bytes 446–461: mbr_entry[0]  (status, chs_start[3], type, chs_end[3],
 *                                  lba_start u32, sectors u32 = 16 bytes)
 *   bytes 462–477: mbr_entry[1]
 *   bytes 478–493: mbr_entry[2]
 *   bytes 494–509: mbr_entry[3]
 *   bytes 510–511: signature (0xAA55, little-endian)
 *
 * Side effects:
 *   Writes to partitions[1..4] and num_partitions (global state).
 *   No disk I/O (buf already contains the sector contents).
 *
 * NOTE(GPT-02): MBR fills partitions[i+1] (1-based); GPT fills
 * partitions[num_partitions] (0-based).  The resulting asymmetry means
 * gpt_get_partition(2) returns the 2nd MBR partition but the 3rd GPT entry.
 */
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
    /* NOTE(GPT-02): idx = i+1 means MBR slot 0 → partitions[1], ...,
     * slot 3 → partitions[4].  partitions[0] is left zeroed.  The GPT
     * path fills from index 0; ext4_init(2) therefore hits a different
     * physical partition depending on which table type was found. */
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

/*
 * gpt_init - detect and parse the partition table from the block device.
 *
 * Reads LBA 1 (512 bytes) via block_read(), attempts to validate the
 * GPT header signature ("EFI PART", 0x5452415020494645) and its CRC32, then
 * reads and validates the partition entries.  Falls back to the MBR parser
 * (mbr_init()) if the GPT signature is absent or the header CRC32 does not
 * match.
 *
 * GPT header CRC32 protocol (verified correct; matches mkdisk.c):
 *   1. Save header->header_crc32.
 *   2. Zero header->header_crc32 in the buffer.
 *   3. Compute crc32(header, header->header_size) — standard IEEE-802.3
 *      reflected polynomial, init 0xFFFFFFFF, final XOR 0xFFFFFFFF.
 *   4. Restore header->header_crc32.
 *   5. Compare computed vs saved; mismatch → MBR fallback.
 *
 * Partition-entry CRC32 (GPT-01):
 *   Computed over total_entries_size bytes at entries_lba.
 *   Mismatch is logged but parsing continues — entries may contain corrupt
 *   start/end LBAs.  See NOTE(GPT-01).
 *
 * Sector arithmetic:
 *   total_entries_size = num_entries × entry_size
 *     (standard: 128 × 128 = 16 384 bytes = 32 sectors at LBA 2)
 *   sectors_to_read    = (total_entries_size + 511) / 512
 *   num_pages          = (total_entries_size + PAGE_SIZE - 1) / PAGE_SIZE
 *
 * Buffer strategy:
 *   A single PMM page (4096 bytes) is allocated for the GPT header read.
 *   If the entries array is also ≤ 4096 bytes (num_pages == 1), the same
 *   page is reused for entries.  If larger, pmm_alloc_pages(num_pages) is
 *   called for a dedicated entries buffer; on failure the code falls back to
 *   the single-page buf[] and caps reads to 32 entries.
 *   NOTE(GPT-03): The 32-entry fallback limit assumes entry_size ≤ 128 bytes
 *   (32 × 128 = 4096 = one page); for non-standard larger entry sizes this
 *   would overrun buf[].
 *
 * Side effects:
 *   Writes partitions[] and num_partitions (global state).
 *   Two or more block_read() calls; one or more PMM alloc/free cycles.
 *
 * Returns: void; callers use gpt_get_partition() to query results.
 */
void gpt_init(void) {
  pr_info("%s", "Partition: Initializing...\n");

  /* Allocate buffer for reading sectors */
  uint8_t *buf = (uint8_t *)pmm_alloc_page();
  if (!buf) {
    pr_info("%s", "Partition: Failed to allocate buffer\n");
    return;
  }

  /* 1. Try reading GPT Header (LBA 1) */
  if (block_read(buf, 1, 1) != 0) {
    pr_info("%s", "Partition: Failed to read LBA 1\n");
    pmm_free_page(buf);
    return;
  }

  struct gpt_header *header = (struct gpt_header *)buf;

  /* 2. Check GPT Signature. If not found, try MBR on LBA 0. */
  if (header->signature != GPT_SIGNATURE) {
    pr_info("%s", "GPT: Invalid signature. Falling back to MBR...\n");
    if (block_read(buf, 0, 1) != 0) {
      pr_err("%s", "Partition: Failed to read LBA 0 (MBR)\n");
      pmm_free_page(buf);
      return;
    }
    mbr_init(buf);
    pmm_free_page(buf);
    return;
  }

  /* 2.1 Verify Header CRC32 */
  /* GPT spec: zero the CRC field in the buffer, compute CRC over the first
   * header_size bytes (typically 92), then restore the field.  crc32() here
   * uses the reflected IEEE-802.3 polynomial (0xEDB88320), init 0xFFFFFFFF,
   * final XOR 0xFFFFFFFF — the same implementation as mkdisk.c (verified). */
  uint32_t orig_crc = header->header_crc32;
  header->header_crc32 = 0;
  uint32_t calc_crc = crc32(header, header->header_size);
  header->header_crc32 = orig_crc;

  if (calc_crc != orig_crc) {
    pr_err("GPT: Header CRC mismatch! (calc: 0x%08x, orig: 0x%08x). Falling back to MBR.\n", calc_crc,
           orig_crc);

           if (block_read(buf, 0, 1) != 0) {
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
  /* entry_size: from the GPT header; typically 128 bytes per spec. */
  uint32_t entry_size = header->partition_entry_size;
  uint32_t num_entries = header->num_partition_entries;
  /* entries_crc: CRC32 of the raw entries array as stored in the header. */
  uint32_t entries_crc = header->partition_entry_crc32;

  /* Calculate total size and pages needed */
  /* Standard case: 128 entries × 128 bytes = 16 384 bytes = 32 sectors.
   * sectors_to_read = ceil(total_entries_size / 512) using integer arithmetic.
   * num_pages       = ceil(total_entries_size / PAGE_SIZE). */
  uint32_t total_entries_size = num_entries * entry_size;
  uint32_t num_pages = (total_entries_size + PAGE_SIZE - 1) / PAGE_SIZE;
  uint32_t sectors_to_read = (total_entries_size + 511) / 512;

  /* Allocate buffer for all partition entries if needed */
  uint8_t *entries_buf = buf;
  if (num_pages > 1) {
    /* Allocate additional pages for partition entries */
    entries_buf = (uint8_t *)pmm_alloc_pages(num_pages);
    if (!entries_buf) {
      /* NOTE(GPT-03): fallback: cap to 32 entries and reuse the single-page
       * buf[4096].  32 × entry_size must be ≤ 4096, which holds only when
       * entry_size ≤ 128.  Standard GPT uses entry_size == 128 exactly
       * (32 × 128 = 4096), so the fallback is safe on standard images. */
      pr_warn("GPT: Cannot allocate %u pages for entries, limiting to 32\n", num_pages);
      total_entries_size = 32 * entry_size;
      sectors_to_read = (total_entries_size + 511) / 512;
      entries_buf = buf;
    }
  }

  if (block_read(entries_buf, entries_lba, sectors_to_read) != 0) {
    pr_info("%s", "GPT: Failed to read partition entries\n");
    if (entries_buf != buf) pmm_free_pages(entries_buf, num_pages);
    pmm_free_page(buf);
    return;
  }

  /* 3.1 Verify Partition Entries CRC32 */
  /* CRC computed over the exact total_entries_size bytes starting at
   * entries_buf.  GPT-01 (fixed): a mismatch now aborts to the MBR parser,
   * matching the header-CRC behaviour above, instead of parsing entries that
   * may carry corrupt start/end LBAs. */
  uint32_t calc_entries_crc = crc32(entries_buf, total_entries_size);
  if (calc_entries_crc != entries_crc) {
    pr_err("GPT: Partition Entries CRC mismatch! (calc: 0x%08x, orig: 0x%08x). Falling back to MBR.\n",
           calc_entries_crc, entries_crc);
    /* GPT-01: the entries array is untrustworthy — do not parse it.  Abort to
     * the MBR parser, mirroring the header-CRC fallback above. */
    if (entries_buf != buf) pmm_free_pages(entries_buf, num_pages);
    if (block_read(buf, 0, 1) != 0) {
      pr_err("%s", "Partition: Failed to read LBA 0 (MBR fallback)\n");
      pmm_free_page(buf);
      return;
    }
    mbr_init(buf);
    pmm_free_page(buf);
    return;
  }

  /* Parse entries */
  /* entries_to_read = actual number of slots in the buffer; may be less than
   * num_entries when the fallback capped total_entries_size to 32 × entry_size. */
  num_partitions = 0;
  uint32_t entries_to_read = total_entries_size / entry_size;

  for (uint32_t i = 0; i < entries_to_read; i++) {
    struct gpt_partition_entry *entry =
        (struct gpt_partition_entry *)(entries_buf + i * entry_size);

    /* Check if entry is used (Type GUID != 0) */
    /* GPT spec: an all-zero type_guid marks an unused partition slot;
     * any non-zero byte in the 16-byte GUID indicates a valid partition. */
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

    /* Populate the in-memory partition descriptor.
     * NOTE(GPT-02): GPT path fills partitions[0], [1], [2], … (0-based).
     * MBR path fills partitions[1], [2], [3], [4] (1-based).  The caller
     * ext4_init() requests index 2: the 3rd GPT entry vs. the 2nd MBR slot. */
    struct partition *p = &partitions[num_partitions];
    p->index = num_partitions;
    p->start_lba = entry->start_lba;
    p->end_lba = entry->end_lba;
    /* size_sectors: inclusive range; +1 because start_lba..end_lba both belong
     * to the partition (e.g. start=34, end=34849 → 34816 sectors). */
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

/*
 * gpt_get_partition - return a pointer to an in-memory partition descriptor.
 *
 * @index: 0-based index into partitions[] (GPT path) or 1-based (MBR path).
 *         ext4_init() passes 2, which on a GPT disk is the third parsed GPT
 *         entry and on an MBR disk is the second MBR partition slot.
 *         NOTE(GPT-02): the index semantics differ between GPT and MBR paths.
 *
 * Precondition: gpt_init() must have been called; no locking is performed.
 *
 * Returns: pointer into partitions[] on success; NULL if index is out of
 *          range [0, num_partitions).  Does not distinguish GPT vs. MBR.
 *
 * Side effects: none.
 */
struct partition *gpt_get_partition(int index) {
  if (index < 0 || index >= num_partitions)
    return NULL;
  return &partitions[index];
}

/*
 * partition_role - decode the NEXS role from a partition's type GUID.
 *
 * The GUID is stored little-endian on disk, exactly as GPT specifies for the
 * first three fields: data1 (4 bytes) and data2 (2 bytes) are LE, data4 is a
 * byte string.  So "NEXS" (0x4E455853) reads back as 53 58 45 4E, and the role
 * (data2) as its two bytes low-first.
 *
 * Returns the role, or 0 for any partition that is not ours — a foreign
 * partition, or an image built before roles existed.  0 is not a valid role,
 * so callers can treat it as "unknown" without a separate flag.
 */
uint16_t partition_role(const struct partition *p) {
  if (!p)
    return 0;
  static const uint8_t nexs_d1[4] = {0x53, 0x58, 0x45, 0x4E}; /* "NEXS" LE */
  for (int i = 0; i < 4; i++)
    if (p->type_guid[i] != nexs_d1[i])
      return 0;
  /* data3 + data4 pin the rest of the namespace, so a coincidental match on
   * the first four bytes cannot be mistaken for one of our roles. */
  if (p->type_guid[6] != 0x58 || p->type_guid[7] != 0x4E)
    return 0;
  return (uint16_t)((uint16_t)p->type_guid[4] |
                    ((uint16_t)p->type_guid[5] << 8));
}

/*
 * partition_find_by_role - locate a partition by what it IS.
 *
 * This is the seam that retires GPT-02: callers stop passing an index whose
 * meaning silently differs between the GPT and MBR paths, and stop depending on
 * the physical ordering of the table.  An installer may lay the disk out in any
 * order, and re-partitioning cannot move the root out from under the kernel.
 */
struct partition *partition_find_by_role(uint16_t role) {
  if (role == 0)
    return NULL;
  for (int i = 0; i < num_partitions; i++)
    if (partition_role(&partitions[i]) == role)
      return &partitions[i];
  return NULL;
}
