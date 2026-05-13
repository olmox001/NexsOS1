/*
 * kernel/include/kernel/buffer.h
 * Buffer Cache (Zero-Copy) for Block Devices
 */
#ifndef _KERNEL_BUFFER_H
#define _KERNEL_BUFFER_H

#include <kernel/list.h>
#include <kernel/types.h>

#define BLOCK_SIZE 4096 /* Matches Page Size */
#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif /* SECTOR_SIZE */
#define SECTORS_PER_BLOCK (BLOCK_SIZE / SECTOR_SIZE)

#define BUFFER_UPTODATE 0x1
#define BUFFER_DIRTY 0x2

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
