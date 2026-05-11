#include <kernel/arch.h>
#include <kernel/printk.h>
#include <kernel/vmm.h>
#include <drivers/virtio.h>
#include <drivers/pci.h>

/* Flag to indicate a Modern (MMIO) base address in our internal HAL */
#define VIRTIO_BASE_MODERN_FLAG (1ULL << 62)

#define MAX_VIRTIO_DEVS 8
#define VIRTIO_HANDLE_FLAG (1ULL << 63)

struct virtio_pci_dev {
    uintptr_t common_base;
    uintptr_t notify_base;
    uint32_t notify_off_multiplier;
    uint8_t is_modern;
    uint16_t legacy_port;
    uint16_t device_id;
    uint32_t irq;
};

static struct virtio_pci_dev virtio_devices[MAX_VIRTIO_DEVS];
static int virtio_dev_count = 0;

/* Translation from common MMIO offsets to Legacy PCI offsets (I/O Port) */
static uint32_t translate_legacy(uint32_t offset) {
    switch (offset) {
        case VIRTIO_MMIO_DEVICE_FEATURES: return 0x00;
        case VIRTIO_MMIO_DRIVER_FEATURES: return 0x04;
        case VIRTIO_MMIO_QUEUE_PFN:       return 0x08;
        case VIRTIO_MMIO_QUEUE_NUM_MAX:   return 0x0C;
        case VIRTIO_MMIO_QUEUE_NUM:       return 0x0C;
        case VIRTIO_MMIO_QUEUE_SEL:       return 0x0E;
        case VIRTIO_MMIO_QUEUE_NOTIFY:    return 0x10;
        case VIRTIO_MMIO_STATUS:          return 0x12;
        case VIRTIO_MMIO_INTERRUPT_STATUS: return 0x13;
        case VIRTIO_MMIO_INTERRUPT_ACK:    return 0x13;
        default: return 0xFFFFFFFF;
    }
}

/* Translation from common MMIO offsets to Modern PCI Common Config offsets */
static uint32_t translate_modern(uint32_t offset) {
    switch (offset) {
        case VIRTIO_MMIO_DEVICE_FEATURES: return 0x04; // device_feature
        case VIRTIO_MMIO_DRIVER_FEATURES: return 0x0C; // driver_feature
        case VIRTIO_MMIO_QUEUE_SEL:       return 0x16; // queue_select
        case VIRTIO_MMIO_QUEUE_NUM_MAX:   return 0x18; // queue_size
        case VIRTIO_MMIO_QUEUE_NUM:       return 0x18; // queue_size
        case VIRTIO_MMIO_STATUS:          return 0x14; // device_status
        case VIRTIO_MMIO_QUEUE_READY:     return 0x1C; // queue_enable
        case VIRTIO_MMIO_QUEUE_DESC_LOW:  return 0x20; // queue_desc_lo
        case VIRTIO_MMIO_QUEUE_DESC_HIGH: return 0x24; // queue_desc_hi
        case VIRTIO_MMIO_QUEUE_DRIVER_LOW: return 0x28; // queue_avail_lo
        case VIRTIO_MMIO_QUEUE_DRIVER_HIGH: return 0x2C; // queue_avail_hi
        case VIRTIO_MMIO_QUEUE_DEVICE_LOW: return 0x30; // queue_used_lo
        case VIRTIO_MMIO_QUEUE_DEVICE_HIGH: return 0x34; // queue_used_hi
        default: return 0xFFFFFFFF;
    }
}

uint32_t virtio_read_reg(uintptr_t handle, uint32_t offset) {
    if (!(handle & VIRTIO_HANDLE_FLAG)) return 0;
    int idx = handle & 0xFF;
    struct virtio_pci_dev *dev = &virtio_devices[idx];

    if (offset == VIRTIO_MMIO_MAGIC_VALUE) return 0x74726976;
    if (offset == VIRTIO_MMIO_VERSION) return dev->is_modern ? 2 : 1;

    if (dev->is_modern) {
        uint32_t mod_off = translate_modern(offset);
        if (mod_off == 0xFFFFFFFF) return 0;
        return *(volatile uint32_t *)(dev->common_base + mod_off);
    } else {
        uint32_t pci_off = translate_legacy(offset);
        if (pci_off == 0xFFFFFFFF) return 0;
        uint16_t port = dev->legacy_port + pci_off;
        if (pci_off == 0x12 || pci_off == 0x13) return inb(port);
        if (pci_off == 0x0C || pci_off == 0x0E || pci_off == 0x10) return inw(port);
        return inl(port);
    }
}

void virtio_write_reg(uintptr_t handle, uint32_t offset, uint32_t val) {
    if (!(handle & VIRTIO_HANDLE_FLAG)) return;
    int idx = handle & 0xFF;
    struct virtio_pci_dev *dev = &virtio_devices[idx];

    if (dev->is_modern) {
        uint32_t mod_off = translate_modern(offset);
        if (mod_off != 0xFFFFFFFF) {
            *(volatile uint32_t *)(dev->common_base + mod_off) = val;
        }
    } else {
        uint32_t pci_off = translate_legacy(offset);
        if (pci_off == 0xFFFFFFFF) return;
        uint16_t port = dev->legacy_port + pci_off;
        if (pci_off == 0x12) outb(port, (uint8_t)val);
        else if (pci_off == 0x0C || pci_off == 0x0E || pci_off == 0x10) outw(port, (uint16_t)val);
        else outl(port, val);
    }
}

void virtio_notify(uintptr_t handle, uint32_t queue_idx) {
    if (!(handle & VIRTIO_HANDLE_FLAG)) return;
    int idx = handle & 0xFF;
    struct virtio_pci_dev *dev = &virtio_devices[idx];

    if (dev->is_modern) {
        /* Modern: notify offset = common_cfg.queue_notify_off * notify_off_multiplier */
        /* For simplicity, we read queue_notify_off from common_cfg */
        uint16_t notify_off = *(volatile uint16_t *)(dev->common_base + 0x1E);
        uintptr_t notify_addr = dev->notify_base + (notify_off * dev->notify_off_multiplier);
        *(volatile uint16_t *)notify_addr = (uint16_t)queue_idx;
    } else {
        /* Legacy: write queue index to notify port */
        outw(dev->legacy_port + 0x10, (uint16_t)queue_idx);
    }
}

static uint32_t pci_get_bar_size(uint32_t bdf, int bar_index) {
    uint8_t bus = (bdf >> 16) & 0xFF;
    uint8_t dev = (bdf >> 8) & 0xFF;
    uint8_t func = bdf & 0xFF;
    uint8_t offset = 0x10 + (bar_index * 4);
    
    uint32_t old = pci_config_read(bus, dev, func, offset);
    pci_config_write(bus, dev, func, offset, 0xFFFFFFFF);
    uint32_t size_mask = pci_config_read(bus, dev, func, offset);
    pci_config_write(bus, dev, func, offset, old);

    if (size_mask == 0) return 0;
    
    /* Clear flags */
    if (size_mask & 1) { /* I/O */
        size_mask &= ~0x3;
    } else { /* Memory */
        size_mask &= ~0xF;
    }
    
    return ~size_mask + 1;
}

static uintptr_t register_vdev(uint32_t bdf, uint16_t v_id, int is_modern) {
    (void)v_id;
    if (virtio_dev_count >= MAX_VIRTIO_DEVS) return 0;
    struct virtio_pci_dev *vdev = &virtio_devices[virtio_dev_count];
    vdev->is_modern = is_modern;

    if (is_modern) {
        uint8_t bus = (bdf >> 16) & 0xFF;
        uint8_t dev_pci = (bdf >> 8) & 0xFF;
        uint8_t func = bdf & 0xFF;
        /* Modern: Scan capabilities for Common and Notify configs */
        for (uint8_t ptr = pci_config_read(bus, dev_pci, func, 0x34) & 0xFF; ptr >= 0x40; ) {
            uint32_t val = pci_config_read(bus, dev_pci, func, ptr);
            if ((val & 0xFF) == 0x09) { /* Vendor Specific (VirtIO Capability) */
                uint8_t type = (val >> 24) & 0xFF;
                uint8_t bar = pci_config_read(bus, dev_pci, func, ptr + 4) & 0xFF;
                uint32_t off = pci_config_read(bus, dev_pci, func, ptr + 8);
                uintptr_t bar_phys = pci_get_bar(bdf, bar) & ~0xF;
                uint32_t bar_size = pci_get_bar_size(bdf, bar);

                /* Map the whole BAR dynamically */
                extern uint64_t *kernel_pgd;
                for (uint32_t p = 0; p < bar_size; p += 4096) {
                    vmm_map_page(kernel_pgd, bar_phys + p, bar_phys + p, PAGE_DEVICE);
                }

                if (type == 1) vdev->common_base = bar_phys + off;
                if (type == 2) {
                    vdev->notify_base = bar_phys + off;
                    vdev->notify_off_multiplier = pci_config_read(bus, dev_pci, func, ptr + 12);
                }
            }
            ptr = (val >> 8) & 0xFF;
        }
        if (!vdev->common_base) return 0;
    } else {
        /* Legacy: Get I/O BAR */
        uint32_t bar0 = pci_get_bar(bdf, 0);
        if (!(bar0 & 1)) return 0;
        vdev->legacy_port = bar0 & ~3;
    }

    /* Enable Bus Master, I/O Space and Memory Space in PCI Command register */
    uint8_t bus = (bdf >> 16) & 0xFF;
    uint8_t dev_pci = (bdf >> 8) & 0xFF;
    uint8_t func = bdf & 0xFF;
    uint32_t cmd = pci_config_read(bus, dev_pci, func, 0x04);
    cmd |= 0x7; /* Bus Master (2) | Memory Space (1) | I/O Space (0) */
    pci_config_write(bus, dev_pci, func, 0x04, cmd);

    uintptr_t handle = VIRTIO_HANDLE_FLAG | virtio_dev_count;
    vdev->device_id = v_id;
    vdev->irq = 32 + pci_get_interrupt(bdf);
    virtio_dev_count++;
    return handle;
}

static void virtio_pci_callback(int bdf, uint16_t vendor, uint16_t device_id) {
    if (vendor != 0x1AF4) return;

    uint16_t v_id = 0;
    int is_modern = 0;

    /* VirtIO PCI IDs: 0x1000-0x103F (Legacy), 0x1040-0x107F (Modern) */
    if (device_id >= 0x1040 && device_id <= 0x107F) {
        v_id = device_id - 0x1040;
        is_modern = 1;
    } else if (device_id >= 0x1000 && device_id <= 0x103F) {
        /* Subsystem ID usually contains the VirtIO Device ID for Legacy */
        v_id = pci_config_read((bdf >> 16) & 0xFF, (bdf >> 8) & 0x1F, bdf & 0x7, 0x2C) >> 16;
    }

    if (v_id != 0) {
        register_vdev(bdf, v_id, is_modern);
    }
}

void arch_virtio_scan(void) {
    virtio_dev_count = 0;
    pci_enumerate(virtio_pci_callback);
    pr_info("VirtIO-PCI: Found %d devices via PCI scan\n", virtio_dev_count);
}


uintptr_t arch_virtio_register_pci(uint32_t bdf) {
    uint8_t bus = (bdf >> 16) & 0xFF;
    uint8_t dev = (bdf >> 8) & 0xFF;
    uint8_t func = bdf & 0xFF;
    uint32_t id = pci_config_read(bus, dev, func, 0);
    uint16_t pci_devid = id >> 16;
    uint16_t v_id = 0;
    int is_modern = 0;
    if (pci_devid >= 0x1040 && pci_devid <= 0x107F) { v_id = pci_devid - 0x1040; is_modern = 1; }
    else if (pci_devid >= 0x1000 && pci_devid <= 0x103F) { v_id = pci_config_read(bus, dev, func, 0x2C) >> 16; }
    
    return register_vdev(bdf, v_id, is_modern);
}
void virtio_setup_queue(uintptr_t handle, uint32_t queue_idx, uint64_t desc_addr, uint64_t avail_addr, uint64_t used_addr) {
    if (!(handle & VIRTIO_HANDLE_FLAG)) return;
    int idx = handle & 0xFF;
    struct virtio_pci_dev *dev = &virtio_devices[idx];

    if (dev->is_modern) {
        /* Write to Common Config */
        *(volatile uint16_t *)(dev->common_base + 0x16) = (uint16_t)queue_idx; /* queue_select */
        *(volatile uint32_t *)(dev->common_base + 0x20) = (uint32_t)(desc_addr & 0xFFFFFFFF);
        *(volatile uint32_t *)(dev->common_base + 0x24) = (uint32_t)(desc_addr >> 32);
        *(volatile uint32_t *)(dev->common_base + 0x28) = (uint32_t)(avail_addr & 0xFFFFFFFF);
        *(volatile uint32_t *)(dev->common_base + 0x2C) = (uint32_t)(avail_addr >> 32);
        *(volatile uint32_t *)(dev->common_base + 0x30) = (uint32_t)(used_addr & 0xFFFFFFFF);
        *(volatile uint32_t *)(dev->common_base + 0x34) = (uint32_t)(used_addr >> 32);
        *(volatile uint16_t *)(dev->common_base + 0x1C) = 1; /* queue_enable */
    } else {
        /* Legacy: Write to I/O Ports */
        outw(dev->legacy_port + 0x0E, (uint16_t)queue_idx);
        outl(dev->legacy_port + 0x08, (uint32_t)(desc_addr >> 12));
    }
}

int arch_virtio_get_count(uint32_t device_id) {
    int count = 0;
    for (int i = 0; i < virtio_dev_count; i++) {
        if (virtio_devices[i].device_id == device_id) count++;
    }
    return count;
}

int arch_virtio_get_device(uint32_t device_id, int index, uintptr_t *out_base, uint32_t *out_irq) {
    int current = 0;
    for (int i = 0; i < virtio_dev_count; i++) {
        if (virtio_devices[i].device_id == device_id) {
            if (current == index) {
                if (out_base) *out_base = VIRTIO_HANDLE_FLAG | i;
                if (out_irq) *out_irq = virtio_devices[i].irq;
                return 0;
            }
            current++;
        }
    }
    return -1;
}
