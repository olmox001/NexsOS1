/*
 * kernel/arch/aarch64/platform.c
 * Platform initialization for QEMU Virt (AArch64)
 */
#include <kernel/platform.h>
#include <drivers/gic.h>
#include <drivers/uart.h>

/*
 * Perform early platform initialization
 */
void arch_platform_early_init(void) {
    /* 
     * Register the Interrupt Controller.
     * In a real multi-platform build, this would be selected
     * based on Device Tree or ACPI.
     */
    gic_register();
}

struct mem_region *arch_platform_get_mem_regions(size_t *count) {
    if (count) *count = 0;
    return NULL;
}
