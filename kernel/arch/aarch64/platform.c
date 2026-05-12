/*
 * kernel/arch/aarch64/platform.c
 * Platform initialization for QEMU Virt (AArch64)
 */
#include <kernel/fdt.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/pmm.h>
#include <kernel/platform.h>
#include <kernel/printk.h>
#include <drivers/gic.h>
#include <drivers/uart.h>

/* Global memory regions for PMM */
static struct mem_region arch_mem_regions[MAX_CPUS * 2]; /* Enough slots */
static size_t arch_region_count = 0;

/*
 * Perform early platform initialization
 */
void arch_platform_early_init(void) {
    uart_puts("PLATFORM: arch_platform_early_init starting\n");
    /* 
     * Register the Interrupt Controller.
     */
    gic_register();

    /* Initialize FDT parser */
    if (boot_fdt_ptr) {
        if (fdt_init(boot_fdt_ptr) == 0) {
            pr_info("AArch64: FDT initialized from 0x%lx\n", boot_fdt_ptr);
        } else {
            pr_warn("AArch64: Failed to initialize FDT from 0x%lx!\n", boot_fdt_ptr);
        }
    } else {
        pr_warn("%s", "AArch64: No DTB pointer found (boot_fdt_ptr is NULL)!\n");
    }
}

struct mem_region *arch_platform_get_mem_regions(size_t *count) {
    if (arch_region_count > 0) {
        if (count) *count = arch_region_count;
        return arch_mem_regions;
    }

    if (fdt_get_mem_regions(arch_mem_regions, MAX_CPUS * 2, &arch_region_count) == 0 && arch_region_count > 0) {
        for (size_t i = 0; i < arch_region_count; i++) {
            pr_info("AArch64: FDT RAM Region [%zu]: 0x%lx - 0x%lx\n", 
                    i, arch_mem_regions[i].base, arch_mem_regions[i].base + arch_mem_regions[i].size);
        }
    } else {
        pr_warn("%s", "AArch64: FDT memory discovery failed! Using 1GB fallback.\n");
        arch_mem_regions[0].base = 0x40000000UL;
        arch_mem_regions[0].size = 0x40000000UL;
        arch_mem_regions[0].type = MEM_REGION_USABLE;
        arch_region_count = 1;
    }

    if (count) *count = arch_region_count;
    return arch_mem_regions;
}
