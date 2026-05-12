/*
 * kernel/include/kernel/virtio_hal.h
 * Unified VirtIO Hardware Abstraction
 */
#ifndef _KERNEL_VIRTIO_HAL_H
#define _KERNEL_VIRTIO_HAL_H

#include <kernel/types.h>

/* VirtIO Transport Type */
typedef enum {
    VIRTIO_TRANSPORT_MMIO,
    VIRTIO_TRANSPORT_PCI
} virtio_transport_t;

/* Unified Register Access Ops */
struct virtio_hw_ops {
    uint32_t (*read32)(uintptr_t base, uint32_t offset);
    void (*write32)(uintptr_t base, uint32_t offset, uint32_t val);
    uint16_t (*read16)(uintptr_t base, uint32_t offset);
    void (*write16)(uintptr_t base, uint32_t offset, uint16_t val);
    uint8_t (*read8)(uintptr_t base, uint32_t offset);
    void (*write8)(uintptr_t base, uint32_t offset, uint8_t val);
};

/* VirtIO Device Handle */
struct virtio_device {
    uintptr_t base;
    uint32_t irq;
    virtio_transport_t transport;
    const struct virtio_hw_ops *ops;
    void *priv;
};

/* Discovery API */
int virtio_probe_device(uint32_t device_id, struct virtio_device *out_dev);

#endif /* _KERNEL_VIRTIO_HAL_H */
