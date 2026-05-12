#ifndef _KERNEL_FDT_H
#define _KERNEL_FDT_H

#include <kernel/types.h>
#include <kernel/pmm.h>

#define FDT_MAGIC 0xd00dfeed
extern uint64_t boot_fdt_ptr;

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

#define FDT_BEGIN_NODE 0x00000001
#define FDT_END_NODE   0x00000002
#define FDT_PROP       0x00000003
#define FDT_NOP        0x00000004
#define FDT_END        0x00000009

struct fdt_reserve_entry {
    uint64_t address;
    uint64_t size;
};

/* API */
int fdt_init(uintptr_t fdt_addr);
int fdt_get_mem_regions(struct mem_region *regions, size_t max_count, size_t *count);
uint32_t fdt_count_cpus(void);
uintptr_t fdt_find_in_memory(uintptr_t start, uintptr_t end);
#endif /* _KERNEL_FDT_H */
