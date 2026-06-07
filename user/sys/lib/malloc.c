/*
 * user/sys/lib/malloc.c
 * Userland Heap Allocator — first-fit with forward coalescing
 *
 * Maintains a singly-linked free list of block_header_t nodes.  Each node is
 * stored immediately before the user-visible payload pointer.  The free list
 * is the same linear chain used for both free and allocated blocks; the
 * 'free' flag distinguishes them.
 *
 * Allocation (malloc):
 *   1. Linear first-fit scan of free_list; splits large free blocks.
 *   2. Falls back to sbrk() -> SYS_SBRK (#216) to extend the heap.
 *
 * Deallocation (free):
 *   Marks the block free and coalesces with the immediately following block
 *   if that block is also free (forward coalescing only; see USR-MALLOC-02/03).
 *
 * This allocator was designed to support Doom's sequential alloc-then-free
 * access pattern; it is not appropriate for long-running services with mixed
 * allocation sizes (see USR-MALLOC-03/04).
 *
 * Known issues:
 *   USR-MALLOC-02 (W2 BAD-IMPL) Forward coalescing assumes block->next is
 *                 physically contiguous with current block.  Only true if
 *                 next was split from current; false for separately sbrk'd
 *                 blocks.  Incorrect coalesce corrupts the free list silently.
 *   USR-MALLOC-03 (W2 WRONG-DESIGN) No backward coalescing; alternating
 *                 alloc/free patterns with different sizes fragment the heap
 *                 permanently.
 *   USR-MALLOC-04 (W2 WRONG-DESIGN) Heap never shrinks; sbrk'd pages are
 *                 never returned to the kernel even when fully free.
 *   USR-MALLOC-05 (W2 BAD-IMPL) Comment claims 16-byte aligned payload.
 *                 block_header_t is 24 bytes on LP64 (size_t 8 + int 4 +
 *                 4 pad + ptr 8), so payload is at offset +24, which is
 *                 8-byte aligned only, not 16.
 *   USR-MALLOC-06 (W1 BAD-IMPL) realloc() uses block->size (the rounded-up
 *                 allocated capacity) in the shrink check; a request that is
 *                 smaller than the rounded size is returned in place, which
 *                 is safe but may return a larger-than-needed block.
 */
#include <os1.h>
#include <stddef.h>
#include <stdint.h>

/*
 * block_header_t - metadata stored immediately before each heap allocation.
 *
 * size: rounded user payload capacity (bytes, 16-byte aligned).
 * free: 1 if this block is on the free list; 0 if allocated.
 * next: pointer to the next block_header in the free list (not necessarily
 *       physically adjacent; see USR-MALLOC-02).
 *
 * NOTE(USR-MALLOC-05): On LP64 (AArch64/x86-64), sizeof(block_header_t) = 24
 * (size_t 8 + int 4 + 4 pad + ptr 8).  The payload starts at offset +24,
 * giving 8-byte alignment, not the 16-byte alignment stated in the malloc()
 * comment.  SIMD callers expecting 16-byte-aligned pointers will be misaligned.
 */
typedef struct block_header {
    size_t size;
    int free;
    struct block_header *next;
} block_header_t;

#define BLOCK_HEADER_SIZE sizeof(block_header_t)

/* free_list: head of the allocation chain.  NULL until the first sbrk() call. */
static block_header_t *free_list = NULL;

/*
 * sbrk - extend the process heap by 'increment' bytes.
 *
 * Delegates to _sys_sbrk (SYS_SBRK, syscall #216).  Returns the old break
 * on success, (void *)-1 on failure (kernel out of memory or quota exceeded).
 *
 * NOTE(USR-MALLOC-04): Heap pages obtained via sbrk are never returned to the
 * kernel; the heap grows monotonically for the lifetime of the process.
 */
void *sbrk(intptr_t increment) {
    return _sys_sbrk(increment);
}

/*
 * malloc - allocate at least 'size' bytes from the userland heap.
 *
 * Rounds size up to the next multiple of 16 for alignment.
 * NOTE(USR-MALLOC-05): The rounded size does NOT account for the 24-byte
 * block_header_t overhead, so the payload is at a +24 offset (8-byte aligned,
 * not 16).
 *
 * First-fit scan:
 *   Walks free_list from the head; stops at the first block with free==1 and
 *   size >= requested.  If the block is at least BLOCK_HEADER_SIZE+16 bytes
 *   larger than needed, it is split: a new block_header_t is carved out of
 *   the tail and inserted after the current block in the list.
 *
 * If no suitable free block is found, calls sbrk(size + BLOCK_HEADER_SIZE)
 * to extend the heap and appends the new block to the list tail.
 *
 * Returns pointer to the payload (just after the header), or NULL on size==0
 * or sbrk failure.
 */
void *malloc(size_t size) {
    if (size == 0) return NULL;

    /* Align size to 16 bytes for performance/compatibility.
     * NOTE(USR-MALLOC-05): Aligns the payload size, but the block_header_t
     * is 24 bytes so the returned pointer is only 8-byte aligned. */
    size = (size + 15) & ~15UL;

    block_header_t *current = free_list;
    block_header_t *prev = NULL;

    /* 1. First-fit: scan free_list for a suitable free block. */
    while (current) {
        if (current->free && current->size >= size) {
            /* Split block if it's much larger than requested.
             * Minimum split threshold: BLOCK_HEADER_SIZE + 16 bytes remaining
             * after the allocation, so the remainder is itself usable. */
            if (current->size >= size + BLOCK_HEADER_SIZE + 16) {
                block_header_t *new_block = (block_header_t *)((uint8_t *)current + BLOCK_HEADER_SIZE + size);
                new_block->size = current->size - size - BLOCK_HEADER_SIZE;
                new_block->free = 1;
                new_block->next = current->next;
                current->next = new_block;
                current->size = size;
            }
            current->free = 0;
            return (void *)(current + 1);  /* Payload starts immediately after header */
        }
        prev = current;
        current = current->next;
    }

    /* 2. No suitable block found: extend heap with sbrk(). */
    size_t total_size = size + BLOCK_HEADER_SIZE;
    block_header_t *block = (block_header_t *)sbrk(total_size);
    if (block == (void *)-1) return NULL;  /* sbrk failed (OOM) */

    block->size = size;
    block->free = 0;
    block->next = NULL;

    /* Link new block at the tail of the free list. */
    if (prev) {
        prev->next = block;
    } else {
        free_list = block;
    }

    return (void *)(block + 1);
}

/*
 * free - return a malloc'd allocation to the free list.
 *
 * Marks the block free and performs forward coalescing with the immediately
 * following block if that block is also free.
 *
 * NOTE(USR-MALLOC-02): Forward coalescing assumes that block->next was split
 * from this block and is therefore physically contiguous.  If block->next was
 * a separate sbrk() region, merging their size fields produces an incorrect
 * oversized block that overlaps unrelated memory.
 *
 * NOTE(USR-MALLOC-03): No backward coalescing; a freed block before the
 * current one is never merged.  Alternate-size alloc/free patterns fragment
 * the list permanently.
 */
void free(void *ptr) {
    if (!ptr) return;

    block_header_t *block = (block_header_t *)ptr - 1;
    block->free = 1;

    /* Forward coalescing: merge with next block if it is also free.
     * NOTE(USR-MALLOC-02): Only safe if block->next is physically contiguous. */
    if (block->next && block->next->free) {
        block->size += BLOCK_HEADER_SIZE + block->next->size;
        block->next = block->next->next;
    }

    /* Coalescing with previous would require a full walk or doubly linked list.
     * Given Doom's allocation patterns, this simple strategy should suffice. */
}

/*
 * realloc - resize a malloc'd allocation.
 *
 * If ptr==NULL: behaves as malloc(size).
 * If size==0: behaves as free(ptr), returns NULL.
 * If block->size >= size: returns ptr unchanged (in-place; no shrink).
 * Otherwise: allocates new buffer, copies block->size bytes (the rounded-up
 *   allocated capacity, which may be slightly larger than the original request),
 *   frees old buffer, returns new pointer.
 *
 * NOTE(USR-MALLOC-06): `block->size >= size` uses the rounded-up allocated
 * size, not the original user request.  A request that fits within the rounded
 * capacity is returned in-place even when the caller intends to shrink — safe
 * but wastes memory on repeated shrink calls.
 *
 * Returns new allocation, or NULL on malloc failure (old ptr NOT freed).
 */
void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    block_header_t *block = (block_header_t *)ptr - 1;
    /* In-place: block already has enough capacity for the rounded size. */
    if (block->size >= size) return ptr;

    void *new_ptr = malloc(size);
    if (new_ptr) {
        /* Copy using block->size (allocated capacity) not original user size.
         * NOTE(USR-MALLOC-06): The comment below reflects that block->size may
         * be slightly larger than the original request, but the copy is safe
         * since the source buffer is at least that large. */
        memcpy(new_ptr, ptr, block->size);
        free(ptr);
    }
    return new_ptr;
}

/*
 * calloc - allocate nmemb*size bytes, zeroed.
 *
 * FIX(USR-MALLOC-01): the nmemb*size product is overflow-checked before the
 * multiply runs.  Without the guard a wrapping product yields a small 'total',
 * malloc() returns an undersized buffer, and the caller's subsequent nmemb*size
 * write overflows the heap (classic calloc overflow -> heap corruption).  The
 * pre-multiply form (size > SIZE_MAX / nmemb) rejects exactly the inputs whose
 * product would wrap, without relying on the wraparound having already happened;
 * legitimate calls are unaffected.  nmemb==0 short-circuits to malloc(0)->NULL,
 * preserving the prior behaviour for zero-count requests.
 */
void *calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > SIZE_MAX / nmemb) return NULL;  /* FIX(USR-MALLOC-01) */

    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}
