/*
 * kernel/drivers/virtio/virtio_hal.c
 * VirtIO HAL Backend - Connects VirtIO drivers to Unified HAL
 */
#include <drivers/virtio.h>
#include <kernel/hal.h>
#include <kernel/string.h>
#include <kernel/printk.h>

#define MAX_VIRTIO_DEVS 16
static struct virtio_device virtio_devices[MAX_VIRTIO_DEVS];
static int virtio_dev_count = 0;

/* --- Register Translation (PCI Modern only) --- */

static uint32_t translate_legacy(uint32_t offset) {
    switch (offset) {
        case VIRTIO_MMIO_DEVICE_FEATURES: return 0x00;
        case VIRTIO_MMIO_DRIVER_FEATURES: return 0x04;
        case VIRTIO_MMIO_QUEUE_PFN:        return 0x08;
        case VIRTIO_MMIO_QUEUE_NUM_MAX:    return 0x0C;
        case VIRTIO_MMIO_QUEUE_NUM:        return 0x0C;
        case VIRTIO_MMIO_QUEUE_SEL:        return 0x0E;
        case VIRTIO_MMIO_QUEUE_NOTIFY:     return 0x10;
        case VIRTIO_MMIO_STATUS:           return 0x12;
        case VIRTIO_MMIO_INTERRUPT_ACK:    return 0x13;
        default: return 0xFFFFFFFF;
    }
}

static uint32_t translate_modern_pci(uint32_t offset) {
    switch (offset) {
        case VIRTIO_MMIO_VERSION:          return 0xFFFFFFFE;
        case VIRTIO_MMIO_DEVICE_FEATURES: return 0x04;
        case VIRTIO_MMIO_DRIVER_FEATURES: return 0x0C;
        case VIRTIO_MMIO_QUEUE_SEL:        return 0x16;
        case VIRTIO_MMIO_QUEUE_NUM_MAX:    return 0x18;
        case VIRTIO_MMIO_QUEUE_NUM:        return 0x18;
        case VIRTIO_MMIO_STATUS:           return 0x14;
        case VIRTIO_MMIO_QUEUE_PFN:        return 0x20;
        default: return 0xFFFFFFFF;
    }
}

/* --- Generic Transport Ops --- */

static uint32_t hal_virtio_read32(struct virtio_device *dev, uint32_t offset) {
    struct hal_device *hdev = (struct hal_device *)dev->priv;
    
    if (dev->is_legacy) {
        uint32_t off = translate_legacy(offset);
        if (off == 0xFFFFFFFF) return 0;
        if (off == 0x12 || off == 0x13) return hal_read8(dev->base + off);
        if (off == 0x0C || off == 0x0E || off == 0x10) return hal_read16(dev->base + off);
        return hal_read32(dev->base + off);
    } else if (hdev && hdev->bus_type == HAL_BUS_TYPE_PCI) {
        /* Modern PCI (even if MMIO) */
        uint32_t off = translate_modern_pci(offset);
        if (off == 0xFFFFFFFE) return 2;
        if (off == 0xFFFFFFFF) return 0;
        if (off == 0x14) return hal_read8(dev->base + off);
        if (off == 0x16 || off == 0x18) return hal_read16(dev->base + off);
        return hal_read32(dev->base + off);
    } else {
        /* MMIO Platform (Direct) */
        return hal_read32(dev->base + offset);
    }
}

static void hal_virtio_write32(struct virtio_device *dev, uint32_t offset, uint32_t val) {
    struct hal_device *hdev = (struct hal_device *)dev->priv;

    if (dev->is_legacy) {
        uint32_t off = translate_legacy(offset);
        if (off == 0xFFFFFFFF) return;
        if (off == 0x12 || off == 0x13) hal_write8(dev->base + off, (uint8_t)val);
        else if (off == 0x0C || off == 0x0E || off == 0x10) hal_write16(dev->base + off, (uint16_t)val);
        else hal_write32(dev->base + off, val);
    } else if (hdev && hdev->bus_type == HAL_BUS_TYPE_PCI) {
        /* Modern PCI */
        uint32_t off = translate_modern_pci(offset);
        if (off == 0xFFFFFFFF || off == 0xFFFFFFFE) return;
        if (off == 0x14) hal_write8(dev->base + off, (uint8_t)val);
        else if (off == 0x16 || off == 0x18) hal_write16(dev->base + off, (uint16_t)val);
        else hal_write32(dev->base + off, val);
    } else {
        /* MMIO Platform (Direct) */
        hal_write32(dev->base + offset, val);
    }
}

static void hal_virtio_notify(struct virtio_device *dev, uint32_t queue_idx) {
    struct hal_device *hdev = (struct hal_device *)dev->priv;

    if (dev->is_legacy) {
        hal_write16(dev->base + 0x10, (uint16_t)queue_idx);
    } else if (hdev && hdev->bus_type == HAL_BUS_TYPE_PCI) {
        /* Modern PCI Notify */
        hal_write16(dev->base + 0x3000, (uint16_t)queue_idx);
    } else {
        /* MMIO Platform Notify */
        hal_write32(dev->base + VIRTIO_MMIO_QUEUE_NOTIFY, queue_idx);
    }
}

static const struct virtio_transport_ops hal_virtio_ops = {
    .read32 = hal_virtio_read32,
    .write32 = hal_virtio_write32,
    .notify = hal_virtio_notify
};

/* --- Bridge --- */

int arch_virtio_get_count(uint32_t device_id) {
    int count = 0;
    while (hal_device_find(0x1AF4, (uint16_t)device_id, count) != NULL) {
        count++;
    }
    return count;
}

int arch_virtio_get_device(uint32_t device_id, int index,
                           virtio_handle_t *out_dev, uint32_t *out_irq) {
    struct hal_device *hdev = hal_device_find(0x1AF4, (uint16_t)device_id, index);
    if (!hdev) return -1;
    
    if (virtio_dev_count >= MAX_VIRTIO_DEVS) return -1;
    
    struct virtio_device *vdev = &virtio_devices[virtio_dev_count++];
    vdev->base = hdev->base;
    vdev->irq = hdev->irq;
    vdev->device_id = device_id;
    vdev->ops = &hal_virtio_ops;
    vdev->priv = hdev;
    
    /* Detect legacy from bus type or address range */
    vdev->is_legacy = (hdev->base < 0x10000 && hdev->bus_type == HAL_BUS_TYPE_PCI); 

    if (out_dev) *out_dev = vdev;
    if (out_irq) *out_irq = vdev->irq;
    return 0;
}

void arch_virtio_scan(void) {}

void virtio_setup_queue(virtio_handle_t dev, uint32_t queue_idx,
                        uint64_t desc_addr, uint64_t avail_addr,
                        uint64_t used_addr) {
    struct hal_device *hdev = (struct hal_device *)dev->priv;

    if (dev->is_legacy) {
        uint32_t pfn = (uint32_t)(desc_addr >> 12);
        hal_write16(dev->base + 0x0E, (uint16_t)queue_idx);
        hal_write32(dev->base + 0x08, pfn);
    } else if (hdev && hdev->bus_type == HAL_BUS_TYPE_PCI) {
        /* Modern PCI */
        virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_SEL, queue_idx);
        hal_write32(dev->base + 0x20, (uint32_t)desc_addr);
        hal_write32(dev->base + 0x24, (uint32_t)(desc_addr >> 32));
        hal_write32(dev->base + 0x28, (uint32_t)avail_addr);
        hal_write32(dev->base + 0x2C, (uint32_t)(avail_addr >> 32));
        hal_write32(dev->base + 0x30, (uint32_t)used_addr);
        hal_write32(dev->base + 0x34, (uint32_t)(used_addr >> 32));
        hal_write16(dev->base + 0x1C, 1); /* queue_enable */
    } else {
        /* MMIO Platform */
        virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_SEL, queue_idx);
        hal_write32(dev->base + 0x028, 4096);
        virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_PFN, desc_addr >> 12);
        virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_READY, 1);
    }
}
