/*
 * kernel/mm/buffer.c
 * Zero-Copy Buffer Cache Implementation
 *
 * Provides a block-level cache between the filesystem (ext4) and the VirtIO
 * block device driver.  Each cached block occupies exactly one 4KB PMM page
 * (matching the filesystem block size).  The "zero-copy" label refers to the
 * fact that struct block_buffer.data points directly into a PMM page rather
 * than copying into a separate heap buffer.
 *
 * Structure:
 *   - hash_table[HASH_BUCKETS]: an array of singly-linked collision lists,
 *     indexed by hash_block(block_number).  Used for O(1) average lookup.
 *   - lru_list: doubly-linked LRU list for eviction ordering; most recently
 *     used buffers are at the head.
 *   - buffer_lock: a single global spinlock protecting both data structures
 *     and the ref_count and flags fields of every struct block_buffer.
 *
 * Known issues:
 *   MM-BUF-01  (W3 BUG)         __evict_buffers() skips referenced/dirty
 *                                buffers; if all MAX_BUFFERS slots are dirty or
 *                                referenced, eviction frees nothing but
 *                                buffer_get() continues allocating -- MAX_BUFFERS
 *                                is not a hard cap.
 *   MM-BUF-02  (W2 PERF)        Two CPUs missing the same block both allocate
 *                                a buffer and both issue a disk read; the loser
 *                                is discarded after the lock is re-acquired.
 *   MM-BUF-03  (W2 BUG/SECURITY) No per-buffer content lock: buffer_sync()
 *                                writes buf->data while readers may be reading
 *                                it concurrently -- data race.
 *   MM-BUF-04  (W2 REFINE)      buffer_sync() flushes at most 64 dirty
 *                                buffers per call; excess remain dirty.
 *   MM-BUF-05  (W1 REFINE)      Weak hash (block % 64) and fixed bucket count.
 */
#include <drivers/virtio_blk.h>
#include <kernel/buffer.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#include <kernel/kmalloc.h>
#include <kernel/spinlock.h>

#define HASH_BUCKETS 64
/* Number of hash buckets; NOTE(MM-BUF-05): fixed, no dynamic resize. */

static struct list_head lru_list;
static struct list_head hash_table[HASH_BUCKETS];
/* buffer_lock: single global lock protecting hash_table, lru_list, total_buffers,
 * and the ref_count/flags fields of every struct block_buffer.
 * NOTE(MM-BUF-03): buf->data content is NOT protected by any lock; readers and
 * buffer_sync() can race on the same data page. */
static DEFINE_SPINLOCK(buffer_lock);

/* Simple hash function */
/*
 * hash_block - map a block number to a hash_table bucket index.
 *
 * NOTE(MM-BUF-05): Uses a trivial modulo hash; sequential block numbers map
 * to sequential buckets, which is poor distribution for sequential workloads.
 */
static uint32_t hash_block(uint64_t block) { return block % HASH_BUCKETS; }

/*
 * buffer_init - initialise the buffer cache data structures.
 *
 * Initialises the LRU list and all HASH_BUCKETS collision lists.
 * Must be called once at boot before buffer_get() or buffer_sync().
 * Not re-entrant; must be called single-threaded.
 */
void buffer_init(void) {
  pr_info("%s", "BufferCache: Initializing...\n");
  INIT_LIST_HEAD(&lru_list);
  for (int i = 0; i < HASH_BUCKETS; i++) {
    INIT_LIST_HEAD(&hash_table[i]);
  }
}

/*
 * __lookup - search the hash table for a cached buffer for 'block'.
 *
 * Must be called with buffer_lock held.
 * Returns a pointer to the matching block_buffer, or NULL if not cached.
 */
static struct block_buffer *__lookup(uint64_t block) {
  uint32_t bucket = hash_block(block);
  struct block_buffer *pos;

  list_for_each_entry(pos, &hash_table[bucket], hash) {
    if (pos->block == block) {
      return pos;
    }
  }
  return NULL;
}

/* MAX_BUFFERS: target eviction threshold; NOT a hard cap (see MM-BUF-01). */
#define MAX_BUFFERS 1024
static int total_buffers = 0;

/*
 * __evict_buffers - attempt to free buffers until total_buffers <= MAX_BUFFERS/2.
 *
 * Walks the LRU list tail-to-head (oldest first) and frees any buffer whose
 * ref_count is 0 and BUFFER_DIRTY flag is clear.
 *
 * Must be called with buffer_lock held.
 *
 * NOTE(MM-BUF-01): If all buffers above MAX_BUFFERS/2 are referenced (ref_count>0)
 * or dirty, the loop exits without freeing anything.  The caller (buffer_get())
 * then continues to allocate additional buffers beyond MAX_BUFFERS, so the cap
 * is not enforced.  Over time this can exhaust PMM pages.
 */
static void __evict_buffers(void) {
  struct block_buffer *pos, *n;
  /* Evict oldest buffers first (LRU) */
  list_for_each_entry_safe_reverse(pos, n, &lru_list, list) {
    if (total_buffers <= MAX_BUFFERS / 2) break;
    if (pos->ref_count == 0 && !(pos->flags & BUFFER_DIRTY)) {
      list_del(&pos->hash);
      list_del(&pos->list);
      pmm_free_page(pos->data);
      kfree(pos);
      total_buffers--;
    }
  }
}

/*
 * buffer_get - retrieve (or load) the cache entry for disk block 'block'.
 *
 * Protocol:
 *   1. Acquire buffer_lock; look up block in hash table.
 *      Hit: move to LRU head, bump ref_count, release lock, return.
 *   2. If total_buffers >= MAX_BUFFERS, call __evict_buffers() (see MM-BUF-01).
 *   3. Release lock; allocate struct block_buffer (kmalloc) and data page (PMM).
 *   4. Issue virtio_blk_read() WITHOUT the lock to avoid holding the spinlock
 *      across potentially-blocking disk I/O.
 *   5. Re-acquire lock; re-check hash table for a race winner (MM-BUF-02).
 *      If another CPU loaded the same block while we were reading, discard our
 *      buffer and return the winner's.
 *   6. Insert into hash table and LRU, increment total_buffers, release lock.
 *
 * NOTE(MM-BUF-02): If two CPUs concurrently miss on the same block, both
 * allocate a page and both issue a disk read.  The loser's page is freed after
 * re-acquiring the lock, but two DMA reads were still issued unnecessarily.
 * An in-flight table (mapping block -> pending read) would prevent this.
 *
 * Increments ref_count on the returned buffer.  Caller must call buffer_put()
 * when finished.
 *
 * Returns: pointer to the block_buffer, or NULL on allocation or I/O failure.
 * Locking: acquires/releases buffer_lock internally; must NOT be called with
 *          buffer_lock already held.
 */
struct block_buffer *buffer_get(uint64_t block) {
  uint64_t flags;
  struct block_buffer *buf = NULL;
  struct block_buffer *exists = NULL;

  spin_lock_irqsave(&buffer_lock, &flags);

  /* 1. Check Cache */
  buf = __lookup(block);
  if (buf) {
    if (!list_empty(&buf->list)) {
      list_move(&buf->list, &lru_list);
    }
    buf->ref_count++;
    spin_unlock_irqrestore(&buffer_lock, flags);
    return buf;
  }

  /* Check if we need to evict before allocating */
  if (total_buffers >= MAX_BUFFERS) {
    __evict_buffers();
  }
  spin_unlock_irqrestore(&buffer_lock, flags);

  /* 2. Allocate New Buffer (Metadata) */
  buf = (struct block_buffer *)kmalloc(sizeof(struct block_buffer));
  if (!buf) return NULL;
  memset(buf, 0, sizeof(*buf));

  /* 3. Allocate Data Page */
  buf->data = (uint8_t *)pmm_alloc_page();
  if (!buf->data) {
    kfree(buf);
    return NULL;
  }

  /* 4. Read from Disk (OUTSIDE lock) */
  if (virtio_blk_read(buf->data, block * SECTORS_PER_BLOCK,
                      SECTORS_PER_BLOCK) != 0) {
    pr_err("BufferCache: Disk read error block %ld\n", block);
    pmm_free_page(buf->data);
    kfree(buf);
    return NULL;
  }

  buf->block = block;
  buf->flags = BUFFER_UPTODATE;
  buf->ref_count = 1;

  /* 5. Insert into Hash and LRU */
  spin_lock_irqsave(&buffer_lock, &flags);
  exists = __lookup(block);
  if (exists) {
    /* Someone else loaded it while we were reading */
    pmm_free_page(buf->data);
    kfree(buf);
    exists->ref_count++;
    spin_unlock_irqrestore(&buffer_lock, flags);
    return exists;
  }

  uint32_t bucket = hash_block(block);
  list_add(&buf->hash, &hash_table[bucket]);
  list_add(&buf->list, &lru_list);
  total_buffers++;
  spin_unlock_irqrestore(&buffer_lock, flags);

  return buf;
}

/*
 * buffer_put - release a caller's reference on a buffer.
 *
 * Decrements buf->ref_count under buffer_lock.  When ref_count reaches zero
 * the buffer becomes eligible for eviction by __evict_buffers() but is not
 * immediately freed.
 *
 * Locking: acquires/releases buffer_lock.
 */
void buffer_put(struct block_buffer *buf) {
  if (!buf)
    return;
  uint64_t flags;
  spin_lock_irqsave(&buffer_lock, &flags);
  if (buf->ref_count > 0)
    buf->ref_count--;
  spin_unlock_irqrestore(&buffer_lock, flags);
}

/*
 * buffer_sync - write dirty buffers to disk.
 *
 * Collects up to MAX_DIRTY (64) dirty buffers under buffer_lock, temporarily
 * incrementing their ref_count to pin them.  Then, for each dirty buffer,
 * issues a virtio_blk_write() WITHOUT buffer_lock to avoid holding the spinlock
 * during I/O.  Clears BUFFER_DIRTY and decrements ref_count under the lock
 * after each write.
 *
 * NOTE(MM-BUF-04): At most 64 buffers are flushed per call.  If more than 64
 * buffers are dirty, the remainder are silently left dirty until the next
 * buffer_sync() call.
 *
 * NOTE(MM-BUF-03): Writing buf->data (virtio_blk_write) while another CPU
 * reads it (e.g. an ext4 path that already holds a reference) constitutes an
 * unsynchronised data race.  There is no per-buffer content lock.
 *
 * Locking: acquires/releases buffer_lock around the collection phase and
 *          around each per-buffer flag update; NOT held during disk I/O.
 */
void buffer_sync(void) {
#define MAX_DIRTY 64
  struct block_buffer *dirty[MAX_DIRTY];
  int ndirty = 0;

  uint64_t flags;
  spin_lock_irqsave(&buffer_lock, &flags);
  struct block_buffer *buf;
  list_for_each_entry(buf, &lru_list, list) {
    if ((buf->flags & BUFFER_DIRTY) && ndirty < MAX_DIRTY) {
      buf->ref_count++;
      dirty[ndirty++] = buf;
    }
  }
  spin_unlock_irqrestore(&buffer_lock, flags);

  for (int i = 0; i < ndirty; i++) {
    virtio_blk_write(dirty[i]->data, dirty[i]->block * SECTORS_PER_BLOCK,
                     SECTORS_PER_BLOCK);
    spin_lock_irqsave(&buffer_lock, &flags);
    dirty[i]->flags &= ~BUFFER_DIRTY;
    if (dirty[i]->ref_count > 0)
      dirty[i]->ref_count--;
    spin_unlock_irqrestore(&buffer_lock, flags);
  }
}
