/*
 * kernel/include/kernel/platform.h
 * Platform definitions for QEMU Virt machine (AArch64)
 */
#ifndef _KERNEL_PLATFORM_H
#define _KERNEL_PLATFORM_H

#include <kernel/pmm.h>

/* IRQ numbers for QEMU virt machine */
#define PLATFORM_IRQ_TIMER_VIRT 27
#define PLATFORM_IRQ_TIMER_PHYS 30
#define PLATFORM_IRQ_UART0      33
#define PLATFORM_IRQ_VIRTIO_START 48

/* MMIO Addresses */
#define PLATFORM_UART_BASE    0x09000000UL
#define PLATFORM_GICD_BASE    0x08000000UL
#define PLATFORM_GICC_BASE    0x08010000UL

/* Platform Functions */
void arch_platform_early_init(void);
struct mem_region *arch_platform_get_mem_regions(size_t *count);

/* arch_platform_get_boot_module - report a firmware-loaded boot module (the
 * release rootfs image in RAM): amd64 = GRUB multiboot2 MODULE tag; aarch64 =
 * none today (returns 0).  Equivalent HAL contract on both arches; the result
 * is cached on first call so it stays valid after the early identity map is
 * gone.  Returns 1 and fills base/size (physical) when present, else 0. */
int arch_platform_get_boot_module(uint64_t *base, uint64_t *size);

#endif /* _KERNEL_PLATFORM_H */
