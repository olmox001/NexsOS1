#ifndef _KERNEL_BOOT_MEMORY_H
#define _KERNEL_BOOT_MEMORY_H

#include <kernel/pmm.h>
#include <kernel/types.h>

/* Freeze one validated boot-memory snapshot before PMM construction. */
void boot_memory_init(void);

/* Return the immutable snapshot selected by boot_memory_init(). */
struct mem_region *boot_memory_regions(size_t *count);

#endif /* _KERNEL_BOOT_MEMORY_H */
