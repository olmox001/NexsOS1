#include <os1.h>
#include <stddef.h>
#include <stdint.h>

/*
 * user/lib/malloc.c
 * Basic Heap Allocator for OS1
 */

typedef struct block_header {
    size_t size;
    int free;
    struct block_header *next;
} block_header_t;

#define BLOCK_HEADER_SIZE sizeof(block_header_t)

static block_header_t *free_list = NULL;

void *sbrk(intptr_t increment) {
    return _sys_sbrk(increment);
}

void *malloc(size_t size) {
    if (size == 0) return NULL;

    /* Align size to 16 bytes for performance/compatibility */
    size = (size + 15) & ~15UL;

    block_header_t *current = free_list;
    block_header_t *prev = NULL;

    /* 1. Try to find an existing free block */
    while (current) {
        if (current->free && current->size >= size) {
            /* Split block if it's much larger than requested */
            if (current->size >= size + BLOCK_HEADER_SIZE + 16) {
                block_header_t *new_block = (block_header_t *)((uint8_t *)current + BLOCK_HEADER_SIZE + size);
                new_block->size = current->size - size - BLOCK_HEADER_SIZE;
                new_block->free = 1;
                new_block->next = current->next;
                current->next = new_block;
                current->size = size;
            }
            current->free = 0;
            return (void *)(current + 1);
        }
        prev = current;
        current = current->next;
    }

    /* 2. No suitable block found, request more memory from kernel */
    size_t total_size = size + BLOCK_HEADER_SIZE;
    block_header_t *block = (block_header_t *)sbrk(total_size);
    if (block == (void *)-1) return NULL;

    block->size = size;
    block->free = 0;
    block->next = NULL;

    if (prev) {
        prev->next = block;
    } else {
        free_list = block;
    }

    return (void *)(block + 1);
}

void free(void *ptr) {
    if (!ptr) return;

    block_header_t *block = (block_header_t *)ptr - 1;
    block->free = 1;

    /* Coalesce with next block if it's also free */
    if (block->next && block->next->free) {
        block->size += BLOCK_HEADER_SIZE + block->next->size;
        block->next = block->next->next;
    }

    /* Coalescing with previous would require a full walk or doubly linked list.
     * Given Doom's allocation patterns, this simple strategy should suffice. */
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    block_header_t *block = (block_header_t *)ptr - 1;
    if (block->size >= size) return ptr;

    void *new_ptr = malloc(size);
    if (new_ptr) {
        /* We use block->size here which might be slightly larger than original request
         * but it's safe since it's the actual allocated capacity. */
        memcpy(new_ptr, ptr, block->size);
        free(ptr);
    }
    return new_ptr;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}
