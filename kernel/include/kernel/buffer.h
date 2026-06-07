/*
 * kernel/include/kernel/buffer.h
 * Buffer Cache (Zero-Copy) for Block Devices
 *
 * Declares the public API for the buffer cache (kernel/mm/buffer.c).
 *
 * The buffer cache sits between the filesystem (ext4) and the VirtIO block
 * driver.  Each cached block occupies one 4KB PMM page (BLOCK_SIZE ==
 * PAGE_SIZE).  buf->data points directly into that PMM page (zero-copy).
 *
 * Callers must bracket every use of a buffer with buffer_get() / buffer_put()
 * to maintain the ref_count; buffers with ref_count > 0 are pinned and will
 * not be evicted.
 *
 * Known issues (see docs/review/analysis/01-mm-memory-management.md):
 *   MM-BUF-01  (W3 BUG)         MAX_BUFFERS not a hard cap.
 *   MM-BUF-02  (W2 PERF)        Duplicate disk reads on concurrent miss.
 *   MM-BUF-03  (W2 BUG/SECURITY) No per-buffer content lock; data race.
 *   MM-BUF-04  (W2 REFINE)      buffer_sync() flushes at most 64 buffers.
 *   MM-BUF-05  (W1 REFINE)      Weak hash and fixed bucket count.
 */
#ifndef _KERNEL_BUFFER_H
#define _KERNEL_BUFFER_H

#include <kernel/list.h>
#include <kernel/types.h>

/* BLOCK_SIZE: filesystem block size; matches PAGE_SIZE (4KB) for zero-copy. */
#define BLOCK_SIZE 4096 /* Matches Page Size */
#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif /* SECTOR_SIZE */
/* SECTORS_PER_BLOCK: number of 512-byte disk sectors per cached block (= 8). */
#define SECTORS_PER_BLOCK (BLOCK_SIZE / SECTOR_SIZE)

/* BUFFER_UPTODATE: buf->data reflects the on-disk content (read has completed). */
#define BUFFER_UPTODATE 0x1
/* BUFFER_DIRTY: buf->data has been modified and must be written to disk before
 * the buffer can be evicted.  Set by filesystem code; cleared by buffer_sync(). */
#define BUFFER_DIRTY 0x2

/*
 * struct block_buffer - cache entry for one 4KB disk block.
 *
 * block:     logical block number (in BLOCK_SIZE units, not sector units).
 *            Callers multiply by SECTORS_PER_BLOCK to get the sector offset.
 * data:      pointer to a 4KB PMM page holding the block content.
 *            NOTE(MM-BUF-03): There is no per-buffer lock protecting data;
 *            concurrent reads and buffer_sync() writes race.
 * flags:     OR of BUFFER_UPTODATE / BUFFER_DIRTY; protected by buffer_lock.
 * ref_count: caller reference count; protected by buffer_lock.
 *            Buffers with ref_count > 0 cannot be evicted.
 * list:      entry in the global LRU list; head = most recently used.
 * hash:      entry in hash_table[bucket] collision list.
 */
struct block_buffer {
  uint64_t block; /* Block index (4KB units) */
  uint8_t *data;  /* Pointer to 4KB page (Zero-copy) */
  uint32_t flags;
  uint32_t ref_count;

  struct list_head list; /* LRU List */
  struct list_head hash; /* Hash Table Collision List */
};

/* API */
void buffer_init(void);

/* Get a buffer for a block. Reads from disk if not cached.
 * Increments ref_count. */
struct block_buffer *buffer_get(uint64_t block);

/* Release a buffer. Decrements ref_count. */
void buffer_put(struct block_buffer *buf);

/* Write all dirty buffers to disk */
void buffer_sync(void);

#endif /* _KERNEL_BUFFER_H */
