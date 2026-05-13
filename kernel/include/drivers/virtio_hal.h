#ifndef _DRIVERS_VIRTIO_HAL_H
#define _DRIVERS_VIRTIO_HAL_H

#include <kernel/types.h>
#include <kernel/hal_device.h>

/* VirtIO Transport Type is now mapped to hal_res_type_t */
typedef hal_device_t virtio_device_t;

static inline uint32_t virtio_read32(virtio_device_t *dev, uint32_t offset) {
    return hal_dev_read32(dev, offset);
}

static inline void virtio_write32(virtio_device_t *dev, uint32_t offset, uint32_t val) {
    hal_dev_write32(dev, offset, val);
}

static inline uint16_t virtio_read16(virtio_device_t *dev, uint32_t offset) {
    return hal_dev_read16(dev, offset);
}

static inline void virtio_write16(virtio_device_t *dev, uint32_t offset, uint16_t val) {
    hal_dev_write16(dev, offset, val);
}

static inline uint8_t virtio_read8(virtio_device_t *dev, uint32_t offset) {
    return hal_dev_read8(dev, offset);
}

static inline void virtio_write8(virtio_device_t *dev, uint32_t offset, uint8_t val) {
    hal_dev_write8(dev, offset, val);
}

#endif
