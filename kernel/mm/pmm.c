/*
 * kernel/mm/pmm.c
 * Physical Memory Manager (PMM)
 *
 * Implements the lowest layer of the OS1/NEXS memory subsystem: page-frame
 * allocation over two zones (DMA and Normal).  The allocator is split into two
 * phases:
 *
 *   pmm_early_init()  - probes the mem_region table supplied by the boot layer,
 *                        calculates the size of per-frame metadata, and places
 *                        the metadata (page_array + bitmaps) in the first
 *                        usable physical region above the kernel image.  The
 *                        MMU is NOT yet active at this point.
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
#include <arch/arch.h>
#include <kernel/memlayout.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

/* Memory configuration from architecture */
#define MEMORY_BASE ARCH_RAM_START
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
 * search_ticks_* are raw arch_timer_get_count() ticks; the collector converts
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
 * Called very early in boot, before the MMU is active and before any dynamic
 * allocation is possible.  Scans the mem_region table to determine the highest
 * usable physical address (clamped at 256 GB for metadata sanity), computes
 * the sizes of page_array, dma_bitmap, and normal_bitmap, and places them
 * contiguously in the first sufficiently large usable region at or above the
 * kernel image end (__kernel_end).
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
 * NOTE(MM-PMM-07): All metadata pointers are derived from physical addresses
 * and used as C pointers directly, relying on the identity-map invariant.
 */
void pmm_early_init(struct mem_region *regions, size_t count) {
  uint64_t mem_end = MEMORY_BASE;
  uint64_t max_detected = 0;

  /* 1. Discover total RAM.  max_detected = highest usable END address (the
   * metadata span); usable_bytes = real RAM sum (MM-PMM-08 #117: with -m 5G
   * QEMU amd64 splits RAM around the 3..4GB PCI hole, so span 6GB != RAM 5GB). */
  uint64_t usable_bytes = 0;
  if (regions && count > 0) {
    for (size_t i = 0; i < count; i++) {
      if (regions[i].type == MEM_REGION_USABLE) {
        uint64_t end = regions[i].base + regions[i].size;
        if (end > max_detected) max_detected = end;
        if (end > MEMORY_BASE)
          usable_bytes += end - (regions[i].base > MEMORY_BASE
                                     ? regions[i].base
                                     : MEMORY_BASE);
      }
    }
  } else {
    /* Fallback to 1GB if no regions provided */
    max_detected = MEMORY_BASE + (1UL << 30);
    usable_bytes = (1UL << 30);
    pr_warn("%s", "PMM: No memory regions detected, falling back to 1GB\n");
  }

  /* Safety: clamp to 256GB for metadata sanity, but allow much more than 1GB */
  if (max_detected > MEMORY_BASE + (256UL << 30)) {
    max_detected = MEMORY_BASE + (256UL << 30);
  }
  
  mem_end = max_detected;
  total_pages = (mem_end - MEMORY_BASE) / PAGE_SIZE;
  usable_pages = usable_bytes / PAGE_SIZE;
  if (usable_pages > total_pages) usable_pages = total_pages;

  /* 2. Calculate metadata sizes */
  size_t page_array_size = total_pages * sizeof(struct page);
  uint64_t dma_end_pfn = phys_to_pfn(DMA_ZONE_END - MEMORY_BASE);
  size_t dma_bitmap_size = (dma_end_pfn + 63) / 64 * 8;
  size_t normal_bitmap_size = (total_pages - dma_end_pfn + 63) / 64 * 8;
  size_t total_metadata_size = PAGE_ALIGN(page_array_size + dma_bitmap_size + normal_bitmap_size);

  /* 3. Find a place for metadata (must be in the first usable region) */
  uintptr_t metadata_phys = 0;
  for (size_t i = 0; i < count; i++) {
    if (regions[i].type == MEM_REGION_USABLE && regions[i].size >= total_metadata_size) {
      extern char __kernel_end[];
      /* __kernel_end is a link-address (virtual) symbol; compare in the
       * physical domain. */
      uint64_t kernel_limit = PAGE_ALIGN(virt_to_phys(__kernel_end));
      uint64_t target = regions[i].base;
      if (target < kernel_limit) target = kernel_limit;

      /* Must not land on any RESERVED region.  A GRUB boot module (the
       * release rootfs) is loaded right after the kernel — exactly where
       * the metadata wants to go — so bump 'target' past every reserved
       * region it would overlap.  The table is unsorted, so iterate to a
       * fixpoint. */
      int bumped = 1;
      while (bumped) {
        bumped = 0;
        for (size_t j = 0; j < count; j++) {
          if (regions[j].type == MEM_REGION_USABLE)
            continue;
          uint64_t r_start = regions[j].base;
          uint64_t r_end = PAGE_ALIGN(regions[j].base + regions[j].size);
          if (target < r_end && target + total_metadata_size > r_start) {
            target = r_end;
            bumped = 1;
          }
        }
      }

      if (target + total_metadata_size <= regions[i].base + regions[i].size) {
        metadata_phys = target;
        break;
      }
    }
  }

  if (!metadata_phys) {
    panic("PMM: Failed to allocate metadata area (%lu KB needed)", total_metadata_size / 1024);
  }

  /* Physical end of the metadata, so vmm_init can ensure the bootstrap map
   * covers it wherever it landed (a boot module can push it well past the
   * kernel image). */
  pmm_metadata_end_phys = metadata_phys + total_metadata_size;

  /* 4. Map pointers (kernel VAs via the direct map; identity while
   * KERNEL_VIRT_BASE == 0) */
  page_array = (struct page *)phys_to_virt(metadata_phys);
  dma_bitmap = (uint64_t *)((uintptr_t)page_array + page_array_size);
  normal_bitmap = (uint64_t *)((uintptr_t)dma_bitmap + dma_bitmap_size);

  /* 5. Zero out metadata */
  memset(phys_to_virt(metadata_phys), 0, total_metadata_size);
  
  pr_info("PMM: Metadata initialized at 0x%lx (%lu KB)\n", metadata_phys, total_metadata_size / 1024);
  pr_info("PMM: Usable RAM: %lu MB (address span %lu MB)\n",
          usable_pages * PAGE_SIZE / (1024 * 1024),
          (mem_end - MEMORY_BASE) / (1024 * 1024));
}

/* Helper to reserve a range */
/*
 * pmm_reserve_range - mark [start_pfn, end_pfn) as PG_RESERVED | PG_KERNEL.
 *
 * Updates both the zone bitmap (under zone lock via bitmap_set) and the
 * global atomic free_pages counter.  Silently skips PFNs already reserved
 * or beyond total_pages.
 *
 * Called from pmm_init() to reserve the kernel image and PMM metadata pages.
 * Must not be called after the system is fully booted (not SMP-safe if
 * called concurrently with allocations outside the zone lock).
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

/*
 * Initialize PMM with memory regions
 *
 * pmm_init - phase-2 PMM setup: zone construction and page reservation.
 *
 * Initialises ZONE_DMA (PFN 0..dma_end_pfn-1) and ZONE_NORMAL
 * (dma_end_pfn..total_pages-1), then calls pmm_reserve_range() to mark the
 * kernel image, PMM metadata, and any non-usable bootloader regions.
 *
 * If pmm_early_init() was not previously called (e.g. direct entry for
 * testing), pmm_init() calls it here, but this path is not the normal flow.
 *
 * After this function returns all allocation primitives are usable.
 *
 * Locking: must be called single-threaded (before SMP init).
 */
void pmm_init(struct mem_region *regions, size_t count) {
  /* If early_init wasn't called, we are in trouble, but let's try to handle it */
  if (!page_array) {
    pmm_early_init(regions, count);
  }

  uint64_t dma_end_pfn = phys_to_pfn(DMA_ZONE_END - MEMORY_BASE);
  uint64_t normal_end_pfn = total_pages;

  /* Initialize zones with our dynamic bitmaps */
  zone_init(&zones[ZONE_DMA], "DMA", 0, dma_end_pfn, dma_bitmap);
  zone_init(&zones[ZONE_NORMAL], "Normal", dma_end_pfn, normal_end_pfn, normal_bitmap);

  free_pages = zones[ZONE_DMA].free_pages + zones[ZONE_NORMAL].free_pages;

  /* Mark kernel pages as reserved (image symbols are virtual; PFNs are
   * physical — translate first). */
  extern char __kernel_start[], __kernel_end[];
  uint64_t kernel_start_pfn = phys_to_pfn(virt_to_phys(__kernel_start) - MEMORY_BASE);
  uint64_t kernel_end_pfn = phys_to_pfn(PAGE_ALIGN(virt_to_phys(__kernel_end)) - MEMORY_BASE);

  /* Also reserve the metadata itself! */
  uint64_t metadata_start_pfn = phys_to_pfn(virt_to_phys(page_array) - MEMORY_BASE);
  size_t page_array_size = total_pages * sizeof(struct page);
  size_t dma_bitmap_size = (dma_end_pfn + 63) / 64 * 8;
  size_t normal_bitmap_size = (total_pages - dma_end_pfn + 63) / 64 * 8;
  uint64_t metadata_end_pfn = phys_to_pfn(PAGE_ALIGN(virt_to_phys(page_array) + page_array_size + dma_bitmap_size + normal_bitmap_size) - MEMORY_BASE);

  pmm_reserve_range(kernel_start_pfn, kernel_end_pfn);
  pmm_reserve_range(metadata_start_pfn, metadata_end_pfn);

  /* Mark other reserved regions from bootloader */
  for (size_t i = 0; i < count; i++) {
    if (regions[i].type != MEM_REGION_USABLE) {
        uint64_t start = phys_to_pfn(regions[i].base - MEMORY_BASE);
        uint64_t end = phys_to_pfn(PAGE_ALIGN(regions[i].base + regions[i].size) - MEMORY_BASE);
        pmm_reserve_range(start, end);
    }
  }

  /* MM-PMM-08 (#117): total_pages spans up to the HIGHEST usable end address.
   * Holes inside that span (amd64 PCI hole 3..4GB, VGA/BIOS 640KB..1MB) are
   * usually ABSENT from the boot memmap — neither usable nor reserved — so
   * the loop above never touches them and they would be handed out as RAM.
   * Walk the usable regions in address order (pick-min, the table may be
   * unsorted) and reserve every PFN they do not cover.  Rounding is
   * conservative: partial pages at region edges are reserved. */
  if (regions && count > 0) {
    uint64_t cursor = MEMORY_BASE;
    int done = 0;
    while (!done) {
      uint64_t next_base = (uint64_t)-1;
      uint64_t next_end = 0;
      for (size_t i = 0; i < count; i++) {
        if (regions[i].type != MEM_REGION_USABLE) continue;
        uint64_t base = regions[i].base;
        uint64_t end = regions[i].base + regions[i].size;
        if (end <= cursor) continue; /* already behind the cursor */
        if (base < cursor) base = cursor;
        if (base < next_base) {
          next_base = base;
          next_end = end;
        }
      }
      if (next_base == (uint64_t)-1) { /* no usable RAM past cursor */
        next_base = MEMORY_BASE + total_pages * PAGE_SIZE;
        done = 1;
      }
      if (next_base > cursor) {
        pmm_reserve_range(phys_to_pfn(cursor - MEMORY_BASE),
                          phys_to_pfn(PAGE_ALIGN(next_base) - MEMORY_BASE));
      }
      if (!done) cursor = next_end;
    }
  }

  pr_info("PMM: %lu MB usable, %lu MB free (span %lu MB; gaps reserved)\n",
          usable_pages * PAGE_SIZE / (1024 * 1024),
          free_pages * PAGE_SIZE / (1024 * 1024),
          total_pages * PAGE_SIZE / (1024 * 1024));
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
 *     memory barrier (arch_mb).  This ensures DMA coherency for the caller.
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
  uint64_t search_t0 = arch_timer_get_count();
  int64_t pfn = bitmap_find_free(z->bitmap, z->next_free_pfn, npages);
  if (pfn < 0) {
    /* Wrap around and search from the beginning */
    pfn = bitmap_find_free(z->bitmap, 0, z->next_free_pfn);
  }
  uint64_t search_ticks = arch_timer_get_count() - search_t0;

  if (pfn < 0) {
    spin_unlock_irqrestore(&z->lock, flags);
    return NULL;
  }

  /* Update next_free_pfn for next caller */
  z->next_free_pfn = (pfn + 1) % npages;

  bitmap_set(z->bitmap, pfn);
  z->free_pages--;

  spin_unlock_irqrestore(&z->lock, flags);

  /* Convert to absolute PFN, then to the page's kernel virtual address
   * (direct map; identity while KERNEL_VIRT_BASE == 0). */
  uint64_t abs_pfn = z->start_pfn + pfn;
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
    arch_cache_clean_range(addr, PAGE_SIZE);
    arch_mb();
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
 * (arch_cache_clean_range) and barrier-fenced (arch_mb) so a device can DMA
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

  uint64_t search_t0 = arch_timer_get_count();
  int64_t pfn =
      bitmap_find_contiguous(z->bitmap, 0, z->end_pfn - z->start_pfn, count);
  uint64_t search_ticks = arch_timer_get_count() - search_t0;
  if (pfn < 0) {
    spin_unlock_irqrestore(&z->lock, flags);
    return NULL;
  }

  /* Mark all pages as used */
  for (size_t i = 0; i < count; i++) {
    bitmap_set(z->bitmap, pfn + i);
  }
  z->free_pages -= count;

  spin_unlock_irqrestore(&z->lock, flags);

  /* Initialize pages */
  uint64_t abs_pfn = z->start_pfn + pfn;
  for (size_t i = 0; i < count; i++) {
    struct page *pg = &page_array[abs_pfn + i];
    pg->flags = 0;
    pg->refcount = 1;
  }

  void *addr = phys_to_virt(MEMORY_BASE + pfn_to_phys(abs_pfn));
  memset(addr, 0, PAGE_SIZE * count);
  if (dma) {
    /* DMA-coherency prep — only for device buffers (perf §3.1). */
    arch_cache_clean_range(addr, PAGE_SIZE * count);
    arch_mb();
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
#if ARCH_MEMORY_BASE > 0
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
#if ARCH_MEMORY_BASE > 0
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
 * Computes pfn = page - page_array, then returns MEMORY_BASE + pfn_to_phys(pfn).
 * The caller is responsible for ensuring 'page' is a valid entry in page_array.
 *
 * The returned value is a PHYSICAL address; use phys_to_virt() to get a
 * dereferenceable pointer.
 */
uint64_t pmm_page_to_phys(struct page *page) {
  uint64_t pfn = page - page_array;
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

/* pmm_get_total_pages - return total_pages (immutable after pmm_init). */
uint64_t pmm_get_total_pages(void) { return total_pages; }

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
 * search_ticks_* are raw arch_timer_get_count() ticks; the caller converts to
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
