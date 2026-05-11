/*
 * kernel/mm/pmm.c
 * Physical Memory Manager
 *
 * Bitmap-based page frame allocator with zone support.
 */
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

/* Memory configuration for QEMU virt */
#define MEMORY_BASE 0x40000000UL  /* DRAM starts at 1GB */
#define DMA_ZONE_END 0x41000000UL /* 16MB for DMA zone */

/* Maximum supported memory (1GB for now) */
#define MAX_MEMORY (1UL << 30)
#define MAX_PAGES (MAX_MEMORY / PAGE_SIZE)

/* Page array - one descriptor per physical page */
static struct page page_array[MAX_PAGES];

/* Zones */
static struct zone zones[ZONE_COUNT];

/* Bitmap for allocation (1 bit per page) */
static uint64_t dma_bitmap[MAX_PAGES / 64 / 16]; /* DMA zone bitmap */
static uint64_t normal_bitmap[MAX_PAGES / 64];   /* Normal zone bitmap */

/* Global statistics */
static uint64_t total_pages;
static uint64_t free_pages;

/*
 * Mark a page as used in the bitmap
 */
static void bitmap_set(uint64_t *bitmap, uint64_t bit) {
  bitmap[bit / 64] |= (1UL << (bit % 64));
}

/*
 * Mark a page as free in the bitmap
 */
static void bitmap_clear(uint64_t *bitmap, uint64_t bit) {
  bitmap[bit / 64] &= ~(1UL << (bit % 64));
}

/*
 * Check if a page is free
 */
static int bitmap_test(uint64_t *bitmap, uint64_t bit) {
  return (bitmap[bit / 64] & (1UL << (bit % 64))) != 0;
}

/*
 * Find first free bit in bitmap
 */
static int64_t bitmap_find_free(uint64_t *bitmap, uint64_t start,
                                uint64_t end) {
  for (uint64_t i = start; i < end; i++) {
    if (!bitmap_test(bitmap, i)) {
      return i;
    }
  }
  return -1;
}

/*
 * Find contiguous free pages
 */
static int64_t bitmap_find_contiguous(uint64_t *bitmap, uint64_t start,
                                      uint64_t end, uint64_t count) {
  uint64_t found = 0;
  uint64_t first = 0;

  for (uint64_t i = start; i < end; i++) {
    if (!bitmap_test(bitmap, i)) {
      if (found == 0)
        first = i;
      found++;
      if (found == count)
        return first;
    } else {
      found = 0;
    }
  }
  return -1;
}

/*
 * Initialize a zone
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
 * Initialize PMM with memory regions
 */
void pmm_init(struct mem_region *regions, size_t count) {
  uint64_t mem_end = MEMORY_BASE;

  /* Find total memory from regions */
  if (regions && count > 0) {
    for (size_t i = 0; i < count; i++) {
      if (regions[i].type == MEM_REGION_USABLE) {
        uint64_t end = regions[i].base + regions[i].size;
        if (end > mem_end)
          mem_end = end;
      }
    }
  } else {
    /* Default: assume 1GB RAM starting at MEMORY_BASE */
    mem_end = MEMORY_BASE + MAX_MEMORY;
  }

  /* Clamp to maximum supported */
  if (mem_end > MEMORY_BASE + MAX_MEMORY) {
    mem_end = MEMORY_BASE + MAX_MEMORY;
  }

  total_pages = (mem_end - MEMORY_BASE) / PAGE_SIZE;
  free_pages = 0;

  /* Initialize page array */
  memset(page_array, 0, sizeof(page_array));

  /* Initialize zones */
  uint64_t dma_end_pfn = phys_to_pfn(DMA_ZONE_END - MEMORY_BASE);
  uint64_t normal_end_pfn = total_pages;

  zone_init(&zones[ZONE_DMA], "DMA", 0, dma_end_pfn, dma_bitmap);
  zone_init(&zones[ZONE_NORMAL], "Normal", dma_end_pfn, normal_end_pfn,
            normal_bitmap);

  free_pages = zones[ZONE_DMA].free_pages + zones[ZONE_NORMAL].free_pages;

  /* Mark kernel pages as reserved */
  extern char __kernel_start[], __kernel_end[];
  uint64_t kernel_start_pfn =
      phys_to_pfn((uint64_t)__kernel_start - MEMORY_BASE);
  uint64_t kernel_end_pfn =
      phys_to_pfn(PAGE_ALIGN((uint64_t)__kernel_end) - MEMORY_BASE);

  for (uint64_t pfn = kernel_start_pfn; pfn < kernel_end_pfn; pfn++) {
    struct page *pg = &page_array[pfn];
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
    free_pages--;
  }

  pr_info("PMM: %lu MB total, %lu MB free\n",
          total_pages * PAGE_SIZE / (1024 * 1024),
          free_pages * PAGE_SIZE / (1024 * 1024));
  pr_info("PMM: DMA zone: %lu pages, Normal zone: %lu pages\n",
          zones[ZONE_DMA].free_pages, zones[ZONE_NORMAL].free_pages);
}

/*
 * Allocate a single page from specified zone
 */
static void *zone_alloc_page(struct zone *z) {
  uint64_t flags;
  spin_lock_irqsave(&z->lock, &flags);

  uint64_t npages = z->end_pfn - z->start_pfn;
  int64_t pfn = bitmap_find_free(z->bitmap, z->next_free_pfn, npages);
  if (pfn < 0) {
    /* Wrap around and search from the beginning */
    pfn = bitmap_find_free(z->bitmap, 0, z->next_free_pfn);
  }

  if (pfn < 0) {
    spin_unlock_irqrestore(&z->lock, flags);
    return NULL;
  }

  /* Update next_free_pfn for next caller */
  z->next_free_pfn = (pfn + 1) % npages;

  bitmap_set(z->bitmap, pfn);
  z->free_pages--;

  spin_unlock_irqrestore(&z->lock, flags);

  /* Convert to absolute PFN and get physical address */
  uint64_t abs_pfn = z->start_pfn + pfn;
  struct page *pg = &page_array[abs_pfn];
  pg->flags = 0;
  pg->refcount = 1;

  void *addr = (void *)(MEMORY_BASE + pfn_to_phys(abs_pfn));
  memset(addr, 0, PAGE_SIZE);
  arch_cache_clean_range(addr, PAGE_SIZE);
  arch_data_barrier();

  __sync_fetch_and_sub(&free_pages, 1);

  return addr;
}

/*
 * Allocate a single page
 */
void *pmm_alloc_page(void) {
  /* Try normal zone first, then DMA */
  void *page = zone_alloc_page(&zones[ZONE_NORMAL]);
  if (!page) {
    page = zone_alloc_page(&zones[ZONE_DMA]);
  }
  return page;
}

/*
 * Allocate multiple contiguous pages
 */
void *pmm_alloc_pages(size_t count) {
  if (count == 0)
    return NULL;
  if (count == 1)
    return pmm_alloc_page();

  struct zone *z = &zones[ZONE_NORMAL];
  uint64_t flags;

  spin_lock_irqsave(&z->lock, &flags);

  int64_t pfn =
      bitmap_find_contiguous(z->bitmap, 0, z->end_pfn - z->start_pfn, count);
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

  void *addr = (void *)(MEMORY_BASE + pfn_to_phys(abs_pfn));
  memset(addr, 0, PAGE_SIZE * count);

  __sync_fetch_and_sub(&free_pages, count);

  return addr;
}

/*
 * Free a single page
 */
void pmm_free_page(void *page) {
  if (!page)
    return;

  uint64_t phys = (uint64_t)page;
  if (phys < MEMORY_BASE) {
    pr_err("PMM: Attempt to free invalid address %p (below MEMORY_BASE)\n",
           page);
    return;
  }

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
}

/*
 * Free multiple contiguous pages
 */
void pmm_free_pages(void *page, size_t count) {
  uint64_t phys = (uint64_t)page;
  for (size_t i = 0; i < count; i++) {
    pmm_free_page((void *)(phys + i * PAGE_SIZE));
  }
}

/*
 * Allocate with specific alignment (for block I/O)
 */
void *pmm_alloc_aligned(size_t size, size_t align) {
  size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  size_t align_pages = (align + PAGE_SIZE - 1) / PAGE_SIZE;

  if (align_pages <= 1) {
    return pmm_alloc_pages(pages);
  }

  /* Allocate extra pages to ensure alignment */
  size_t total = pages + align_pages - 1;
  void *mem = pmm_alloc_pages(total);
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
 */
struct page *pmm_phys_to_page(uint64_t phys) {
  if (phys < MEMORY_BASE)
    return NULL;
  uint64_t pfn = phys_to_pfn(phys - MEMORY_BASE);
  if (pfn >= total_pages)
    return NULL;
  return &page_array[pfn];
}

/*
 * Get physical address for page descriptor
 */
uint64_t pmm_page_to_phys(struct page *page) {
  uint64_t pfn = page - page_array;
  return MEMORY_BASE + pfn_to_phys(pfn);
}

/*
 * Statistics
 */
uint64_t pmm_get_free_pages(void) { return free_pages; }

uint64_t pmm_get_total_pages(void) { return total_pages; }

void pmm_dump_stats(void) {
  pr_info("%s", "PMM Statistics:\n");
  pr_info("  Total: %lu pages (%lu MB)\n", total_pages,
          total_pages * PAGE_SIZE / (1024 * 1024));
  pr_info("  Free:  %lu pages (%lu MB)\n", free_pages,
          free_pages * PAGE_SIZE / (1024 * 1024));
  pr_info("  Used:  %lu pages (%lu MB)\n", total_pages - free_pages,
          (total_pages - free_pages) * PAGE_SIZE / (1024 * 1024));
}
