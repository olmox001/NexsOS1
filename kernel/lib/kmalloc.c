/*
 * kernel/lib/kmalloc.c
 * Kernel Heap Allocator (Bucket/Slab-like)
 *
 * Uses power-of-two free lists for small allocations (16B to 4KB).
 * Uses page allocator for large allocations.
 */
#include <kernel/kmalloc.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/types.h>

#define HEAP_SIZE (32 * 1024 * 1024) /* Increased to 32 MB heap */
#define MIN_BUCKET_SIZE 16
#define MAX_BUCKET_SIZE 4096
#define NUM_BUCKETS 9 /* 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 */

#define BLOCK_MAGIC 0xDEADBEEF
#define BLOCK_FREE 0xDEADDEAD

/* Block header */
struct block_header {
  uint32_t magic;
  uint32_t size;             /* User size */
  struct block_header *next; /* In free list */
  uint32_t bucket_idx;       /* 0..NUM_BUCKETS-1, or -1 if large */
} __attribute__((aligned(16)));

/* Global heap state */
/* We allocate heap pages from PMM for small objects.
 * Large objects go directly to PMM.
 */
static uint8_t *heap_base = NULL;
static uint8_t *heap_ptr = NULL;
static uint8_t *heap_end = NULL;

static struct block_header *buckets[NUM_BUCKETS];
static int heap_initialized = 0;
static DEFINE_SPINLOCK(kmalloc_lock);

/* Convert size to bucket index. Returns -1 if too large. */
static int get_bucket_index(size_t size) {
  if (size <= 16)
    return 0;
  if (size <= 32)
    return 1;
  if (size <= 64)
    return 2;
  if (size <= 128)
    return 3;
  if (size <= 256)
    return 4;
  if (size <= 512)
    return 5;
  if (size <= 1024)
    return 6;
  if (size <= 2048)
    return 7;
  if (size <= 4096)
    return 8;
  return -1;
}

static size_t get_bucket_size(int idx) { return 16 << idx; }

/*
 * Initialize kernel heap
 */
void kmalloc_init(void) {
  uint64_t flags;
  spin_lock_irqsave(&kmalloc_lock, &flags);

  if (heap_initialized) {
    spin_unlock_irqrestore(&kmalloc_lock, flags);
    return;
  }

  /* Allocate memory pool for small allocations */
  uint32_t pages = (HEAP_SIZE + 4095) / 4096;
  heap_base = (uint8_t *)pmm_alloc_pages(pages);

  if (!heap_base) {
    pr_err("%s", "kmalloc: Failed to allocate heap\n");
    spin_unlock_irqrestore(&kmalloc_lock, flags);
    return;
  }

  heap_end = heap_base + HEAP_SIZE;
  heap_ptr = heap_base;

  for (int i = 0; i < NUM_BUCKETS; i++) {
    buckets[i] = NULL;
  }

  heap_initialized = 1;

  pr_info("kmalloc: Initialized bucket allocator. Heap: %u MB at %p\n",
          HEAP_SIZE / (1024 * 1024), heap_base);
  spin_unlock_irqrestore(&kmalloc_lock, flags);
}

/*
 * Allocate memory
 */
void *kmalloc(size_t size) {
  if (!heap_initialized)
    kmalloc_init();
  if (size == 0)
    return NULL;

  if (size > (size_t)(-1) - sizeof(struct block_header))
    return NULL;

  /* Add header overhead */
  size_t total_req = size + sizeof(struct block_header);

  /* Determine bucket */
  int idx = get_bucket_index(total_req);

  uint64_t flags;
  spin_lock_irqsave(&kmalloc_lock, &flags);

  if (idx >= 0) {
    /* Small allocation from bucket */
    size_t bucket_sz = get_bucket_size(idx);

    /* Check free list for this bucket */
    if (buckets[idx]) {
      struct block_header *blk = buckets[idx];
      buckets[idx] = blk->next;
      blk->magic = BLOCK_MAGIC;
      blk->size =
          size; /* Store requested size for info, bucket size is implied */
      blk->next = NULL;
      blk->bucket_idx = idx;

      spin_unlock_irqrestore(&kmalloc_lock, flags);
      return (void *)(blk + 1);
    }

    /* No free block, carve from heap_ptr */
    /* Align to bucket size (optional but good for slab alignment) or just 16 */
    /* Ensure sufficient space */
    size_t alloc_sz = bucket_sz;
    /* Ensure header fits. bucket_sz always >= 16. Header is 16 bytes. */
    /* Wait, if bucket is 16, header is 16, user gets 0?
       Actually get_bucket_index(total_req) ensures total_req <= bucket_size.
       So if user asks for 1 byte, total=17. bucket=32 (index 1).
       If user asks for 16, total=32. bucket=32.
    */

    if (heap_ptr + alloc_sz <= heap_end) {
      struct block_header *blk = (struct block_header *)heap_ptr;
      heap_ptr += alloc_sz;

      blk->magic = BLOCK_MAGIC;
      blk->size = size;
      blk->next = NULL;
      blk->bucket_idx = idx;

      spin_unlock_irqrestore(&kmalloc_lock, flags);
      return (void *)(blk + 1);
    }

    /* Heap exhausted for small objects */
    pr_err("kmalloc: Small heap exhausted (request %lu)\n", size);
    spin_unlock_irqrestore(&kmalloc_lock, flags);
    return NULL;

  } else {
    /* Large allocation: via PMM directly */
    /* We still need a header to track it for kfree?
       Yes, otherwise kfree won't know size to free pages.
    */
    size_t pages = (total_req + 4095) / 4096;
    spin_unlock_irqrestore(&kmalloc_lock, flags); /* PMM has its own lock */

    void *ptr = pmm_alloc_pages(pages);
    if (!ptr)
      return NULL;

    struct block_header *blk = (struct block_header *)ptr;
    blk->magic = BLOCK_MAGIC;
    blk->size = size;
    blk->next = NULL;
    blk->bucket_idx = 0xFFFFFFFF; // Mark as large

    return (void *)(blk + 1);
  }
}

/*
 * Allocate zeroed memory
 */
void *kcalloc(size_t nmemb, size_t size) {
  if (nmemb && size > (size_t)(-1) / nmemb)
    return NULL;
  size_t total = nmemb * size;
  void *ptr = kmalloc(total);
  if (ptr) {
    memset(ptr, 0, total);
  }
  return ptr;
}

/*
 * Free memory
 */
void kfree(void *ptr) {
  if (!ptr)
    return;

  uint64_t flags;
  /* Read header (unsafe if invalid ptr, but standard risk) */
  struct block_header *blk = ((struct block_header *)ptr) - 1;

  if (blk->magic != BLOCK_MAGIC) {
    pr_err("kfree: Invalid magic %x at %p\n", blk->magic, ptr);
    return;
  }

  if (blk->bucket_idx == 0xFFFFFFFF) {
    /* Large allocation - free pages */
    /* Recalculate pages */
    size_t total_req = blk->size + sizeof(struct block_header);
    size_t pages = (total_req + 4095) / 4096;

    blk->magic = 0; /* Clear magic */
    pmm_free_pages((void *)blk, pages);
    return;
  }

  /* Small allocation - return to bucket free list */
  if (blk->bucket_idx >= NUM_BUCKETS) {
    pr_err("kfree: Invalid bucket index %d\n", blk->bucket_idx);
    return;
  }

  spin_lock_irqsave(&kmalloc_lock, &flags);

  blk->magic = BLOCK_FREE;
  blk->next = buckets[blk->bucket_idx];
  buckets[blk->bucket_idx] = blk;

  spin_unlock_irqrestore(&kmalloc_lock, flags);
}

/*
 * Reallocate memory
 */
void *krealloc(void *ptr, size_t new_size) {
  if (!ptr)
    return kmalloc(new_size);
  if (new_size == 0) {
    kfree(ptr);
    return NULL;
  }

  struct block_header *blk = ((struct block_header *)ptr) - 1;
  if (blk->magic != BLOCK_MAGIC)
    return NULL;

  /* If it fits in old block (and not shrinking too much to bother), return same
   */
  /* Actually with buckets, if new_size matches same bucket, we can keep it. */
  /* But let's simplify: simple alloc+copy+free */

  void *new_ptr = kmalloc(new_size);
  if (!new_ptr)
    return NULL;

  size_t old_size = blk->size;
  size_t copy_size = (old_size < new_size) ? old_size : new_size;

  memcpy(new_ptr, ptr, copy_size);
  kfree(ptr);

  return new_ptr;
}
