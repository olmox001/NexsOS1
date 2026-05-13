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

uint32_t pci_config_read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
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
