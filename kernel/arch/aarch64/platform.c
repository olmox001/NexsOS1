/*
 * kernel/arch/aarch64/platform.c
 * Platform initialization for QEMU Virt (AArch64)
 */
#include <kernel/multiboot2.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/pmm.h>
#include <kernel/platform.h>
#include <kernel/printk.h>
#include <drivers/gic.h>

/* Global memory regions for PMM */
static struct mem_region arch_mem_regions[MAX_CPUS * 2]; /* Enough slots */
static size_t arch_region_count = 0;

/*
 * Perform early platform initialization
 */
void arch_platform_early_init(void) {
    /* 
     * Register the Interrupt Controller.
     */
    gic_register();
}

struct mem_region *arch_platform_get_mem_regions(size_t *count) {
    if (arch_region_count > 0) {
        if (count) *count = arch_region_count;
        return arch_mem_regions;
    }

    /* Parse Multiboot2 info */
    uint64_t boot_info_ptr = arch_get_boot_info();
    if (boot_info_ptr == 0) {
        pr_warn("%s", "AArch64: No boot info found! Using default 1GB RAM.\n");
        if (count) *count = 0;
        return NULL;
    }

    struct kernel_multiboot_info_struct *kmbi = (struct kernel_multiboot_info_struct *)boot_info_ptr;
    struct mb2_tag_mmap *mmap_tag = (struct mb2_tag_mmap *)kmbi->mmap_ptr;

    if (!mmap_tag || mmap_tag->type != MB2_TAG_TYPE_MMAP) {
        pr_warn("%s", "AArch64: No memory map tag found in boot info!\n");
        if (count) *count = 0;
        return NULL;
    }

    uint32_t entry_size = mmap_tag->entry_size;
    uint32_t num_entries = (mmap_tag->size - sizeof(struct mb2_tag_mmap)) / entry_size;

    arch_region_count = 0;
    for (uint32_t i = 0; i < num_entries && arch_region_count < (MAX_CPUS * 2); i++) {
        struct mb2_mmap_entry *entry = (struct mb2_mmap_entry *)((uint8_t *)mmap_tag->entries + i * entry_size);
        
        arch_mem_regions[arch_region_count].base = entry->addr;
        arch_mem_regions[arch_region_count].size = entry->len;
        /* MB2 type 1 is usable RAM */
        arch_mem_regions[arch_region_count].type = (entry->type == 1) ? MEM_REGION_USABLE : MEM_REGION_RESERVED;
        
        pr_info("AArch64: Mem Region [%zu]: 0x%lx - 0x%lx (Type: %d)\n", 
                arch_region_count, entry->addr, entry->addr + entry->len, (int)entry->type);
        
        arch_region_count++;
    }

    if (count) *count = arch_region_count;
    return arch_mem_regions;
}
