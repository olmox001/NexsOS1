/*
 * kernel/mm/pmm.c
 * Physical Memory Manager (PMM)
 *
 * Implements the lowest layer of the OS1/NEXS memory subsystem: page-frame
 * allocation over two zones (DMA and Normal).  The allocator is split into two
 * phases:
 *
 *   pmm_early_init()  - validates the boot memory map, calculates per-frame
 *                        metadata sizes, and places page_array + bitmaps in
 *                        the lowest usable interval above the kernel image
 *                        that is already reachable through bootstrap tables.
 *                        The bootstrap MMU is active; the full VMM map is not.
 *
 *   pmm_init()        - initialises the two zone descriptors, marks the kernel
 *                        image and PMM metadata as reserved, and reserves any
 *                        non-USABLE regions reported by the bootloader.
 *
 * Allocation primitives:
 *   pmm_alloc_page()    - next-fit single page (Normal first, DMA fallback).
 *   pmm_alloc_pages()   - contiguous multi-page block from ZONE_NORMAL only.
 *   pmm_alloc_aligned() - aligned contiguous block from ZONE_NORMAL only.
 *   pmm_free_page()     - returns a page; poisons it 0xCC; panics on double-free.
 *
 * PA/VA contract (MM-PMM-07 RESOLVED, Phase B2 sweep):
 *   pmm_alloc_*() return KERNEL VIRTUAL pointers obtained through
 *   phys_to_virt(MEMORY_BASE + pfn_to_phys(pfn)); pmm_free_*() accept the
 *   same pointers and translate back with virt_to_phys().  Callers that
 *   need the physical address (PTE output addresses, DMA descriptors)
 *   must use virt_to_phys() explicitly.  All PMM metadata (page_array,
 *   zone bitmaps) is addressed through phys_to_virt() as well.  With
 *   KERNEL_VIRT_BASE == 0 (memlayout.h) this is bit-identical to the old
 *   identity behaviour.
 *
 * Known issues:
 *   MM-PMM-01  (W3 STUB)         pmm_init_region() is a no-op stub.
 *   MM-PMM-02  RESOLVED (Phase B2): pmm_alloc_pages (and therefore
 *                                pmm_alloc_aligned) now clean+fence like the
 *                                single-page path; safe for DMA buffers.
 *   MM-PMM-03  (W2 PERF)         Contiguous alloc is an O(n) scan from PFN 0.
 *   MM-PMM-04  (W2 WRONG-DESIGN) pmm_alloc_pages/aligned search ZONE_NORMAL only.
 *   MM-PMM-05  (W2 BAD-IMPL)     global free_pages (atomic) vs per-zone free_pages
 *                                (plain under lock) can desync.
 *   MM-PMM-06  (W1 REFINE)       next_free_pfn is ignored by the contiguous path.
 *   MM-PMM-07  RESOLVED (Phase B2): VA/PA separation via memlayout.h (see above).
 *   MM-PMM-08  RESOLVED (#117):  total_pages spans up to the HIGHEST usable end
 *                                address, so address-space holes inside the span
 *                                (amd64 PCI hole 3..4GB, VGA/BIOS 640KB..1MB)
 *                                used to be free allocatable "RAM".  pmm_init()
 *                                now reserves every PFN not covered by a USABLE
 *                                region; the boot log reports the real usable
 *                                sum next to the span.
 */
#include <hal/config.h>
#include <kernel/memlayout.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

/* Memory configuration from architecture */
#define MEMORY_BASE HAL_RAM_START
#define DMA_ZONE_END (MEMORY_BASE + 0x1000000UL) /* 16MB for DMA zone */

/* Forward declaration */
void pmm_init_region(uint64_t base, uint64_t size);

/*
 * pmm_init_region - register an additional usable RAM region with the PMM.
 *
 * NOTE(MM-PMM-01): This function is a stub.  It prints the region address and
 * returns without updating any zone bitmap or metadata.  Multi-region
 * management beyond the single early-init scan is not implemented.
 *
 * Parameters:
 *   base  - physical start address of the region.
 *   size  - length in bytes.
 */
void pmm_init_region(uint64_t base, uint64_t size) {
  /* Mark pages in this region as usable (they are already 0 in bitmap if zone_init was called) */
  /* This is a placeholder; real implementation would manage multiple regions. */
  pr_info("PMM: Region added 0x%lx - 0x%lx\n", base, base + size);
}

/* Pointers to dynamic metadata */
/* page_array: flat array of struct page, one entry per page frame (PFN 0..total_pages-1).
 * Placed immediately after the kernel image by pmm_early_init().
 * Accessed under the appropriate zone lock (or atomically for free_pages).
 *
 * NOTE(MM-PMM-07): page_array is addressed using the identity-map assumption
 * (the physical address returned by the boot stage IS the usable pointer). */
static struct page *page_array = NULL;
static struct zone zones[ZONE_COUNT];
/* dma_bitmap / normal_bitmap: one bit per PFN in each zone; 1 = allocated/reserved, 0 = free.
 * Stored contiguously in memory immediately after page_array. */
static uint64_t *dma_bitmap = NULL;
static uint64_t *normal_bitmap = NULL;

/* Global statistics */
/* total_pages: immutable after pmm_init(); no lock needed for reads after init. */
static uint64_t total_pages;
/* free_pages: updated with atomic GCC built-ins (__sync_fetch_and_{add,sub}).
 * NOTE(MM-PMM-05): Per-zone zone->free_pages is updated under the zone spinlock
 * (non-atomic), while this global is atomic.  The two counters can disagree
 * transiently if a CPU is preempted between the two updates. */
static uint64_t free_pages;
/* usable_pages: sum of the USABLE boot regions in pages (MM-PMM-08 #117).
 * total_pages is the metadata SPAN (up to the highest usable end address) and
 * may exceed this when the map has holes; immutable after pmm_early_init(). */
static uint64_t usable_pages;
/* pmm_metadata_end_phys: physical end of the page_array + bitmaps, set in
 * pmm_early_init().  vmm_init() maps the bootstrap window up to here so the
 * metadata is reachable after the CR3/TTBR switch even when a boot module
 * pushed it far past the kernel image. */
static uint64_t pmm_metadata_end_phys;

/* --- Instrumentation counters (perf brief §1; surfaced via OS1_sys_stats).
 * Updated with relaxed atomics off the lock so the hot path pays only a single
 * fetch-and-add; *_max uses a racy compare (a lost update on a stat is benign).
 * search_ticks_* are raw hal_timer_get_count() ticks; the collector converts
 * to ns via timer_counts_to_ns().  These measure the bitmap-SEARCH cost only —
 * the fragmentation-sensitive part — not the memset/cache-clean tail. */
static uint64_t stat_alloc_calls;
static uint64_t stat_free_calls;
static uint64_t stat_alloc_search_ticks_total;
static uint64_t stat_alloc_search_ticks_max;

uint64_t pmm_metadata_top(void) { return pmm_metadata_end_phys; }

/*
 * Mark a page as used in the bitmap
 *
 * bitmap_set - mark PFN 'bit' as allocated (1) in 'bitmap'.
 *
 * Must be called with the owning zone->lock held.
 * Uses 64-bit words; 'bit' is a zone-relative PFN.
 */
static void bitmap_set(uint64_t *bitmap, uint64_t bit) {
  bitmap[bit / 64] |= (1UL << (bit % 64));
}

/*
 * Mark a page as free in the bitmap
 *
 * bitmap_clear - mark PFN 'bit' as free (0) in 'bitmap'.
 *
 * Must be called with the owning zone->lock held.
 */
static void bitmap_clear(uint64_t *bitmap, uint64_t bit) {
  bitmap[bit / 64] &= ~(1UL << (bit % 64));
}

/*
 * Check if a page is free
 *
 * bitmap_test - return non-zero if PFN 'bit' is allocated (1), zero if free.
 *
 * Must be called with the owning zone->lock held.
 */
static int bitmap_test(uint64_t *bitmap, uint64_t bit) {
  return (bitmap[bit / 64] & (1UL << (bit % 64))) != 0;
}

/*
 * Find first free bit in bitmap
 *
 * bitmap_find_free - find the first free (0) bit in [start, end).
 *
 * Must be called with the owning zone->lock held.
 * Returns the zone-relative PFN of the first free frame, or -1 if none.
 *
 * MM-PMM-03 RESOLVED (perf §3.1): word-level fast path.  A fully-allocated
 * 64-bit word (== ~0) is skipped in one compare instead of 64 bit tests, so the
 * scan cost tracks the number of *occupied words* before the first hole, not the
 * absolute PFN.  The first non-full word's free bit is located with a single
 * __builtin_ctzll(~w).  Sub-word head/tail are handled bit-by-bit.
 */
static int64_t bitmap_find_free(uint64_t *bitmap, uint64_t start,
                                uint64_t end) {
  uint64_t i = start;

  /* Head: advance to a word boundary one bit at a time (< 64 iterations). */
  while (i < end && (i & 63)) {
    if (!bitmap_test(bitmap, i))
      return (int64_t)i;
    i++;
  }
  /* Body: whole 64-bit words.  Skip fully-allocated words in one compare. */
  for (; i + 64 <= end; i += 64) {
    uint64_t w = bitmap[i / 64];
    if (w != ~0ULL)
      return (int64_t)(i + (uint64_t)__builtin_ctzll(~w));
  }
  /* Tail: remaining < 64 bits. */
  for (; i < end; i++) {
    if (!bitmap_test(bitmap, i))
      return (int64_t)i;
  }
  return -1;
}

/*
 * Find contiguous free pages
 *
 * bitmap_find_contiguous - find a run of 'count' consecutive free bits
 * in [start, end).
 *
 * Must be called with the owning zone->lock held.
 * Returns the zone-relative PFN of the first frame in the run, or -1.
 *
 * MM-PMM-03 (partial, perf §3.1): a fully-allocated 64-bit word (== ~0) that is
 * word-aligned and breaks the current run is skipped in one compare instead of
 * 64 bit tests — the dominant cost when scanning past occupied memory.  The
 * run accounting stays bit-exact across word boundaries (a partial or non-full
 * word is still walked bit-by-bit), so correctness is unchanged.
 * NOTE(MM-PMM-06): still scans from 'start' (callers pass 0); a next_free_pfn
 * hint for the contiguous path is a further refinement, not done here.
 */
static int64_t bitmap_find_contiguous(uint64_t *bitmap, uint64_t start,
                                      uint64_t end, uint64_t count) {
  uint64_t found = 0;
  uint64_t first = 0;
  uint64_t i = start;

  while (i < end) {
    /* Run is broken and we're at a word boundary: skip a fully-allocated word
     * in one compare. */
    if (found == 0 && (i & 63) == 0 && i + 64 <= end &&
        bitmap[i / 64] == ~0ULL) {
      i += 64;
      continue;
    }
    if (!bitmap_test(bitmap, i)) {
      if (found == 0)
        first = i;
      found++;
      if (found == count)
        return (int64_t)first;
    } else {
      found = 0;
    }
    i++;
  }
  return -1;
}

/*
 * Initialize a zone
 *
 * zone_init - initialise a struct zone descriptor for a contiguous PFN range.
 *
 * Parameters:
 *   z         - zone descriptor to initialise.
 *   name      - human-readable name (e.g. "DMA", "Normal").
 *   start_pfn - first absolute PFN in this zone.
 *   end_pfn   - one past the last absolute PFN in this zone.
 *   bitmap    - pre-allocated bitmap storage (caller must size it as
 *               ceil((end_pfn - start_pfn) / 64) * 8 bytes).
 *
 * Clears the bitmap (all pages initially free) and initialises the zone lock.
 * Called from pmm_init() before any allocations are made.
 */
static void zone_init(struct zone *z, const char *name, uint64_t start_pfn,
                      uint64_t end_pfn, uint64_t *bitmap) {
  z->name = name;
  z->start_pfn = start_pfn;
  z->end_pfn = end_pfn;
  z->free_pages = 0;
  z->bitmap = bitmap;
  z->next_free_pfn = 0;
  spin_lock_init(&z->lock);

  /* Clear bitmap - all pages initially free */
  uint64_t npages = end_pfn - start_pfn;
  memset(bitmap, 0, (npages + 63) / 64 * 8);
  z->free_pages = npages;
}

/*
 * Early PMM initialization - allocate metadata
 *
 * pmm_early_init - phase-1 PMM setup: discover RAM extent and place metadata.
 *
 * Called during K1 after the boot MMU/direct map is active, but before PMM
 * allocation and VMM reconfiguration. Scans the mem_region table to determine the highest
 * usable physical end (rejecting extents beyond HAL_PHYS_MAP_LIMIT), computes the sizes
 * of page_array, dma_bitmap, and normal_bitmap, and places them contiguously
 * in the lowest sufficiently large usable interval above __kernel_end while
 * avoiding all reserved overlays.
 *
 * No guessed RAM fallback is used: the platform must provide a validated map
 * or complete its explicitly selected legacy page-probe path before calling.
 *
 * After this function returns:
 *   - page_array, dma_bitmap, normal_bitmap global pointers are valid.
 *   - total_pages is set.
 *   - No zone is initialised; no allocations are possible yet.
 *
 * Locking: must be called single-threaded (before SMP init).
 * IRQ context: IRQs are not enabled at call time.
 *
 * Parameters:
 *   regions - array of mem_region entries from the boot platform.
 *   count   - number of entries in 'regions'.
 *
 * PA/VA: metadata physical addresses are converted through phys_to_virt();
 * the active direct map is VA = PA + KERNEL_VIRT_BASE, not identity mapping.
 */

static uint64_t pmm_max_u64(uint64_t a, uint64_t b) {
  return a > b ? a : b;
}

static uint64_t pmm_align_up_page(uint64_t value) {
  const uint64_t mask = PAGE_SIZE - 1ULL;
  if (value > UINT64_MAX - mask)
    return UINT64_MAX;
  return (value + mask) & ~mask;
}

/* Count the union of complete usable pages inside the PMM's physical span.
 * The input table is not required to be sorted or disjoint. */
uint64_t pmm_calculate_usable_bytes(struct mem_region *regions,
                                    size_t count, uint64_t base,
                                    uint64_t span_end) {
  if (!regions || count == 0 || span_end <= base)
    return 0;
  uint64_t cursor = base;
  uint64_t total = 0;

  while (cursor < span_end) {
    uint64_t covered_end = cursor;
    uint64_t next_start = UINT64_MAX;

    for (size_t i = 0; i < count; i++) {
      if (regions[i].type != MEM_REGION_USABLE || regions[i].size == 0)
        continue;
      if (regions[i].base > UINT64_MAX - regions[i].size)
        panic("PMM: overflowing usable region base=0x%lx size=0x%lx",
              regions[i].base, regions[i].size);

      uint64_t start = regions[i].base;
      uint64_t end = start + regions[i].size;
      if (end <= base || start >= span_end)
        continue;
      if (start < base)
        start = base;
      if (end > span_end)
        end = span_end;

      start = pmm_align_up_page(start);
      end &= ~((uint64_t)PAGE_SIZE - 1ULL);
      if (end <= start || end <= cursor)
        continue;

      if (start <= cursor) {
        if (end > covered_end)
          covered_end = end;
      } else if (start < next_start) {
        next_start = start;
      }
    }

    if (covered_end > cursor) {
      uint64_t segment = covered_end - cursor;
      if (total > UINT64_MAX - segment)
        panic("%s", "PMM: usable byte union overflow");
      total += segment;
      cursor = covered_end;
    } else if (next_start != UINT64_MAX) {
      cursor = next_start;
    } else {
      break;
    }
  }

  return total;
}

/*
 * pmm_early_init - phase-1 PMM setup: discover the physical span and place
 * metadata in a complete usable-page interval above the kernel image and
 * inside the bootstrap direct-map window (before VMM installs the full map).
 *
 * MEMORY_BASE is the first address represented by PFN zero. Physical address
 * space below it is not part of the PMM and must never be counted as RAM.
 * Non-contiguous usable intervals are retained as holes in the flat PFN span;
 * pmm_init() reserves those holes before exposing allocations.
 */
void pmm_early_init(struct mem_region *regions, size_t count) {
  if (!regions || count == 0)
    panic("%s", "PMM: no boot memory-region table; refusing guessed RAM size");

  uint64_t max_detected = MEMORY_BASE;
  for (size_t i = 0; i < count; i++) {
    if (regions[i].type != MEM_REGION_USABLE || regions[i].size == 0)
      continue;
    if (regions[i].base > UINT64_MAX - regions[i].size)
      panic("PMM: overflowing usable region base=0x%lx size=0x%lx",
            regions[i].base, regions[i].size);

    uint64_t end = regions[i].base + regions[i].size;
    if (!hal_phys_range_valid(regions[i].base, end))
      panic("PMM: usable region outside direct-map window "
            "[0x%lx,0x%lx)", regions[i].base, end);
    if (end > max_detected)
      max_detected = end;
  }

  if (max_detected <= MEMORY_BASE)
    panic("PMM: no usable RAM above MEMORY_BASE=0x%lx", (uint64_t)MEMORY_BASE);

  uint64_t span_limit = HAL_PHYS_MAP_LIMIT;
  if (MEMORY_BASE >= span_limit)
    panic("%s", "PMM: MEMORY_BASE is outside the architecture physical map");
  if (max_detected > span_limit)
    panic("PMM: usable RAM ends beyond direct-map limit 0x%lx", span_limit);

  uint64_t mem_end = max_detected & ~((uint64_t)PAGE_SIZE - 1ULL);
  if (mem_end <= MEMORY_BASE)
    panic("%s", "PMM: usable span contains no complete page");

  total_pages = (mem_end - MEMORY_BASE) / PAGE_SIZE;
  uint64_t usable_bytes =
      pmm_calculate_usable_bytes(regions, count, MEMORY_BASE, mem_end);
  usable_pages = usable_bytes / PAGE_SIZE;
  if (usable_pages == 0 || usable_pages > total_pages)
    panic("PMM: invalid usable-page union (%lu pages, span %lu pages)",
          usable_pages, total_pages);

  /* 2. Calculate metadata sizes with checked size_t arithmetic. */
  if (total_pages > (uint64_t)(SIZE_MAX / sizeof(struct page)))
    panic("%s", "PMM: page descriptor array size overflows size_t");
  size_t page_array_size = (size_t)total_pages * sizeof(struct page);

  uint64_t dma_end_pfn = phys_to_pfn(DMA_ZONE_END - MEMORY_BASE);
  if (dma_end_pfn > total_pages)
    dma_end_pfn = total_pages;

  uint64_t dma_words = (dma_end_pfn + 63ULL) / 64ULL;
  uint64_t normal_pages = total_pages - dma_end_pfn;
  uint64_t normal_words = (normal_pages + 63ULL) / 64ULL;
  if (dma_words > (uint64_t)(SIZE_MAX / sizeof(uint64_t)) ||
      normal_words > (uint64_t)(SIZE_MAX / sizeof(uint64_t)))
    panic("%s", "PMM: bitmap size overflows size_t");

  size_t dma_bitmap_size = (size_t)dma_words * sizeof(uint64_t);
  size_t normal_bitmap_size = (size_t)normal_words * sizeof(uint64_t);
  if (page_array_size > SIZE_MAX - dma_bitmap_size ||
      page_array_size + dma_bitmap_size > SIZE_MAX - normal_bitmap_size)
    panic("%s", "PMM: metadata size overflows size_t");

  size_t raw_metadata_size =
      page_array_size + dma_bitmap_size + normal_bitmap_size;
  if (raw_metadata_size > SIZE_MAX - (PAGE_SIZE - 1U))
    panic("%s", "PMM: metadata page alignment overflows size_t");
  size_t total_metadata_size =
      (raw_metadata_size + PAGE_SIZE - 1U) & ~(PAGE_SIZE - 1U);

  /* 3. Find the lowest complete usable interval that can contain metadata.
   * The unsorted reservation overlays are applied to a fixpoint. */
  extern char __kernel_end[];
  uint64_t kernel_phys_end = virt_to_phys(__kernel_end);
  uint64_t kernel_limit = pmm_align_up_page(kernel_phys_end);
  if (kernel_limit == UINT64_MAX)
    panic("%s", "PMM: kernel end alignment overflow");

  uint64_t metadata_phys = UINT64_MAX;
  for (size_t i = 0; i < count; i++) {
    if (regions[i].type != MEM_REGION_USABLE || regions[i].size == 0)
      continue;
    if (regions[i].base > UINT64_MAX - regions[i].size)
      panic("PMM: overflowing usable region base=0x%lx size=0x%lx",
            regions[i].base, regions[i].size);

    uint64_t region_start = regions[i].base;
    uint64_t region_end = region_start + regions[i].size;
    region_start = pmm_max_u64(region_start, MEMORY_BASE);
    if (region_end > mem_end)
      region_end = mem_end;
    /* pmm_early_init touches metadata before VMM remaps all RAM. Restrict
     * placement to memory already reachable through the bootstrap tables. */
    if (region_start >= HAL_BOOTSTRAP_PHYS_MAP_LIMIT)
      continue;
    if (region_end > HAL_BOOTSTRAP_PHYS_MAP_LIMIT)
      region_end = HAL_BOOTSTRAP_PHYS_MAP_LIMIT;

    region_start = pmm_align_up_page(region_start);
    region_end &= ~((uint64_t)PAGE_SIZE - 1ULL);
    if (region_start == UINT64_MAX || region_end <= region_start)
      continue;

    uint64_t target = region_start > kernel_limit ? region_start : kernel_limit;
    for (size_t pass = 0; pass <= count; pass++) {
      if (target > UINT64_MAX - total_metadata_size)
        break;
      uint64_t target_end = target + total_metadata_size;
      int bumped = 0;

      for (size_t j = 0; j < count; j++) {
        if (regions[j].type == MEM_REGION_USABLE || regions[j].size == 0)
          continue;
        if (regions[j].base > UINT64_MAX - regions[j].size)
          panic("PMM: overflowing reserved region base=0x%lx size=0x%lx",
                regions[j].base, regions[j].size);

        uint64_t reserved_start = regions[j].base;
        uint64_t reserved_end = reserved_start + regions[j].size;
        if (reserved_end <= target || reserved_start >= target_end)
          continue;

        target = pmm_align_up_page(reserved_end);
        if (target == UINT64_MAX)
          break;
        bumped = 1;
        break;
      }

      if (target == UINT64_MAX)
        break;
      if (bumped)
        continue;

      if (target_end <= region_end && target < metadata_phys)
        metadata_phys = target;
      break;
    }
  }

  if (metadata_phys == UINT64_MAX)
    panic("PMM: no usable interval can contain metadata (%lu KB needed)",
          total_metadata_size / 1024U);

  pmm_metadata_end_phys = metadata_phys + total_metadata_size;

  /* 4. Map metadata pointers through the architecture direct map. */
  page_array = (struct page *)phys_to_virt(metadata_phys);
  dma_bitmap = (uint64_t *)((uintptr_t)page_array + page_array_size);
  normal_bitmap = (uint64_t *)((uintptr_t)dma_bitmap + dma_bitmap_size);

  /* 5. Zero out metadata. */
  memset(phys_to_virt(metadata_phys), 0, total_metadata_size);

  pr_info("PMM: Metadata initialized at 0x%lx (%lu KB)\n",
          metadata_phys, total_metadata_size / 1024U);
  pr_info("PMM: Usable RAM: %lu MB (address span %lu MB; MEMORY_BASE=0x%lx)\n",
          usable_pages * PAGE_SIZE / (1024U * 1024U),
          (mem_end - MEMORY_BASE) / (1024U * 1024U),
          (uint64_t)MEMORY_BASE);
}

/* Helper to reserve a range */
/*
 * pmm_reserve_range - mark [start_pfn, end_pfn) as PG_RESERVED | PG_KERNEL.
 *
 * Updates both the zone bitmap (under zone lock via bitmap_set) and the
 * global atomic free_pages counter.  Silently skips PFNs already reserved
 * or beyond total_pages.
 *
 * Called from pmm_init() to reserve kernel/metadata pages, firmware and
 * bootloader overlays, and PFN holes. Must not be called after SMP bring-up.
 */
static void pmm_reserve_range(uint64_t start_pfn, uint64_t end_pfn) {
  for (uint64_t pfn = start_pfn; pfn < end_pfn; pfn++) {
    if (pfn >= total_pages) break;
    struct page *pg = &page_array[pfn];
    if (pg->flags & PG_RESERVED) continue;

    pg->flags = PG_RESERVED | PG_KERNEL;
    pg->refcount = 1;

    /* Mark in bitmap */
    if (pfn < zones[ZONE_DMA].end_pfn) {
      bitmap_set(zones[ZONE_DMA].bitmap, pfn);
      zones[ZONE_DMA].free_pages--;
    } else {
      bitmap_set(zones[ZONE_NORMAL].bitmap, pfn - zones[ZONE_DMA].end_pfn);
      zones[ZONE_NORMAL].free_pages--;
    }
    __sync_fetch_and_sub(&free_pages, 1);
  }
}


/* Reserve the intersection of a physical interval with the PMM's represented
 * span. Start is rounded down and end up so partial edge pages are not freed.
 * Regions below MEMORY_BASE or above the highest usable end are harmless. */
static void pmm_reserve_phys_range(uint64_t base, uint64_t size) {
  if (size == 0)
    return;
  if (base > UINT64_MAX - size)
    panic("PMM: overflowing physical reservation base=0x%lx size=0x%lx",
          base, size);

  uint64_t span_start = MEMORY_BASE;
  if (total_pages > (UINT64_MAX - span_start) / PAGE_SIZE)
    panic("%s", "PMM: physical span end overflow");
  uint64_t span_end = span_start + total_pages * PAGE_SIZE;
  uint64_t range_end = base + size;

  if (range_end <= span_start || base >= span_end)
    return;

  uint64_t clipped_start = base < span_start ? span_start : base;
  uint64_t clipped_end = range_end > span_end ? span_end : range_end;
  uint64_t start_pfn = (clipped_start - span_start) / PAGE_SIZE;
  uint64_t end_offset = clipped_end - span_start;
  uint64_t end_pfn = (end_offset + PAGE_SIZE - 1ULL) / PAGE_SIZE;
  if (end_pfn > total_pages)
    end_pfn = total_pages;
  if (start_pfn < end_pfn)
    pmm_reserve_range(start_pfn, end_pfn);
}

/*
 * Initialize PMM with memory regions
 *
 * pmm_init - phase-2 PMM setup: zone construction and page reservation.
 *
 * Initialises ZONE_DMA (the first 16 MiB above MEMORY_BASE) and ZONE_NORMAL
 * (the remaining represented PFNs), then reserves kernel/metadata pages,
 * all non-usable overlays, and every hole outside the usable-region union.
 *
 * If pmm_early_init() was not previously called (e.g. direct entry for
 * testing), pmm_init() calls it here, but this path is not the normal flow.
 *
 * After this function returns all allocation primitives are usable.
 *
 * Locking: must be called single-threaded (before SMP init).
 */
void pmm_init(struct mem_region *regions, size_t count) {
  if (!page_array)
    pmm_early_init(regions, count);
  if (!regions || count == 0 || total_pages == 0)
    panic("%s", "PMM: pmm_init requires a validated non-empty region map");

  uint64_t dma_end_pfn = phys_to_pfn(DMA_ZONE_END - MEMORY_BASE);
  if (dma_end_pfn > total_pages)
    dma_end_pfn = total_pages;
  uint64_t normal_end_pfn = total_pages;

  zone_init(&zones[ZONE_DMA], "DMA", 0, dma_end_pfn, dma_bitmap);
  zone_init(&zones[ZONE_NORMAL], "Normal", dma_end_pfn, normal_end_pfn,
            normal_bitmap);
  free_pages = zones[ZONE_DMA].free_pages + zones[ZONE_NORMAL].free_pages;

  /* Reserve the kernel image and metadata using absolute physical intervals;
   * the helper clips them to [MEMORY_BASE, MEMORY_BASE + total_pages*PAGE_SIZE). */
  extern char __kernel_start[], __kernel_end[];
  uint64_t kernel_start_phys = virt_to_phys(__kernel_start);
  uint64_t kernel_end_phys = virt_to_phys(__kernel_end);
  if (kernel_end_phys <= kernel_start_phys)
    panic("%s", "PMM: invalid kernel physical image interval");
  pmm_reserve_phys_range(kernel_start_phys,
                         kernel_end_phys - kernel_start_phys);

  uint64_t metadata_start_phys = virt_to_phys(page_array);
  if (pmm_metadata_end_phys <= metadata_start_phys)
    panic("%s", "PMM: invalid metadata physical interval");
  pmm_reserve_phys_range(metadata_start_phys,
                         pmm_metadata_end_phys - metadata_start_phys);

  /* Apply reservation-map, /reserved-memory, DTB, firmware and boot-module
   * overlays. The physical helper clips below-base and above-span entries. */
  for (size_t i = 0; i < count; i++) {
    if (regions[i].type == MEM_REGION_USABLE || regions[i].size == 0)
      continue;
    pmm_reserve_phys_range(regions[i].base, regions[i].size);
  }

  /* Reserve every complete PFN not covered by the union of usable intervals.
   * This preserves non-contiguous RAM while keeping the flat PFN bitmap model. */
  uint64_t span_end = MEMORY_BASE + total_pages * PAGE_SIZE;
  uint64_t cursor = MEMORY_BASE;
  while (cursor < span_end) {
    uint64_t next_start = UINT64_MAX;
    uint64_t next_end = 0;

    for (size_t i = 0; i < count; i++) {
      if (regions[i].type != MEM_REGION_USABLE || regions[i].size == 0)
        continue;
      if (regions[i].base > UINT64_MAX - regions[i].size)
        panic("PMM: overflowing usable region base=0x%lx size=0x%lx",
              regions[i].base, regions[i].size);

      uint64_t start = regions[i].base;
      uint64_t end = start + regions[i].size;
      start = pmm_max_u64(start, MEMORY_BASE);
      if (end > span_end)
        end = span_end;

      start = pmm_align_up_page(start);
      end &= ~((uint64_t)PAGE_SIZE - 1ULL);
      if (start == UINT64_MAX || end <= cursor || end <= start)
        continue;
      if (start < cursor)
        start = cursor;
      if (start < next_start) {
        next_start = start;
        next_end = end;
      }
    }

    if (next_start == UINT64_MAX) {
      pmm_reserve_phys_range(cursor, span_end - cursor);
      break;
    }

    if (next_start > cursor)
      pmm_reserve_phys_range(cursor, next_start - cursor);

    if (next_end <= cursor)
      panic("%s", "PMM: usable-region union failed to advance");
    cursor = next_end;
  }

  pr_info("PMM: %lu MB usable, %lu MB free (span %lu MB; gaps/reservations excluded)\n",
          usable_pages * PAGE_SIZE / (1024U * 1024U),
          free_pages * PAGE_SIZE / (1024U * 1024U),
          total_pages * PAGE_SIZE / (1024U * 1024U));
  pr_info("PMM: DMA zone: %lu pages, Normal zone: %lu pages\n",
          zones[ZONE_DMA].free_pages, zones[ZONE_NORMAL].free_pages);
}

/*
 * Allocate a single page from specified zone
 *
 * zone_alloc_page - allocate one page frame from zone 'z'.
 *
 * Uses next-fit: starts the bitmap scan at z->next_free_pfn, then wraps
 * around to 0 if needed.  Updates z->next_free_pfn to hint the next call.
 *
 * After finding a free PFN:
 *   - marks it allocated in the zone bitmap (under zone lock).
 *   - decrements z->free_pages (under lock) and global free_pages (atomic).
 *   - initialises struct page (flags=0, refcount=1).
 *   - zeroes the page data (memset), cleans the D-cache line, issues a full
 *     memory barrier (hal_mb).  This ensures DMA coherency for the caller.
 *
 * Returns phys_to_virt(MEMORY_BASE + pfn_to_phys(abs_pfn)): the page's
 * kernel virtual address in the direct map (MM-PMM-07 resolved).
 *
 * Returns: pointer to the zeroed page, or NULL if the zone is exhausted.
 * Locking: takes and releases z->lock with IRQ save/restore.
 * IRQ context: safe to call from IRQ-disabled context.
 */
static void *zone_alloc_page(struct zone *z, int dma) {
  uint64_t flags;
  spin_lock_irqsave(&z->lock, &flags);

  uint64_t npages = z->end_pfn - z->start_pfn;
  uint64_t search_t0 = hal_timer_get_count();
  int64_t pfn = bitmap_find_free(z->bitmap, z->next_free_pfn, npages);
  if (pfn < 0) {
    /* Wrap around and search from the beginning */
    pfn = bitmap_find_free(z->bitmap, 0, z->next_free_pfn);
  }
  uint64_t search_ticks = hal_timer_get_count() - search_t0;

  if (pfn < 0) {
    spin_unlock_irqrestore(&z->lock, flags);
    return NULL;
  }

  /* The negative sentinel was rejected above; the PFN is now zone-relative
   * and non-negative.  Convert once to the bitmap's unsigned index type. */
  uint64_t found_pfn = (uint64_t)pfn;
  z->next_free_pfn = (found_pfn + 1) % npages;

  bitmap_set(z->bitmap, found_pfn);
  z->free_pages--;

  spin_unlock_irqrestore(&z->lock, flags);

  /* Convert to absolute PFN, then to the page's kernel virtual address
   * (direct map; identity while KERNEL_VIRT_BASE == 0). */
  uint64_t abs_pfn = z->start_pfn + found_pfn;
  struct page *pg = &page_array[abs_pfn];
  pg->flags = 0;
  pg->refcount = 1;

  void *addr = phys_to_virt(MEMORY_BASE + pfn_to_phys(abs_pfn));
  memset(addr, 0, PAGE_SIZE);
  if (dma) {
    /* DMA-coherency prep (perf §3.1): make the zeroed page visible to a device
     * that may not snoop the CPU caches.  ONLY DMA allocations pay this — the
     * vast majority (page tables, kernel stacks, struct process, user/sbrk
     * pages, kmalloc chunks) are CPU-only and skip the per-page DC CVAC loop +
     * full barrier, the dominant fixed cost on the spawn hot path. */
    hal_cache_clean(addr, PAGE_SIZE);
    hal_mb();
  }

  __sync_fetch_and_sub(&free_pages, 1);

  /* Instrumentation (perf brief §1): count this allocation and fold in its
   * bitmap-search cost.  Relaxed atomics; the max is a benign racy compare. */
  __sync_fetch_and_add(&stat_alloc_calls, 1);
  __sync_fetch_and_add(&stat_alloc_search_ticks_total, search_ticks);
  if (search_ticks > stat_alloc_search_ticks_max)
    stat_alloc_search_ticks_max = search_ticks;

  return addr;
}

/*
 * Allocate a single page
 *
 * pmm_alloc_page - allocate one 4KB page from the best available zone.
 *
 * Tries ZONE_NORMAL first (preferred to preserve DMA pages for constrained
 * devices), falls back to ZONE_DMA if Normal is exhausted.
 *
 * The returned page is zeroed, cache-cleaned, and memory-barrier-fenced (done
 * inside zone_alloc_page).
 *
 * Returns: pointer to a 4KB page, or NULL on global exhaustion.
 * Locking: no caller lock required; zone locks are taken internally.
 */
void *pmm_alloc_page(void) {
  /* Try normal zone first, then DMA.  CPU-only page (no cache-clean) — see
   * pmm_alloc_page_dma() for device-DMA buffers. */
  void *page = zone_alloc_page(&zones[ZONE_NORMAL], 0);
  if (!page) {
    page = zone_alloc_page(&zones[ZONE_DMA], 0);
  }
  return page;
}

/*
 * pmm_alloc_page_dma - like pmm_alloc_page() but cache-cleaned + fenced so the
 * page is coherent for a device that may not snoop the CPU caches (perf §3.1).
 * Use ONLY for buffers a device DMAs to/from (virtqueues, ring/cmd buffers,
 * GPU backing).  Ordinary kernel allocations must use pmm_alloc_page().
 */
void *pmm_alloc_page_dma(void) {
  void *page = zone_alloc_page(&zones[ZONE_NORMAL], 1);
  if (!page) {
    page = zone_alloc_page(&zones[ZONE_DMA], 1);
  }
  return page;
}

/*
 * Allocate multiple contiguous pages
 *
 * pmm_alloc_pages - allocate 'count' physically contiguous 4KB pages.
 *
 * Delegates to pmm_alloc_page() for count==1 (which includes cache-clean
 * and barrier).  For count>1, searches ZONE_NORMAL only via a linear bitmap
 * scan from PFN 0.
 *
 * NOTE(MM-PMM-04): Only ZONE_NORMAL is searched; there is no mechanism to
 * allocate a contiguous DMA-zone block.  DMA device drivers that need
 * physically contiguous memory below 16 MB cannot satisfy that requirement
 * through this path.
 *
 * Like the single-page path, the returned memory is zeroed, cache-cleaned
 * (hal_cache_clean) and barrier-fenced (hal_mb) so a device can DMA
 * to/from it immediately (MM-PMM-02 resolved; pmm_alloc_aligned inherits the
 * guarantee since it carves from this function).
 *
 * NOTE(MM-PMM-03): The contiguous scan is O(n) from PFN 0 on every call;
 * there is no buddy structure or run-length metadata to accelerate it.
 *
 * Returns: pointer to the first page of the contiguous run, or NULL.
 * Locking: takes and releases ZONE_NORMAL's lock with IRQ save/restore.
 */
static void *alloc_pages_impl(size_t count, int dma) {
  if (count == 0)
    return NULL;
  if (count == 1)
    return dma ? pmm_alloc_page_dma() : pmm_alloc_page();

  struct zone *z = &zones[ZONE_NORMAL];
  uint64_t flags;

  spin_lock_irqsave(&z->lock, &flags);

  uint64_t search_t0 = hal_timer_get_count();
  int64_t pfn =
      bitmap_find_contiguous(z->bitmap, 0, z->end_pfn - z->start_pfn, count);
  uint64_t search_ticks = hal_timer_get_count() - search_t0;
  if (pfn < 0) {
    spin_unlock_irqrestore(&z->lock, flags);
    return NULL;
  }

  /* The negative sentinel was rejected above; convert the zone-relative
   * start PFN once before using it as an unsigned bitmap/page-array index. */
  uint64_t found_pfn = (uint64_t)pfn;

  /* Mark all pages as used */
  for (size_t i = 0; i < count; i++) {
    bitmap_set(z->bitmap, found_pfn + i);
  }
  z->free_pages -= count;

  spin_unlock_irqrestore(&z->lock, flags);

  /* Initialize pages */
  uint64_t abs_pfn = z->start_pfn + found_pfn;
  for (size_t i = 0; i < count; i++) {
    struct page *pg = &page_array[abs_pfn + i];
    pg->flags = 0;
    pg->refcount = 1;
  }

  void *addr = phys_to_virt(MEMORY_BASE + pfn_to_phys(abs_pfn));
  memset(addr, 0, PAGE_SIZE * count);
  if (dma) {
    /* DMA-coherency prep — only for device buffers (perf §3.1). */
    hal_cache_clean(addr, PAGE_SIZE * count);
    hal_mb();
  }

  __sync_fetch_and_sub(&free_pages, count);

  /* Instrumentation (perf brief §1): the contiguous multi-page path is the
   * O(n)-from-0 scan (MM-PMM-03/06) — fold its search cost in too. */
  __sync_fetch_and_add(&stat_alloc_calls, 1);
  __sync_fetch_and_add(&stat_alloc_search_ticks_total, search_ticks);
  if (search_ticks > stat_alloc_search_ticks_max)
    stat_alloc_search_ticks_max = search_ticks;

  return addr;
}

/* pmm_alloc_pages - 'count' contiguous CPU-only pages (no cache-clean). */
void *pmm_alloc_pages(size_t count) { return alloc_pages_impl(count, 0); }

/* pmm_alloc_pages_dma - 'count' contiguous pages, cache-cleaned + fenced for
 * device DMA (perf §3.1).  Use ONLY for buffers a device DMAs to/from. */
void *pmm_alloc_pages_dma(size_t count) { return alloc_pages_impl(count, 1); }

/*
 * Free a single page
 *
 * pmm_free_page - release a single 4KB page back to its zone.
 *
 * Steps:
 *   1. Validates the pointer is within [MEMORY_BASE, MEMORY_BASE+total_pages*PAGE_SIZE).
 *   2. Checks PG_RESERVED; logs a warning and returns without freeing if set.
 *   3. Atomically decrements struct page refcount; if still >0 after decrement,
 *      another reference exists (shared page) -- no free is performed.
 *   4. Acquires the zone lock; checks bitmap to detect double-free (panics).
 *   5. Clears the bitmap bit and increments zone->free_pages (under lock).
 *   6. Poisons the page with 0xCC to catch use-after-free dereferences.
 *   7. Increments global free_pages atomically.
 *
 * The double-free check (step 4) is done under the lock, which prevents the
 * race where two CPUs both see refcount==1 and both attempt to free.
 *
 * Locking: takes and releases the zone spinlock with IRQ save/restore.
 */
void pmm_free_page(void *page) {
  if (!page)
    return;

  uint64_t phys = virt_to_phys(page);
#if HAL_RAM_START > 0
  if (phys < MEMORY_BASE) {
    pr_err("PMM: Attempt to free invalid address %p (below MEMORY_BASE)\n",
           page);
    return;
  }
#endif

  uint64_t pfn = phys_to_pfn(phys - MEMORY_BASE);
  if (pfn >= total_pages) {
    pr_err("PMM: Attempt to free invalid address %p (out of bounds)\n", page);
    return;
  }

  struct page *pg = &page_array[pfn];
  if (pg->flags & PG_RESERVED) {
    pr_warn("PMM: Attempt to free reserved page %016lx\n", phys);
    return;
  }

  /* Atomic decrement and check if it was the last reference */
  if (__sync_fetch_and_sub(&pg->refcount, 1) > 1)
    return;

  /* Find which zone this belongs to */
  struct zone *z;
  uint64_t zone_pfn;

  if (pfn < zones[ZONE_DMA].end_pfn) {
    z = &zones[ZONE_DMA];
    zone_pfn = pfn;
  } else {
    z = &zones[ZONE_NORMAL];
    zone_pfn = pfn - zones[ZONE_DMA].end_pfn;
  }

  uint64_t flags;
  spin_lock_irqsave(&z->lock, &flags);

  if (!bitmap_test(z->bitmap, zone_pfn)) {
    spin_unlock_irqrestore(&z->lock, flags);
    panic("PMM: Double free detected at %p (PFN %lu)", page, pfn);
  }

  bitmap_clear(z->bitmap, zone_pfn);
  z->free_pages++;

  spin_unlock_irqrestore(&z->lock, flags);

  /* Poison memory to catch use-after-free bugs */
  memset(page, 0xCC, PAGE_SIZE);

  __sync_fetch_and_add(&free_pages, 1);
  __sync_fetch_and_add(&stat_free_calls, 1); /* perf brief §1 */
}

/*
 * Free multiple contiguous pages
 *
 * pmm_free_pages - release 'count' contiguous pages starting at 'page'.
 *
 * Calls pmm_free_page() for each page sequentially.  Each call acquires and
 * releases the zone lock independently, so this is not an atomic bulk free.
 */
void pmm_free_pages(void *page, size_t count) {
  uint8_t *va = (uint8_t *)page;
  for (size_t i = 0; i < count; i++) {
    pmm_free_page(va + i * PAGE_SIZE);
  }
}

/*
 * Allocate with specific alignment (for block I/O)
 *
 * pmm_alloc_aligned - allocate 'size' bytes with at least 'align'-byte alignment.
 *
 * Allocates (pages + align_pages - 1) contiguous pages via pmm_alloc_pages()
 * to guarantee an aligned sub-range exists within the block, then frees the
 * unused leading and trailing pages.
 *
 * 'align' must be a power of two and a multiple of PAGE_SIZE for the bit-mask
 * arithmetic to be correct (no validation is performed on 'align').
 *
 * NOTE(MM-PMM-04): Like pmm_alloc_pages(), this function searches ZONE_NORMAL
 * only.  Aligned contiguous allocations in the DMA zone are not possible.
 *
 * Cache coherency: inherited from pmm_alloc_pages(), which cleans and fences
 * the whole run before returning (MM-PMM-02 resolved) — the aligned sub-range
 * is therefore DMA-safe as returned.
 *
 * Returns: aligned pointer, or NULL on failure.
 * Locking: delegates to pmm_alloc_pages() and pmm_free_pages(); no additional
 *          lock is held across the gap between those calls.
 */
void *pmm_alloc_aligned(size_t size, size_t align) {
  size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  size_t align_pages = (align + PAGE_SIZE - 1) / PAGE_SIZE;

  if (align_pages <= 1) {
    return pmm_alloc_pages_dma(pages); /* aligned allocs are DMA-safe by contract */
  }

  /* Allocate extra pages to ensure alignment.  DMA path: this API is documented
   * DMA-safe (block I/O), so it cleans+fences even though pmm_alloc_pages no
   * longer does (perf §3.1).  No current callers, but keep the contract. */
  size_t total = pages + align_pages - 1;
  void *mem = pmm_alloc_pages_dma(total);
  if (!mem)
    return NULL;

  /* Calculate aligned address */
  uint64_t addr = (uint64_t)mem;
  uint64_t aligned = (addr + align - 1) & ~(align - 1);

  /* Free unused pages at start */
  size_t skip = (aligned - addr) / PAGE_SIZE;
  if (skip > 0) {
    pmm_free_pages(mem, skip);
  }

  /* Free unused pages at end */
  size_t unused = total - skip - pages;
  if (unused > 0) {
    pmm_free_pages((void *)(aligned + pages * PAGE_SIZE), unused);
  }

  return (void *)aligned;
}

/*
 * Get page descriptor for physical address
 *
 * pmm_phys_to_page - return the struct page descriptor for a physical address.
 *
 * Returns NULL if 'phys' is below MEMORY_BASE (guarded by ARCH_MEMORY_BASE
 * compile-time check) or beyond total_pages.
 *
 * 'phys' is a PHYSICAL address; pointer callers must virt_to_phys() first.
 */
struct page *pmm_phys_to_page(uint64_t phys) {
#if HAL_RAM_START > 0
  if (phys < MEMORY_BASE)
    return NULL;
#endif
  uint64_t pfn = phys_to_pfn(phys - MEMORY_BASE);
  if (pfn >= total_pages)
    return NULL;
  return &page_array[pfn];
}

/*
 * Get physical address for page descriptor
 *
 * pmm_page_to_phys - return the physical address corresponding to a struct page.
 *
 * Validates that 'page' is an aligned entry in page_array, then returns
 * MEMORY_BASE + pfn_to_phys(pfn).  Returns physical zero for an invalid entry.
 *
 * The returned value is a PHYSICAL address; use phys_to_virt() to get a
 * dereferenceable pointer.
 */
uint64_t pmm_page_to_phys(struct page *page) {
  if (!page || !page_array)
    return 0;

  /* Use integer bounds before deriving the PFN: subtracting unrelated C
   * pointers is undefined, and a negative ptrdiff_t must never become a
   * huge unsigned frame number.  Physical zero is reserved and is the
   * invalid sentinel for this legacy helper. */
  uintptr_t base = (uintptr_t)page_array;
  uintptr_t addr = (uintptr_t)page;
  if (addr < base)
    return 0;
  uintptr_t offset = addr - base;
  if (total_pages > UINTPTR_MAX / sizeof(*page))
    return 0;
  uintptr_t bytes = (uintptr_t)total_pages * sizeof(*page);
  if (offset >= bytes || (offset % sizeof(*page)) != 0)
    return 0;

  uint64_t pfn = (uint64_t)(offset / sizeof(*page));
  return MEMORY_BASE + pfn_to_phys(pfn);
}

/*
 * Statistics
 *
 * pmm_get_free_pages - return the approximate global count of free pages.
 *
 * NOTE(MM-PMM-05): The global free_pages counter is updated atomically, but
 * per-zone free_pages counters are updated under individual zone locks.  The
 * two can transiently disagree.  This function returns only the global counter.
 */
uint64_t pmm_get_free_pages(void) { return free_pages; }

/* pmm_get_total_pages - return total_pages (immutable after pmm_init).
 *
 * This is the metadata SPAN, not the amount of memory that exists.  It is the
 * bound of page_array[]/the zone bitmaps, so it is what PFN indexing must be
 * validated against — and nothing else.  Anything reporting "how much RAM does
 * this machine have" wants pmm_get_usable_pages(). */
uint64_t pmm_get_total_pages(void) { return total_pages; }

/*
 * pmm_get_usable_pages - RAM that actually exists, in pages.
 *
 * MM-PMM-09 (#94, fixed 2026-07-23): the span and the usable total are not the
 * same number on amd64, and userland was being shown the span.  The firmware
 * memory map has a hole below 4 GB for PCI/MMIO, so with 6 GB installed the
 * usable regions sum to ~5119 MB while the address span from MEMORY_BASE to the
 * highest usable end is 6144 MB.  Reporting the span made the monitor print
 * "free 5026 / 6144 MB, used 1118 MB" — attributing the ~1 GB hardware hole to
 * consumed memory, so used was wrong by more than an order of magnitude (the
 * real figure is ~93 MB).
 *
 * The gap is already handled correctly INSIDE the PMM (the regions are reserved
 * and never allocated); it simply must not be counted as memory.  Per the HAL
 * abstraction rule: amd64 does not use the gap and must not present it as
 * something that exists.  aarch64 has no such hole, so span == usable there and
 * this changes nothing — which is the point: one number, correct on both.
 */
uint64_t pmm_get_usable_pages(void) { return usable_pages; }

/*
 * pmm_dump_stats - print total/free/used page counts to the kernel log.
 *
 * Uses the global free_pages counter; see NOTE(MM-PMM-05) for caveats.
 * Suitable for boot-time diagnostics only; takes no locks.
 */
void pmm_dump_stats(void) {
  pr_info("%s", "PMM Statistics:\n");
  pr_info("  Total: %lu pages (%lu MB)\n", total_pages,
          total_pages * PAGE_SIZE / (1024 * 1024));
  pr_info("  Free:  %lu pages (%lu MB)\n", free_pages,
          free_pages * PAGE_SIZE / (1024 * 1024));
  pr_info("  Used:  %lu pages (%lu MB)\n", total_pages - free_pages,
          (total_pages - free_pages) * PAGE_SIZE / (1024 * 1024));
}

/*
 * pmm_get_counters - read the cumulative allocator counters (perf brief §1).
 *
 * search_ticks_* are raw hal_timer_get_count() ticks; the caller converts to
 * ns via timer_counts_to_ns().  All four out-params are optional (NULL-skip).
 * Lock-free reads of relaxed atomics — values are a coherent-enough snapshot.
 */
void pmm_get_counters(uint64_t *alloc_calls, uint64_t *free_calls,
                      uint64_t *search_ticks_total, uint64_t *search_ticks_max) {
  if (alloc_calls)       *alloc_calls = stat_alloc_calls;
  if (free_calls)        *free_calls = stat_free_calls;
  if (search_ticks_total) *search_ticks_total = stat_alloc_search_ticks_total;
  if (search_ticks_max)  *search_ticks_max = stat_alloc_search_ticks_max;
}

/*
 * pmm_get_fragmentation - instantaneous ZONE_NORMAL fragmentation snapshot
 * (perf brief §1/§2.1): the largest contiguous free run (pages) and the number
 * of distinct free runs.  A shrinking largest_run while free_pages stays flat
 * is the fragmentation signature.  Word-level scan with all-free / all-allocated
 * fast paths; taken under the zone lock (on-demand at ~1 Hz poll, off the hot
 * path).  Either out-param may be NULL.
 */
void pmm_get_fragmentation(uint64_t *largest_run, uint64_t *run_count) {
  struct zone *z = &zones[ZONE_NORMAL];
  uint64_t flags;
  uint64_t cur = 0, max = 0, runs = 0;
  int in_run = 0;

  spin_lock_irqsave(&z->lock, &flags);
  uint64_t npages = z->end_pfn - z->start_pfn;
  uint64_t nwords = (npages + 63) / 64;
  for (uint64_t wi = 0; wi < nwords; wi++) {
    uint64_t w = z->bitmap[wi];
    uint64_t bits = 64;
    if (wi == nwords - 1 && (npages % 64))
      bits = npages % 64;

    if (w == 0 && bits == 64) {
      /* whole word free — extend (or start) the current run */
      if (!in_run) { in_run = 1; runs++; cur = 0; }
      cur += 64;
      if (cur > max) max = cur;
    } else if (w == ~0ULL) {
      /* whole word allocated — close any run */
      in_run = 0; cur = 0;
    } else {
      for (uint64_t b = 0; b < bits; b++) {
        if (!(w & (1ULL << b))) {
          if (!in_run) { in_run = 1; runs++; cur = 0; }
          cur++;
          if (cur > max) max = cur;
        } else {
          in_run = 0; cur = 0;
        }
      }
    }
  }
  spin_unlock_irqrestore(&z->lock, flags);

  if (largest_run) *largest_run = max;
  if (run_count)   *run_count = runs;
}
