/*
 * kernel/include/kernel/pmm.h
 * Physical Memory Manager
 *
 * Page frame allocator with block-aligned allocation support
 * for zero-copy I/O and partition management.
 *
 * This header defines the public API and key data structures of the PMM.
 * The PMM is split into two zones:
 *   ZONE_DMA    (PFN 0 .. dma_end_pfn-1): physical memory below 16 MB,
 *               reserved for devices requiring sub-16MB DMA addresses.
 *   ZONE_NORMAL (dma_end_pfn .. total_pages-1): general-purpose allocation.
 *
 * PA/VA contract (MM-PMM-07 resolved, see kernel/memlayout.h):
 *   pmm_alloc_*() return kernel VIRTUAL pointers (direct map,
 *   phys_to_virt of the frame address); pmm_free_*() take the same
 *   pointers back.  Callers needing the frame's PHYSICAL address (page
 *   table entries, DMA descriptors, TTBR/CR3) must use virt_to_phys().
 *   pmm_phys_to_page()/pmm_page_to_phys() speak physical addresses only.
 *
 * Known issues (see docs/review/analysis/01-mm-memory-management.md):
 *   MM-PMM-01 through MM-PMM-06 (07 resolved).
 */
#ifndef _KERNEL_PMM_H
#define _KERNEL_PMM_H

#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

/* Page size definitions */
/* 4KB granule, matching hardware page tables on both arches. */
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT) /* 4KB */
#define PAGE_MASK (~(PAGE_SIZE - 1))

/* Block alignment for I/O */
#define BLOCK_SIZE_512 512
#define BLOCK_SIZE_4K 4096
#define BLOCK_SIZE_64K 65536

/* Memory zones */
/* NOTE(MM-PMM-04): pmm_alloc_pages() and pmm_alloc_aligned() only search
 * ZONE_NORMAL; contiguous DMA-zone allocations are not supported. */
#define ZONE_DMA 0    /* 0-16MB, for legacy DMA */
#define ZONE_NORMAL 1 /* 16MB-end, normal allocations */
#define ZONE_COUNT 2

/* Page flags */
/* Stored in struct page.flags. */
#define PG_RESERVED (1 << 0) /* Cannot be allocated */
#define PG_KERNEL (1 << 1)   /* Kernel memory */
#define PG_USER (1 << 2)     /* User memory */
#define PG_DIRTY (1 << 3)    /* Page modified */
#define PG_LOCKED (1 << 4)   /* Page locked in memory */

/* Physical page descriptor */
/*
 * struct page - per-frame metadata stored in the global page_array.
 *
 * One entry exists for every physical page frame between MEMORY_BASE and
 * MEMORY_BASE + total_pages*PAGE_SIZE.
 *
 * refcount: set to 1 on allocation, decremented on pmm_free_page().  If it
 *           reaches 0 the page is returned to its zone bitmap.  In practice
 *           it is never incremented above 1 because there is no vmm_map path
 *           that increments it (frame sharing is not implemented).
 *           NOTE(MM-VMM-04): vmm_destroy_pgd does not decrement refcount for
 *           user-mapped frames; those pages therefore never reach refcount 0
 *           and are never freed on process exit.
 *
 * lru, priv: currently unused; reserved for a future page-cache integration.
 */
struct page {
  uint32_t flags;
  uint32_t refcount;
  struct list_head lru; /* For page cache LRU */
  void *priv;           /* For slab/buffer use */
};

/* Memory zone descriptor */
/*
 * struct zone - descriptor for a contiguous range of page frames.
 *
 * start_pfn / end_pfn: absolute PFN range [start_pfn, end_pfn).
 * bitmap:       allocation bitmap; bit i (0-based within the zone) is 1 if
 *               the corresponding frame is allocated or reserved, 0 if free.
 * next_free_pfn: next-fit hint; zone-relative PFN where the last single-page
 *               allocation succeeded.  Wraps to 0 at end_pfn.
 *               NOTE(MM-PMM-06): ignored by the contiguous-allocation path.
 * free_pages:   count of free frames in this zone.
 *               NOTE(MM-PMM-05): updated under zone->lock (non-atomically);
 *               can transiently disagree with the global atomic free_pages.
 * lock:         spinlock protecting bitmap and free_pages for this zone.
 */
struct zone {
  const char *name;
  uint64_t start_pfn;     /* First page frame number */
  uint64_t end_pfn;       /* Last page frame number */
  uint64_t free_pages;    /* Number of free pages */
  uint64_t *bitmap;       /* Allocation bitmap */
  uint64_t next_free_pfn; /* Next-Fit optimization: last known free PFN */
  spinlock_t lock;        /* Zone lock */
};

/* Memory region from bootloader/DTB */
/*
 * struct mem_region - a single physical memory extent reported by the boot layer.
 *
 * Populated by arch_platform_get_mem_regions() from either the DTB (AArch64)
 * or the multiboot/PVH memory map (AMD64).  Used by pmm_early_init() and
 * vmm_dynamic_remap() to discover all of physical RAM.
 */
struct mem_region {
  uint64_t base;
  uint64_t size;
  uint32_t type; /* 1=usable, 2=reserved, etc */
};

#define MEM_REGION_USABLE 1
#define MEM_REGION_RESERVED 2
#define MEM_REGION_ACPI 3
#define MEM_REGION_MMIO 4

/* PMM API */

/* Initialize PMM with memory regions */
void pmm_early_init(struct mem_region *regions, size_t count);
void pmm_init(struct mem_region *regions, size_t count);
/* Physical end address of the PMM metadata (page_array + bitmaps), valid after
 * pmm_early_init(); vmm_init() maps the bootstrap window up to here. */
uint64_t pmm_metadata_top(void);
/* NOTE(MM-PMM-01): pmm_init_region() is a stub; it only prints and returns. */
void pmm_init_region(uint64_t base, uint64_t size);

/* Allocate a single page */
/* Allocate a single page -- zeroed, cache-cleaned, memory-barrier-fenced.
 * Returns NULL if all zones are exhausted. */
void *pmm_alloc_page(void);

/* Allocate multiple contiguous pages */
/* Allocate 'count' physically contiguous pages from ZONE_NORMAL.
 * NOTE(MM-PMM-02): Does NOT cache-clean or barrier for count>1.
 * NOTE(MM-PMM-04): Only searches ZONE_NORMAL; no DMA-zone contiguous alloc. */
void *pmm_alloc_pages(size_t count);

/* Free a single page */
/* Free a single page; poisons with 0xCC; panics on double-free. */
void pmm_free_page(void *page);

/* Free multiple contiguous pages */
/* Free 'count' contiguous pages starting at 'page'; calls pmm_free_page()
 * for each page independently (not an atomic bulk operation). */
void pmm_free_pages(void *page, size_t count);

/* Allocate with specific alignment (for block I/O) */
/* Allocate 'size' bytes with at least 'align'-byte alignment from ZONE_NORMAL.
 * 'align' must be a power-of-two multiple of PAGE_SIZE.
 * NOTE(MM-PMM-02): Same cache-clean/barrier omission as pmm_alloc_pages(). */
void *pmm_alloc_aligned(size_t size, size_t align);

/* Get page descriptor for physical address */
/* Return the struct page descriptor for a physical address; NULL if out of range. */
struct page *pmm_phys_to_page(uint64_t phys);

/* Get physical address for page descriptor */
/* Return the physical address for a struct page descriptor. */
uint64_t pmm_page_to_phys(struct page *page);

/* Convert between physical and page frame number */
/* Convert between physical address (relative to MEMORY_BASE) and page frame number.
 * phys_to_pfn: phys must already have MEMORY_BASE subtracted by the caller.
 * pfn_to_phys: returns the offset from MEMORY_BASE, NOT an absolute address. */
static inline uint64_t phys_to_pfn(uint64_t phys) { return phys >> PAGE_SHIFT; }

static inline uint64_t pfn_to_phys(uint64_t pfn) { return pfn << PAGE_SHIFT; }

/* Page aligned macros */
#define PAGE_ALIGN(addr) (((addr) + PAGE_SIZE - 1) & PAGE_MASK)
#define PAGE_ALIGN_DOWN(addr) ((addr) & PAGE_MASK)

/* Statistics */
uint64_t pmm_get_free_pages(void);
uint64_t pmm_get_total_pages(void);
void pmm_dump_stats(void);

#endif /* _KERNEL_PMM_H */
