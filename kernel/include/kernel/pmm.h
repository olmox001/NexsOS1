/*
 * kernel/include/kernel/pmm.h
 * Physical Memory Manager
 *
 * Page frame allocator with block-aligned allocation support
 * for zero-copy I/O and partition management.
 */
#ifndef _KERNEL_PMM_H
#define _KERNEL_PMM_H

#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

/* Page size definitions */
#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT) /* 4KB */
#define PAGE_MASK (~(PAGE_SIZE - 1))

/* Block alignment for I/O */
#define BLOCK_SIZE_512 512
#define BLOCK_SIZE_4K 4096
#define BLOCK_SIZE_64K 65536

/* Memory zones */
#define ZONE_DMA 0    /* 0-16MB, for legacy DMA */
#define ZONE_NORMAL 1 /* 16MB-end, normal allocations */
#define ZONE_COUNT 2

/* Page flags */
#define PG_RESERVED (1 << 0) /* Cannot be allocated */
#define PG_KERNEL (1 << 1)   /* Kernel memory */
#define PG_USER (1 << 2)     /* User memory */
#define PG_DIRTY (1 << 3)    /* Page modified */
#define PG_LOCKED (1 << 4)   /* Page locked in memory */

/* Physical page descriptor */
struct page {
  uint32_t flags;
  uint32_t refcount;
  struct list_head lru; /* For page cache LRU */
  void *priv;           /* For slab/buffer use */
};

/* Memory zone descriptor */
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
void pmm_init(struct mem_region *regions, size_t count);
void pmm_init_region(uint64_t base, uint64_t size);

/* Allocate a single page */
void *pmm_alloc_page(void);

/* Allocate multiple contiguous pages */
void *pmm_alloc_pages(size_t count);

/* Free a single page */
void pmm_free_page(void *page);

/* Free multiple contiguous pages */
void pmm_free_pages(void *page, size_t count);

/* Allocate with specific alignment (for block I/O) */
void *pmm_alloc_aligned(size_t size, size_t align);

/* Get page descriptor for physical address */
struct page *pmm_phys_to_page(uint64_t phys);

/* Get physical address for page descriptor */
uint64_t pmm_page_to_phys(struct page *page);

/* Convert between physical and page frame number */
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
