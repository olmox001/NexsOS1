#ifndef _KERNEL_HAL_DEVICE_H
#define _KERNEL_HAL_DEVICE_H

#include <kernel/types.h>
#include <kernel/hal_unified.h>
#include <kernel/memlayout.h>

/**
 * HAL Bus Type
 */
typedef enum {
    HAL_BUS_TYPE_PLATFORM,
    HAL_BUS_TYPE_PCI,
    HAL_BUS_TYPE_VIRTIO_MMIO
} hal_bus_type_t;

/**
 * HAL Device Resource Type
 */
typedef enum {
    HAL_RES_MMIO,
    HAL_RES_PORT,
    HAL_RES_IRQ,
    HAL_RES_MEM
} hal_res_type_t;

/**
 * HAL Device Resource
 */
struct hal_resource {
    hal_res_type_t type;
    uintptr_t base;
    size_t size;
    uint32_t irq;
};

/**
 * HAL Device Handle
 */
typedef struct {
    hal_res_type_t io_type;
    hal_bus_type_t bus_type;
    uintptr_t base;
    uint32_t irq;
} hal_device_t;

/* --- Register Access Primitives ---
 *
 * dev->base carries the device's PHYSICAL MMIO base; the dereference
 * happens at its direct-map kernel VA (phys_to_virt — identity while
 * KERNEL_VIRT_BASE == 0).  Port I/O (amd64) is unaffected by paging. */

static inline uint8_t hal_dev_read8(hal_device_t *dev, uint32_t reg) {
    uintptr_t addr = dev->base + reg;
    if (dev->io_type == HAL_RES_MMIO) {
        return *(volatile uint8_t *)phys_to_virt(addr);
    } else {
        return hal_inb((uint16_t)addr);
    }
}

static inline void hal_dev_write8(hal_device_t *dev, uint32_t reg, uint8_t val) {
    uintptr_t addr = dev->base + reg;
    if (dev->io_type == HAL_RES_MMIO) {
        *(volatile uint8_t *)phys_to_virt(addr) = val;
    } else {
        hal_outb((uint16_t)addr, val);
    }
}

static inline uint16_t hal_dev_read16(hal_device_t *dev, uint32_t reg) {
    uintptr_t addr = dev->base + reg;
    if (dev->io_type == HAL_RES_MMIO) {
        return *(volatile uint16_t *)phys_to_virt(addr);
    } else {
        return hal_inw((uint16_t)addr);
    }
}

static inline void hal_dev_write16(hal_device_t *dev, uint32_t reg, uint16_t val) {
    uintptr_t addr = dev->base + reg;
    if (dev->io_type == HAL_RES_MMIO) {
        *(volatile uint16_t *)phys_to_virt(addr) = val;
    } else {
        hal_outw((uint16_t)addr, val);
    }
}

static inline uint32_t hal_dev_read32(hal_device_t *dev, uint32_t reg) {
    uintptr_t addr = dev->base + reg;
    if (dev->io_type == HAL_RES_MMIO) {
        return *(volatile uint32_t *)phys_to_virt(addr);
    } else {
        return hal_inl((uint16_t)addr);
    }
}

static inline void hal_dev_write32(hal_device_t *dev, uint32_t reg, uint32_t val) {
    uintptr_t addr = dev->base + reg;
    if (dev->io_type == HAL_RES_MMIO) {
        *(volatile uint32_t *)phys_to_virt(addr) = val;
    } else {
        hal_outl((uint16_t)addr, val);
    }
}

#endif /* _KERNEL_HAL_DEVICE_H */
