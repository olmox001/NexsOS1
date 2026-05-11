/*
 * kernel/include/kernel/platform.h
 * Platform definitions for QEMU Virt machine (AArch64)
 */
#ifndef _KERNEL_PLATFORM_H
#define _KERNEL_PLATFORM_H

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
void platform_early_init(void);

#endif /* _KERNEL_PLATFORM_H */
