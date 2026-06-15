/*
 * kernel/include/kernel/bootmodule.h
 * RAM-backed ramdisk block provider (release storage).
 *
 * The arch-specific "is there a boot module, and where" is the HAL contract
 * arch_platform_get_boot_module() (kernel/platform.h).  This header only
 * declares the block provider's activation; the driver and main.c consume the
 * HAL contract, so nothing here is arch-specific.
 */
#ifndef _KERNEL_BOOTMODULE_H
#define _KERNEL_BOOTMODULE_H

/* Register the RAM-backed ramdisk as the active block backend IFF the HAL
 * reports a boot module (overriding virtio-blk).  No-op otherwise.  Call after
 * virtio_blk_init() and after the module region has been reserved. */
void ramdisk_init(void);

#endif /* _KERNEL_BOOTMODULE_H */
