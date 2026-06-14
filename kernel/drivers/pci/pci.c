/*
 * kernel/drivers/pci/pci.c
 * Minimal PCI Bus Driver
 */
#include <kernel/types.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/hal.h>
#include <drivers/pci.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static DEFINE_SPINLOCK(pci_lock);

/* Arch config backend. NULL = built-in CF8/CFC port I/O (amd64). aarch64 sets
 * ECAM MMIO ops via pci_set_config_ops(). */
static struct pci_config_ops *cfg_ops = NULL;

void pci_set_config_ops(struct pci_config_ops *ops) { cfg_ops = ops; }

uint32_t pci_config_read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    if (cfg_ops) return cfg_ops->read32(bus, device, func, offset);

    uint32_t address = (uint32_t)((uint32_t)bus << 16) |
                       (uint32_t)((uint32_t)device << 11) |
                       (uint32_t)((uint32_t)func << 8) |
                       (offset & 0xFC) |
                       ((uint32_t)0x80000000);

    uint64_t flags;
    spin_lock_irqsave(&pci_lock, &flags);
    hal_write32(PCI_CONFIG_ADDR, address);
    uint32_t val = hal_read32(PCI_CONFIG_DATA);
    spin_unlock_irqrestore(&pci_lock, flags);
    return val;
}

void pci_config_write(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value) {
    if (cfg_ops) { cfg_ops->write32(bus, device, func, offset, value); return; }

    uint32_t address = (uint32_t)((uint32_t)bus << 16) |
                       (uint32_t)((uint32_t)device << 11) |
                       (uint32_t)((uint32_t)func << 8) |
                       (offset & 0xFC) |
                       ((uint32_t)0x80000000);

    uint64_t flags;
    spin_lock_irqsave(&pci_lock, &flags);
    hal_write32(PCI_CONFIG_ADDR, address);
    hal_write32(PCI_CONFIG_DATA, value);
    spin_unlock_irqrestore(&pci_lock, flags);
}

/* 
 * Find a device by Vendor and Device ID
 * Returns bus:device:func in a single 32-bit int (or -1 if not found)
 */
int pci_find_device(uint16_t vendor, uint16_t device_id) {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t reg0 = pci_config_read(bus, dev, 0, 0);
            uint16_t v = reg0 & 0xFFFF;
            uint16_t d = reg0 >> 16;
            
            if (v == vendor && d == device_id) {
                return (bus << 16) | (dev << 8) | 0;
            }
            
            /* Check if multi-function */
            uint32_t header_type = pci_config_read(bus, dev, 0, 0x0C);
            if (header_type & 0x80) {
                for (int func = 1; func < 8; func++) {
                    reg0 = pci_config_read(bus, dev, func, 0);
                    if ((reg0 & 0xFFFF) == vendor && (reg0 >> 16) == device_id) {
                        return (bus << 16) | (dev << 8) | func;
                    }
                }
            }
        }
    }
    return -1;
}

/* 
 * Enumerate all PCI devices and call callback
 */
void pci_enumerate(void (*callback)(int bdf, uint16_t vendor, uint16_t device_id)) {
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t reg0 = pci_config_read(bus, dev, 0, 0);
            uint16_t v = reg0 & 0xFFFF;
            uint16_t d = reg0 >> 16;
            
            if (v == 0xFFFF) continue;
            
            callback((bus << 16) | (dev << 8) | 0, v, d);
            
            /* Check if multi-function */
            uint32_t header_type = pci_config_read(bus, dev, 0, 0x0C);
            if (header_type & 0x80) {
                for (int func = 1; func < 8; func++) {
                    reg0 = pci_config_read(bus, dev, func, 0);
                    v = reg0 & 0xFFFF;
                    d = reg0 >> 16;
                    if (v != 0xFFFF) {
                        callback((bus << 16) | (dev << 8) | func, v, d);
                    }
                }
            }
        }
    }
}

/* Get BAR address (simplistic) */
uint32_t pci_get_bar(int bdf, int bar_index) {
    uint8_t bus = (bdf >> 16) & 0xFF;
    uint8_t dev = (bdf >> 8) & 0xFF;
    uint8_t func = bdf & 0xFF;
    return pci_config_read(bus, dev, func, 0x10 + (bar_index * 4));
}

/* Get Interrupt Line */
uint8_t pci_get_interrupt(int bdf) {
    uint8_t bus = (bdf >> 16) & 0xFF;
    uint8_t dev = (bdf >> 8) & 0xFF;
    uint8_t func = bdf & 0xFF;
    return pci_config_read(bus, dev, func, 0x3C) & 0xFF;
}

/* Class triplet at config offset 0x08:
 *   byte0 = revision, byte1 = prog_if, byte2 = subclass, byte3 = class_code.
 * Returns the raw 32-bit word; callers shift out the field they want. */
uint32_t pci_get_class(int bdf) {
    uint8_t bus = (bdf >> 16) & 0xFF;
    uint8_t dev = (bdf >> 8) & 0xFF;
    uint8_t func = bdf & 0xFF;
    return pci_config_read(bus, dev, func, 0x08);
}

/* Header type at config offset 0x0C, byte2. Bit7 = multi-function. */
uint8_t pci_get_header_type(int bdf) {
    uint8_t bus = (bdf >> 16) & 0xFF;
    uint8_t dev = (bdf >> 8) & 0xFF;
    uint8_t func = bdf & 0xFF;
    return (pci_config_read(bus, dev, func, 0x0C) >> 16) & 0xFF;
}
/* Get BAR size */
uint32_t pci_get_bar_size(int bdf, int bar_index) {
    uint8_t bus = (bdf >> 16) & 0xFF;
    uint8_t dev = (bdf >> 8) & 0xFF;
    uint8_t func = bdf & 0xFF;
    uint8_t offset = 0x10 + (bar_index * 4);

    uint32_t original = pci_config_read(bus, dev, func, offset);
    pci_config_write(bus, dev, func, offset, 0xFFFFFFFF);
    uint32_t size = pci_config_read(bus, dev, func, offset);
    pci_config_write(bus, dev, func, offset, original);

    if (size == 0 || size == 0xFFFFFFFF) return 0;
    
    /* Clear flags */
    if (size & 1) { /* I/O */
        size &= 0xFFFFFFFC;
    } else { /* Memory */
        size &= 0xFFFFFFF0;
    }

    return ~size + 1;
}

/*
 * pci_scan_and_register - arch-neutral PCI enumeration into the HAL registry.
 *
 * Defines the long-declared DRV-PCI-01 (#51) function. Walks config space via
 * the active backend (CF8/CFC on amd64, ECAM on aarch64), and for each present
 * function registers a hal_device carrying its vendor/device, class triplet and
 * first BAR (64-bit aware), enabling bus-master + MMIO/IO decode. The bound
 * driver (xHCI/...) maps its own BAR window. IRQ is left 0: the providers that
 * use this path (USB, behind this generic scan) are polled, and PCI INTx->GIC
 * routing is platform-specific (derive from FDT when an IRQ-driven PCI device
 * needs it). amd64 keeps its own richer callback (virtio legacy/modern), so
 * this is currently the aarch64 ECAM entry point.
 */
extern int arch_vmm_map_device(uint64_t base, uint64_t size);

/* Platform MMIO windows for BAR assignment. Set by the arch (aarch64 from its
 * DTB ranges) before pci_scan_and_register(). When zero (amd64), firmware has
 * already programmed the BARs and we never assign. */
static uint64_t mmio32_next, mmio32_end, mmio64_next, mmio64_end;

void pci_set_mmio_windows(uint64_t m32_base, uint64_t m32_size,
                          uint64_t m64_base, uint64_t m64_size) {
    mmio32_next = m32_base; mmio32_end = m32_base + m32_size;
    mmio64_next = m64_base; mmio64_end = m64_base + m64_size;
}

/* Size-aligned bump allocation from the platform MMIO window. */
static uint64_t pci_alloc_mmio(uint64_t size, int is64) {
    if (size < 0x1000) size = 0x1000;
    if (is64 && mmio64_next) {
        uint64_t a = (mmio64_next + size - 1) & ~(size - 1);
        if (a + size <= mmio64_end) { mmio64_next = a + size; return a; }
    }
    if (mmio32_next) {
        uint64_t a = (mmio32_next + size - 1) & ~(size - 1);
        if (a + size <= mmio32_end) { mmio32_next = a + size; return a; }
    }
    return 0;
}

static void pci_register_one(int bdf, uint16_t vendor, uint16_t device) {
    struct hal_device dev;
    memset(&dev, 0, sizeof(dev));
    dev.bus_type = HAL_BUS_TYPE_PCI;
    dev.vendor_id = vendor;
    dev.device_id = device;
    dev.pci_bdf = (uint32_t)bdf;

    uint32_t cls = pci_get_class(bdf);
    dev.class_code = (cls >> 24) & 0xFF;
    dev.subclass = (cls >> 16) & 0xFF;
    dev.prog_if = (cls >> 8) & 0xFF;
    dev.header_type = pci_get_header_type(bdf);

    uint8_t bus = (bdf >> 16) & 0xFF, d = (bdf >> 8) & 0xFF, f = bdf & 0xFF;

    /* First BAR; follow a 64-bit memory BAR into its high half. */
    uint32_t b0 = pci_get_bar(bdf, 0);
    int is_io = (b0 & 1) != 0;
    int is64 = (!is_io) && ((b0 & 0x6) == 0x4);
    uint64_t base;
    if (is_io) {
        base = (uint64_t)(b0 & ~0x3u);
    } else {
        base = (uint64_t)(b0 & ~0xFu);
        if (is64) base |= ((uint64_t)pci_get_bar(bdf, 1)) << 32;
    }

    /* Bare-metal aarch64 virt has no firmware PCI resource assignment, so MMIO
     * BARs arrive unprogrammed (base 0). Assign one from the platform window. */
    if (!is_io && base == 0 && (mmio32_next || mmio64_next)) {
        uint32_t size = pci_get_bar_size(bdf, 0);
        if (size) {
            uint64_t a = pci_alloc_mmio(size, is64);
            if (a) {
                pci_config_write(bus, d, f, 0x10, (uint32_t)a);
                if (is64) pci_config_write(bus, d, f, 0x14, (uint32_t)(a >> 32));
                base = a;
            }
        }
    }

    /* Enable IO | MEM | BusMaster after BAR assignment. */
    uint32_t cmd = pci_config_read(bus, d, f, 0x04);
    pci_config_write(bus, d, f, 0x04, cmd | 0x7);

    dev.base = base;
    dev.irq = 0; /* polled providers; INTx routing is platform-specific */

    if (vendor == 0x1AF4)
        snprintf(dev.name, sizeof(dev.name), "VirtIO-%d", device);
    else
        snprintf(dev.name, sizeof(dev.name), "PCI-%04x:%04x", vendor, device);

    if (!is_io && base)
        arch_vmm_map_device(base, 0x20000);

    hal_register_device(&dev);
}

void pci_scan_and_register(void) {
    pr_info("%s", "PCI: scanning config space (ECAM/port)...\n");
    for (int bus = 0; bus < 256; bus++) {
        for (int d = 0; d < 32; d++) {
            uint32_t r0 = pci_config_read(bus, d, 0, 0);
            uint16_t v = r0 & 0xFFFF;
            if (v == 0xFFFF) continue;
            pci_register_one((bus << 16) | (d << 8), v, r0 >> 16);

            /* Multi-function devices: header type bit7 (config 0x0C bit23). */
            if (pci_config_read(bus, d, 0, 0x0C) & 0x800000) {
                for (int fn = 1; fn < 8; fn++) {
                    r0 = pci_config_read(bus, d, fn, 0);
                    v = r0 & 0xFFFF;
                    if (v == 0xFFFF) continue;
                    pci_register_one((bus << 16) | (d << 8) | fn, v, r0 >> 16);
                }
            }
        }
    }
}
