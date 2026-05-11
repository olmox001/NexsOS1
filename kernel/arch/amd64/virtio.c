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

static uintptr_t register_vdev(uint32_t bdf, uint16_t v_id, int is_modern) {
    (void)v_id;
    if (virtio_dev_count >= MAX_VIRTIO_DEVS) return 0;
    struct virtio_pci_dev *vdev = &virtio_devices[virtio_dev_count];
    vdev->is_modern = is_modern;

    if (is_modern) {
        uint8_t dev_pci = (bdf >> 8) & 0xFF;
        /* Modern: Scan capabilities for Common and Notify configs */
        for (uint8_t ptr = 0x40; ptr <= 0xFC; ptr++) {
            uint32_t val = pci_config_read(0, dev_pci, 0, ptr);
            if ((val & 0xFF) == 0x09) { /* Vendor Specific */
                uint8_t type = (val >> 24) & 0xFF;
                uint8_t bar = pci_config_read(0, dev_pci, 0, ptr + 4) & 0xFF;
                uint32_t off = pci_config_read(0, dev_pci, 0, ptr + 8);
                uintptr_t bar_phys = pci_get_bar(bdf, bar) & ~0xF;

                /* Map BAR for MMIO */
                extern uint64_t *kernel_pgd;
                for(int p=0; p<4; p++) vmm_map_page(kernel_pgd, bar_phys + p*4096, bar_phys + p*4096, PAGE_DEVICE);

                if (type == 1) vdev->common_base = bar_phys + off;
                if (type == 2) {
                    vdev->notify_base = bar_phys + off;
                    vdev->notify_off_multiplier = pci_config_read(0, dev_pci, 0, ptr + 12);
                }
            }
        }
        if (!vdev->common_base) return 0;
    } else {
        /* Legacy: Get I/O BAR */
        uint32_t bar0 = pci_get_bar(bdf, 0);
        if (!(bar0 & 1)) return 0;
        vdev->legacy_port = bar0 & ~3;
    }

    uintptr_t handle = VIRTIO_HANDLE_FLAG | virtio_dev_count;
    virtio_dev_count++;
    return handle;
}

uintptr_t arch_virtio_register_pci(uint32_t bdf) {
    uint32_t id = pci_config_read(0, (bdf >> 8) & 0x1F, 0, 0);
    if (id == 0xFFFFFFFF) return 0;

    uint16_t pci_devid = id >> 16;
    uint16_t v_id = 0;
    int is_modern = 0;

    if (pci_devid >= 0x1040 && pci_devid <= 0x107F) {
        v_id = pci_devid - 0x1040;
        is_modern = 1;
    } else if (pci_devid >= 0x1000 && pci_devid <= 0x103F) {
        v_id = pci_config_read(0, (bdf >> 8) & 0x1F, 0, 0x2C) >> 16;
    }

    if (v_id == 0) return 0;
    return register_vdev(bdf, v_id, is_modern);
}

int arch_virtio_probe(uint32_t device_id, uintptr_t *out_handle, uint32_t *out_irq) {
    for (int dev = 0; dev < 32; dev++) {
        uint32_t bdf = (0 << 16) | (dev << 8) | 0;
        uint32_t id = pci_config_read(0, dev, 0, 0);
        if (id == 0xFFFFFFFF) continue;

        uint16_t pci_devid = id >> 16;
        uint16_t v_id = 0;
        int is_modern = 0;

        if (pci_devid >= 0x1040 && pci_devid <= 0x107F) {
            v_id = pci_devid - 0x1040;
            is_modern = 1;
        } else if (pci_devid >= 0x1000 && pci_devid <= 0x103F) {
            v_id = pci_config_read(0, dev, 0, 0x2C) >> 16;
        }

        if (v_id == device_id) {
            uintptr_t handle = register_vdev(bdf, v_id, is_modern);
            if (!handle) return -1;

            if (out_handle) *out_handle = handle;
            if (out_irq) *out_irq = 32 + pci_get_interrupt(bdf);
            return 0;
        }
    }
    return -1;
}
