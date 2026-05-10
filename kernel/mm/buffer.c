/*
 * kernel/mm/buffer.c
 * Zero-Copy Buffer Cache Implementation
 */
#include <drivers/virtio_blk.h>
#include <kernel/buffer.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>

#include <kernel/kmalloc.h>
#include <kernel/spinlock.h>

#define HASH_BUCKETS 64

static struct list_head lru_list;
static struct list_head hash_table[HASH_BUCKETS];
static DEFINE_SPINLOCK(buffer_lock);

/* Simple hash function */
static uint32_t hash_block(uint64_t block) { return block % HASH_BUCKETS; }

void buffer_init(void) {
  pr_info("%s", "BufferCache: Initializing...\n");
  INIT_LIST_HEAD(&lru_list);
  for (int i = 0; i < HASH_BUCKETS; i++) {
    INIT_LIST_HEAD(&hash_table[i]);
  }
}

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

struct block_buffer *buffer_get(uint64_t block) {
  uint64_t flags;
  spin_lock_irqsave(&buffer_lock, &flags);

  /* 1. Check Cache */
  struct block_buffer *buf = __lookup(block);

  if (buf) {
    /* Move to head of LRU (most recently used) */
    if (!list_empty(&buf->list)) {
      list_move(&buf->list, &lru_list);
    }
    buf->ref_count++;
    spin_unlock_irqrestore(&buffer_lock, flags);
    return buf;
  }
  spin_unlock_irqrestore(&buffer_lock, flags);

  /* 2. Allocate New Buffer (Metadata) */
  buf = (struct block_buffer *)kmalloc(sizeof(struct block_buffer));
  if (!buf)
    return NULL;
  memset(buf, 0, sizeof(*buf));

  /* 3. Allocate Data Page */
  buf->data = (uint8_t *)pmm_alloc_page();
  if (!buf->data) {
    kfree(buf);
    return NULL;
  }

  /* 4. Read from Disk (OUTSIDE lock to avoid blocking other CPUs) */
  /* Block is 4KB = 8 sectors */
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
  /* Double check if someone else loaded it while we were reading */
  struct block_buffer *exists = __lookup(block);
  if (exists) {
    pmm_free_page(buf->data);
    kfree(buf);
    exists->ref_count++;
    spin_unlock_irqrestore(&buffer_lock, flags);
    return exists;
  }

  uint32_t bucket = hash_block(block);
  list_add(&buf->hash, &hash_table[bucket]);
  list_add(&buf->list, &lru_list);
  spin_unlock_irqrestore(&buffer_lock, flags);

  return buf;
}

void buffer_put(struct block_buffer *buf) {
  if (!buf)
    return;
  uint64_t flags;
  spin_lock_irqsave(&buffer_lock, &flags);
  if (buf->ref_count > 0)
    buf->ref_count--;
  spin_unlock_irqrestore(&buffer_lock, flags);
}

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
