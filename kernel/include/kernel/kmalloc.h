/*
 * kernel/include/kernel/kmalloc.h
 * Kernel Heap Allocator
 *
 * Declares the public API of the kernel heap allocator (kernel/lib/kmalloc.c).
 *
 * The allocator uses nine power-of-two buckets (16 B - 4 KB) over a fixed
 * 32 MB bump-pointer pool for small allocations, and passes large requests
 * (total > 4 KB) directly to pmm_alloc_pages().
 *
 * Important constraints:
 *   - The heap does NOT grow; once exhausted, small allocations fail.
 *   - Freed small blocks are NOT returned to the PMM (see MM-KM-01).
 *   - All operations are serialised through a single global spinlock (MM-KM-06).
 *   - Large allocations return a pointer that is NOT page-aligned (MM-KM-03).
 *
 * Known issues (see docs/review/analysis/01-mm-memory-management.md):
 *   MM-KM-01 through MM-KM-06.
 */
#ifndef _KERNEL_KMALLOC_H
#define _KERNEL_KMALLOC_H

#include <kernel/types.h>

/* kmalloc_init: allocate the 32 MB heap pool from PMM; safe to call multiple
 * times (idempotent via heap_initialized flag). */
void kmalloc_init(void);

/* kmalloc: allocate 'size' bytes; returns NULL on size==0 or exhaustion.
 * NOTE(MM-KM-03): large allocs (size+header > 4096) return a non-page-aligned
 * pointer even though the underlying PMM page IS page-aligned. */
void *kmalloc(size_t size);

/* kcalloc: allocate nmemb*size bytes zeroed; overflow-safe. */
void *kcalloc(size_t nmemb, size_t size);

/* krealloc: resize allocation; always alloc+copy+free (no in-place shrink).
 * NOTE(MM-KM-04): does not optimise the case where new_size fits the same bucket. */
void *krealloc(void *ptr, size_t new_size);

/* kfree: return a kmalloc'd pointer.  Small allocations go to the bucket free
 * list (never returned to PMM).  Large allocations are returned to PMM.
 * Panics/logs on invalid magic or out-of-range bucket_idx. */
void kfree(void *ptr);

/* Instrumentation (perf brief §1/§2.2; surfaced via OS1_sys_stats).
 * Reports cumulative pool size (never shrinks today), current live user bytes,
 * the high-water mark of live bytes, and the live allocation count.  All
 * out-params are NULL-skippable. */
void kmalloc_get_stats(uint64_t *heap_total_bytes, uint64_t *bytes_in_use,
                       uint64_t *high_water_bytes, uint64_t *live_allocs);

/* Macros for stb_truetype compatibility */
/* Redirect stb heap calls to kmalloc/kfree. */
#define STBTT_malloc(x, u) ((void)(u), kmalloc(x))
#define STBTT_free(x, u) ((void)(u), kfree(x))

#endif /* _KERNEL_KMALLOC_H */
