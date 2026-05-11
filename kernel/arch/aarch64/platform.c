/*
 * kernel/arch/aarch64/platform.c
 * Platform initialization for QEMU Virt (AArch64)
 */
#include <kernel/platform.h>
#include <drivers/gic.h>
#include <drivers/uart.h>

void platform_early_init(void) {
    /* 
     * Register the Interrupt Controller.
     * In a real multi-platform build, this would be selected
     * based on Device Tree or ACPI.
     */
    gic_register();
}
