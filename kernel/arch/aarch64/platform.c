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
            pr_info("AArch64: IDTF/FDT initialized from 0x%lx\n", boot_fdt_ptr);
        } else {
            pr_warn("AArch64: [IDTF] Failed to initialize FDT from 0x%lx! Hardware discovery compromised.\n", boot_fdt_ptr);
        }
    } else {
        pr_warn("%s", "AArch64: [IDTF] No Hardware Description Tree (DTB) found! System will attempt manual discovery.\n");
    }
}

struct mem_region *arch_platform_get_mem_regions(size_t *count) {
    if (arch_region_count > 0) {
        if (count) *count = arch_region_count;
        return arch_mem_regions;
    }

    if (fdt_get_mem_regions(arch_mem_regions, MAX_CPUS * 2, &arch_region_count) == 0 && arch_region_count > 0) {
        for (size_t i = 0; i < arch_region_count; i++) {
            pr_info("AArch64: IDTF RAM Region [%zu]: 0x%lx - 0x%lx\n", 
                    i, arch_mem_regions[i].base, arch_mem_regions[i].base + arch_mem_regions[i].size);
        }
    } else {
        extern volatile bool probe_in_progress;
        extern volatile bool probe_failed;
        
        pr_warn("%s", "AArch64: [IDTF] Memory discovery failed! Attempting manual page-by-page probe...\n");
        
        /* Manual Probe Fallback: Scan RAM in 128MB steps to find the upper limit */
        uint64_t probe_base = 0x40000000UL;
        uint64_t probe_limit = 0x40000000UL + (128UL << 30); /* Max 128GB probe */
        uint64_t found_end = probe_base;
        
        for (uint64_t addr = probe_base + (1UL << 30); addr < probe_limit; addr += 0x08000000UL) {
            probe_in_progress = true;
            probe_failed = false;
            arch_mb();
            
            /* Try to read from the address */
            volatile uint64_t *ptr = (volatile uint64_t *)addr;
            (void)*ptr;
            
            arch_mb();
            if (probe_failed) {
                probe_in_progress = false;
                break;
            }
            
            probe_in_progress = false;
            found_end = addr;
        }
        
        /* Step back slightly to ensure we are in valid RAM */
        if (found_end > probe_base) {
            arch_mem_regions[0].base = probe_base;
            arch_mem_regions[0].size = found_end - probe_base;
            arch_mem_regions[0].type = MEM_REGION_USABLE;
            arch_region_count = 1;
            pr_info("AArch64: Manual discovery found %lu MB RAM\n", arch_mem_regions[0].size / 1024 / 1024);
        }

        /* Safety: Set limit to one page before the last available one */
        arch_mem_regions[0].base = probe_base;
        arch_mem_regions[0].size = (found_end > probe_base) ? (found_end - probe_base - PAGE_SIZE) : 0;
        arch_mem_regions[0].type = MEM_REGION_USABLE;
        arch_region_count = 1;
        
        pr_info("AArch64: Manual probe discovered %lu MB RAM\n", arch_mem_regions[0].size / (1024 * 1024));
    }

    if (count) *count = arch_region_count;
    return arch_mem_regions;
}
extern volatile uint32_t cpu_boot_ack;
extern void *arch_secondary_stacks[MAX_CPUS];
extern void kernel_secondary_main(void);
extern void smp_create_idle_task(uint32_t cpu_id);

void arch_smp_init(void) {
    /* Write TTBR0 for secondary cores via HAL */
    uint64_t current_pgd = arch_vmm_get_pgd();
    arch_vmm_set_secondary_pgd(current_pgd);

    uint32_t cpu_count = fdt_count_cpus();
    if (cpu_count == 0) {
        pr_warn("AArch64: FDT CPU discovery failed, probing up to %d cores\n", MAX_CPUS);
        cpu_count = MAX_CPUS;
    }
    if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;

    pr_info("AArch64: Starting SMP initialization for %u potential cores\n", cpu_count);

    /* Allocate/Setup stacks for all cores before waking them */
    arch_smp_setup_stacks(cpu_count);

    for (uint32_t i = 1; i < cpu_count; i++) {
        void *stack = NULL;
        bool allocated = false;

        if (i < 8) {
            /* Use BSS stack for first 8 cores */
            stack = arch_get_kernel_stack(i);
        } else {
            /* Allocate dynamic stack for cores 9+ */
            void *ptr = pmm_alloc_pages(131072 / 4096);
            if (!ptr) {
                pr_err("AArch64: Failed to allocate stack for CPU %d\n", i);
                break;
            }
            stack = (void *)((uintptr_t)ptr + 131072);
            arch_secondary_stacks[i] = stack;
            allocated = true;
        }

        int ret = arch_cpu_wake_secondary(i, (void (*)(void))kernel_secondary_main, stack);
        
        if (ret == 0) {
            smp_create_idle_task(i);

            /* Wait for CPU to acknowledge boot with timeout */
            volatile uint32_t timeout = 10000000;
            while (cpu_boot_ack != i && timeout > 0) {
                timeout--;
                arch_nop();
            }
            if (timeout == 0) {
                pr_warn("AArch64: CPU %d failed to acknowledge boot (timeout)\n", i);
                if (allocated) {
                    /* Clean up the unused stack */
                    pmm_free_pages((void *)((uintptr_t)stack - 131072), 32);
                    arch_secondary_stacks[i] = 0;
                }
            } else {
                pr_info("AArch64: CPU %d online\n", i);
            }
        } else {
            if (allocated) {
                pmm_free_pages((void *)((uintptr_t)stack - 131072), 32);
                arch_secondary_stacks[i] = 0;
            }
            break;
        }
    }
}
