#ifndef _KERNEL_FDT_H
#define _KERNEL_FDT_H

#include <kernel/nx_contract.h>
#include <kernel/fdt_parser.h>
#include <kernel/pmm.h>
#include <kernel/types.h>

/* Validated DTB physical location retained for early memory ownership. */
extern uint64_t boot_fdt_ptr;
extern uint64_t boot_fdt_size;

/* Normal path: caller supplies a physical base and the mapped available span. */
int fdt_init(uintptr_t base, size_t available_length) NX_MUST_USE;

/* Explicit compatibility adapter for loaders that provide only x0/DTB base.
 * A non-zero pointer uses a trusted-loader, 16 MiB maximum-size assumption.
 * A zero pointer searches the bounded legacy QEMU RAM window page by page. */
int fdt_init_legacy(uintptr_t base, size_t *actual_length) NX_MUST_USE;

/* Returns usable RAM plus RESERVED overlays (FDT reserve map, /reserved-memory,
 * and the DTB allocation). FDT_PARSER_NO_MEMORY preserves reserved entries so
 * the platform can invoke its explicit legacy RAM probe without losing them. */
int fdt_get_mem_regions(struct mem_region *regions, size_t max_count,
                        size_t *count) NX_MUST_USE;

uint32_t fdt_count_cpus(void);

/* Bounded legacy search. Returns physical address and the validated DTB size. */
uintptr_t fdt_find_in_memory(uintptr_t start, uintptr_t end,
                             size_t *found_size) NX_MUST_USE;

#endif /* _KERNEL_FDT_H */
