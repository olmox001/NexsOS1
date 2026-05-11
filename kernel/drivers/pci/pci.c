/*
 * kernel/drivers/pci/pci.c
 * Minimal PCI Bus Driver
 */
#include <kernel/types.h>
#include <kernel/printk.h>
#include <arch/arch.h>
#include <drivers/pci.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

uint32_t pci_config_read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((uint32_t)bus << 16) | 
                       (uint32_t)((uint32_t)device << 11) |
                       (uint32_t)((uint32_t)func << 8) | 
                       (offset & 0xFC) | 
                       ((uint32_t)0x80000000);
    outl(PCI_CONFIG_ADDR, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((uint32_t)bus << 16) | 
                       (uint32_t)((uint32_t)device << 11) |
                       (uint32_t)((uint32_t)func << 8) | 
                       (offset & 0xFC) | 
                       ((uint32_t)0x80000000);
    outl(PCI_CONFIG_ADDR, address);
    outl(PCI_CONFIG_DATA, value);
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
