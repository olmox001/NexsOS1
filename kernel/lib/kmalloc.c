/*
 * kernel/lib/kmalloc.c
 * Kernel Heap Allocator (Bucket/Slab-like)
 *
 * Uses power-of-two free lists for small allocations (16B to 4KB).
 * Uses page allocator for large allocations.
 *
 * Architecture:
 *   Small allocations (user-size + header <= 4096 bytes) are served from a
 *   32 MB bump-pointer pool (heap_base..heap_end) carved from contiguous PMM
 *   pages at init time.  Nine power-of-two buckets (16, 32, 64, 128, 256,
 *   512, 1024, 2048, 4096 bytes) each maintain a LIFO free list of returned
 *   blocks.  A freed small block goes onto the head of its bucket's list and
 *   is reused by the next same-size request.
 *
 *   Large allocations (user-size + header > 4096 bytes) are served directly
 *   from pmm_alloc_pages(); freed large blocks are returned to the PMM.
 *
 *   Every allocation is preceded by a 32-byte struct block_header
 *   (sizeof = 20 bytes, but __attribute__((aligned(16))) pads to 32 bytes).
 *
 * Known issues:
 *   MM-KM-01  (W3 MISSING/WRONG-DESIGN) Bump-pointer only grows; freed small
 *             blocks stay in per-bucket lists and are NEVER returned to the PMM.
 *             Once heap_ptr reaches heap_end, small allocs fail permanently even
 *             if free lists contain blocks of other sizes.  Not suitable for
 *             long-running workloads.
 *   MM-KM-02  (W2 WRONG-DESIGN) No cross-bucket reuse: a freed 512B block
 *             cannot satisfy a 256B request; structural fragmentation is baked
 *             into the design.
 *   MM-KM-03  (W2 BUG) Large allocations return page_start + 32 (not
 *             page-aligned) -- a footgun for callers expecting page alignment.
 *   MM-KM-04  (W2 REFINE) krealloc() always alloc+copy+free, even when the
 *             new size fits in the same bucket.
 *   MM-KM-05  (W1 PERF) The 32-byte header on a 16-byte minimum bucket means
 *             kmalloc(1) consumes a 64-byte bucket slot (header 32 + user 1
 *             rounds up to next bucket: 64B bucket, not 32B).
 *   MM-KM-06  (W2 PERF) Single global spinlock for all allocations; no per-CPU
 *             magazines; heavy SMP contention expected at scale.
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

/* BLOCK_MAGIC: sentinel written to block_header.magic for allocated blocks.
 * kfree() validates this before accepting a pointer.
 * NOTE(MM-KM-06): The magic check is done under kmalloc_lock to prevent an
 * SMP double-free race (two CPUs both reading BLOCK_MAGIC and both proceeding). */
#define BLOCK_MAGIC 0xDEADBEEF
/* BLOCK_FREE: magic value written when a block is on a bucket free list.
 * Helps catch use-after-free dereferences that read the header region. */
#define BLOCK_FREE 0xDEADDEAD

/* Block header */
/*
 * struct block_header - per-allocation metadata stored immediately before the
 *                        user-visible data pointer.
 *
 * size:       user-requested size (bytes), not the bucket slot size.
 * next:       free-list pointer; valid only when magic == BLOCK_FREE.
 * bucket_idx: index 0..NUM_BUCKETS-1 for small allocations; 0xFFFFFFFF for
 *             large allocations (served via pmm_alloc_pages).
 *
 * NOTE(MM-KM-05): __attribute__((aligned(16))) makes sizeof(block_header)==32,
 * not 20.  A kmalloc(1) call needs total_req = 1 + 32 = 33 bytes, which falls
 * in the 64-byte bucket (idx 2), wasting 31 bytes of user payload capacity.
 *
 * NOTE(MM-KM-03): For large allocations, (struct block_header *)ptr + 1
 * evaluates to ptr + 32, which is NOT page-aligned even though ptr came from
 * pmm_alloc_pages().  Callers that need a page-aligned large allocation will
 * receive a misaligned pointer.
 */
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
/* heap_base/heap_ptr/heap_end: delimit the 32 MB bump-pointer pool.
 * heap_ptr advances on every small allocation carved from the pool.
 * NOTE(MM-KM-01): heap_ptr never retreats; once it reaches heap_end the pool
 * is permanently exhausted, even if free lists contain many returned blocks. */
static uint8_t *heap_base = NULL;
static uint8_t *heap_ptr = NULL;
static uint8_t *heap_end = NULL;

/* buckets[i]: head of the LIFO free list for bucket index i.
 * NOTE(MM-KM-02): Blocks freed to bucket[i] can only satisfy future requests
 * that map to the same bucket index; no cross-bucket reuse occurs. */
static struct block_header *buckets[NUM_BUCKETS];
static int heap_initialized = 0;
/* kmalloc_lock: global spinlock protecting heap_ptr, buckets[], and the magic
 * field of every block_header for small allocations.
 * NOTE(MM-KM-06): A single lock serialises all allocators on all CPUs. */
static DEFINE_SPINLOCK(kmalloc_lock);

/* Convert size to bucket index. Returns -1 if too large. */
/*
 * get_bucket_index - map a total allocation size (user + header) to a bucket.
 *
 * Returns 0..NUM_BUCKETS-1 for sizes that fit in a small bucket, or -1 for
 * sizes > 4096 bytes (large path).
 *
 * The caller passes (user_size + sizeof(struct block_header)) as 'size', so
 * the bucket chosen is the smallest power-of-two slot that holds both the
 * header and user payload.
 *
 * NOTE(MM-KM-05): Because sizeof(struct block_header)==32, a user request of
 * 1 byte requires total_req=33, which lands in the 64-byte bucket (not 32).
 */
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

/* get_bucket_size - return the total slot size (header + payload) for bucket idx.
 * Bucket 0 = 16 bytes, bucket 1 = 32 bytes, ..., bucket 8 = 4096 bytes. */
static size_t get_bucket_size(int idx) { return 16 << idx; }

/*
 * Initialize kernel heap
 *
 * kmalloc_init - allocate the 32 MB small-object heap from the PMM.
 *
 * Calls pmm_alloc_pages() to obtain HEAP_SIZE/PAGE_SIZE contiguous pages.
 * Sets heap_base, heap_ptr, heap_end, and clears all bucket free-list heads.
 * Sets heap_initialized = 1 to skip re-init on subsequent calls.
 *
 * Locking: acquires kmalloc_lock with IRQ save/restore; safe to call before
 *          SMP is active or after (idempotent via heap_initialized guard).
 *
 * NOTE(MM-KM-06): The heap is a single 32 MB PMM allocation; it is NOT
 * grown later.  If the bump pointer reaches heap_end all subsequent small
 * allocations fail permanently (see MM-KM-01).
 */
void kmalloc_init(void) {
  uint64_t flags;
  uint32_t pages;
  
  spin_lock_irqsave(&kmalloc_lock, &flags);

  if (heap_initialized) {
    goto out;
  }

  /* Allocate memory pool for small allocations */
  pages = (HEAP_SIZE + 4095) / 4096;
  heap_base = (uint8_t *)pmm_alloc_pages(pages);

  if (!heap_base) {
    pr_err("%s", "kmalloc: Failed to allocate heap\n");
    goto out;
  }

  heap_end = heap_base + HEAP_SIZE;
  heap_ptr = heap_base;

  for (int i = 0; i < NUM_BUCKETS; i++) {
    buckets[i] = NULL;
  }

  heap_initialized = 1;

  pr_info("kmalloc: Initialized bucket allocator. Heap: %u MB at %p\n",
          HEAP_SIZE / (1024 * 1024), heap_base);

out:
  spin_unlock_irqrestore(&kmalloc_lock, flags);
}

/*
 * Allocate memory
 *
 * kmalloc - allocate 'size' bytes of kernel memory.
 *
 * Small path (total_req = size + sizeof(block_header) <= 4096):
 *   1. Compute bucket index and slot size.
 *   2. Under kmalloc_lock: pop the bucket free list if non-empty (reuse).
 *   3. Otherwise: advance heap_ptr by slot size; fail if heap exhausted.
 *   4. Initialise block_header (magic=BLOCK_MAGIC, size, bucket_idx).
 *   5. Return pointer to byte immediately after the header (user payload).
 *
 * Large path (total_req > 4096):
 *   Releases kmalloc_lock before calling pmm_alloc_pages() to avoid holding
 *   the spinlock across PMM (which takes its own zone lock).
 *   NOTE(MM-KM-03): Returns (block_header *)ptr + 1 = ptr + 32, which is
 *   not page-aligned.  Callers expecting page alignment will be misled.
 *
 * NOTE(MM-KM-01): Freed small blocks returned to bucket free lists are NEVER
 * given back to the PMM.  Once heap_ptr reaches heap_end the allocator fails
 * permanently for small objects, even if free lists are non-empty.
 *
 * NOTE(MM-KM-06): kmalloc_lock is a global spinlock; all CPUs serialise here.
 *
 * Returns: pointer to user payload, or NULL on size 0, overflow, or exhaustion.
 */
void *kmalloc(size_t size) {
  void *res = NULL;
  uint64_t flags = 0;
  int idx;
  size_t total_req;

  if (!heap_initialized)
    kmalloc_init();

  if (size == 0)
    return NULL;

  if (size > (size_t)(-1) - sizeof(struct block_header))
    return NULL;

  /* Add header overhead */
  total_req = size + sizeof(struct block_header);

  /* Determine bucket */
  idx = get_bucket_index(total_req);

  spin_lock_irqsave(&kmalloc_lock, &flags);

  if (idx >= 0) {
    /* Small allocation from bucket */
    size_t bucket_sz = get_bucket_size(idx);

    /* Check free list for this bucket */
    if (buckets[idx]) {
      struct block_header *blk = buckets[idx];
      buckets[idx] = blk->next;
      blk->magic = BLOCK_MAGIC;
      blk->size = size;
      blk->next = NULL;
      blk->bucket_idx = idx;

      res = (void *)(blk + 1);
      goto out;
    }

    /* No free block, carve from heap_ptr */
    size_t alloc_sz = bucket_sz;
    if (heap_ptr + alloc_sz <= heap_end) {
      struct block_header *blk = (struct block_header *)heap_ptr;
      heap_ptr += alloc_sz;

      blk->magic = BLOCK_MAGIC;
      blk->size = size;
      blk->next = NULL;
      blk->bucket_idx = idx;

      res = (void *)(blk + 1);
      goto out;
    }

    /* Heap exhausted for small objects */
    pr_err("kmalloc: Small heap exhausted (request %lu)\n", size);
    res = NULL;
    goto out;

  } else {
    /* Large allocation: via PMM directly */
    /* Unlock before calling PMM to avoid nesting */
    spin_unlock_irqrestore(&kmalloc_lock, flags);
    flags = 0; // Prevent double unlock

    size_t pages = (total_req + 4095) / 4096;
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

out:
  if (flags) {
    spin_unlock_irqrestore(&kmalloc_lock, flags);
  }
  return res;
}

/*
 * Allocate zeroed memory
 *
 * kcalloc - allocate nmemb*size bytes, zeroed.
 *
 * Overflow-safe: returns NULL if nmemb * size would overflow size_t.
 * Delegates to kmalloc() and zeros the result with memset().
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
 *
 * kfree - return a kmalloc'd allocation.
 *
 * Acquires kmalloc_lock BEFORE reading block_header.magic.  This prevents an
 * SMP double-free race where two CPUs each see magic==BLOCK_MAGIC and both
 * proceed to "free" the block.  The deliberate, correct ordering is:
 *   lock -> read magic -> validate -> mutate magic -> act.
 *
 * Large allocations (bucket_idx == 0xFFFFFFFF): zero the magic, release the
 * lock, then call pmm_free_pages().  The lock is NOT held during the PMM call.
 *
 * Small allocations: write magic=BLOCK_FREE, push block onto the bucket free
 * list head (LIFO), release lock.
 * NOTE(MM-KM-01): The freed block is placed in the bucket free list only;
 * it is never returned to the PMM bump pool.
 * NOTE(MM-KM-02): The block is returned to its original bucket only; it
 * cannot be reused by a request for a different bucket size.
 *
 * Locking: acquires/releases kmalloc_lock; NOT safe to call from NMI context.
 */
void kfree(void *ptr) {
  if (!ptr)
    return;

  uint64_t flags;
  struct block_header *blk = ((struct block_header *)ptr) - 1;

  /* Acquire lock BEFORE reading magic to prevent SMP double-free race:
   * two CPUs could both see BLOCK_MAGIC and both proceed to free. */
  spin_lock_irqsave(&kmalloc_lock, &flags);

  if (blk->magic != BLOCK_MAGIC) {
    spin_unlock_irqrestore(&kmalloc_lock, flags);
    pr_err("kfree: Invalid magic %x at %p\n", blk->magic, ptr);
    return;
  }

  if (blk->bucket_idx == 0xFFFFFFFF) {
    /* Large allocation - free pages */
    size_t total_req = blk->size + sizeof(struct block_header);
    size_t pages = (total_req + 4095) / 4096;
    blk->magic = 0;
    spin_unlock_irqrestore(&kmalloc_lock, flags);
    pmm_free_pages((void *)blk, pages);
    return;
  }

  /* Small allocation - return to bucket free list */
  if (blk->bucket_idx >= NUM_BUCKETS) {
    spin_unlock_irqrestore(&kmalloc_lock, flags);
    pr_err("kfree: Invalid bucket index %d\n", blk->bucket_idx);
    return;
  }

  blk->magic = BLOCK_FREE;
  blk->next = buckets[blk->bucket_idx];
  buckets[blk->bucket_idx] = blk;

  spin_unlock_irqrestore(&kmalloc_lock, flags);
}

/*
 * Reallocate memory
 *
 * krealloc - resize a kmalloc'd allocation.
 *
 * If ptr==NULL, behaves as kmalloc(new_size).
 * If new_size==0, behaves as kfree(ptr) and returns NULL.
 * Otherwise: allocates a new buffer, copies min(old_size, new_size) bytes,
 * and frees the old buffer.
 *
 * NOTE(MM-KM-04): Always performs alloc+copy+free regardless of whether
 * the new_size fits within the same bucket as the original allocation.
 * A shrink from 200 bytes to 100 bytes (both bucket idx 3: <=128 + header
 * exceeds 128, so bucket 4: <=256) still allocates a new block, copies, and
 * frees the old one -- even though the old block could be reused directly.
 *
 * Reads old_size from block_header under kmalloc_lock to avoid a race with
 * a concurrent kfree() on the same pointer.
 *
 * Returns: pointer to new allocation, or NULL on failure (old allocation NOT
 *          freed on NULL return -- standard krealloc contract).
 */
void *krealloc(void *ptr, size_t new_size) {
  if (!ptr)
    return kmalloc(new_size);
  if (new_size == 0) {
    kfree(ptr);
    return NULL;
  }

  /* Read size while holding lock to avoid race with concurrent kfree */
  uint64_t flags;
  spin_lock_irqsave(&kmalloc_lock, &flags);
  struct block_header *blk = ((struct block_header *)ptr) - 1;
  if (blk->magic != BLOCK_MAGIC) {
    spin_unlock_irqrestore(&kmalloc_lock, flags);
    return NULL;
  }
  size_t old_size = blk->size;
  spin_unlock_irqrestore(&kmalloc_lock, flags);

  void *new_ptr = kmalloc(new_size);
  if (!new_ptr)
    return NULL;

  size_t copy_size = (old_size < new_size) ? old_size : new_size;
  memcpy(new_ptr, ptr, copy_size);
  kfree(ptr);

  return new_ptr;
}
