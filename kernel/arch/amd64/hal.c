/*
 * kernel/arch/amd64/hal.c
 * AMD64 Architecture Specific HAL Implementation
 */
#include <kernel/hal.h>
#include <drivers/pci.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <drivers/timer.h>
#include <arch/amd64/apic.h>

static void amd64_pci_callback(int bdf, uint16_t vendor, uint16_t device_id) {
    struct hal_device dev;
    memset(&dev, 0, sizeof(dev));
    
    dev.bus_type = HAL_BUS_TYPE_PCI;
    dev.vendor_id = vendor;
    dev.device_id = device_id;
    dev.pci_bdf = (uint32_t)bdf;

    /* Enable PCI Bus Master and IO/Mem space */
    uint32_t cmd = pci_config_read((bdf >> 16) & 0xFF, (bdf >> 8) & 0xFF, bdf & 0x7, 0x04);
    pci_config_write((bdf >> 16) & 0xFF, (bdf >> 8) & 0xFF, bdf & 0x7, 0x04, cmd | 0x7);

    /* Get BAR address. Modern devices use BAR4 for MMIO usually. Legacy uses BAR0 for Port I/O. */
    uint32_t b0 = pci_get_bar(bdf, 0);
    uint32_t b4 = pci_get_bar(bdf, 4);

    bool is_modern = (vendor == 0x1AF4 && device_id >= 0x1041);
    
    if (is_modern && b4 != 0 && !(b4 & 1)) {
        dev.base = b4 & ~0xF;
    } else {
        dev.base = b0;
        if (dev.base & 1) dev.base &= ~3;
        else dev.base &= ~0xF;
    }
    
    /* Handle VirtIO ID translation */
    if (vendor == 0x1AF4) {
        if (is_modern) {
            dev.device_id = device_id - 0x1040;
        } else {
            /* Legacy device: Use Subsystem Device ID for type */
            uint32_t sub_id = pci_config_read((bdf >> 16) & 0xFF, (bdf >> 8) & 0xFF, bdf & 0x7, 0x2C);
            dev.device_id = sub_id >> 16;
        }
    }
    
    dev.irq = 32 + pci_get_interrupt(bdf);
    
    /* Basic naming */
    if (vendor == 0x1AF4) {
        snprintf(dev.name, sizeof(dev.name), "VirtIO-%d", device_id);
    } else {
        snprintf(dev.name, sizeof(dev.name), "PCI-%04x:%04x", vendor, device_id);
    }
    
    hal_register_device(&dev);
}

void arch_bus_scan(void) {
    pr_info("%s", "HAL: Scanning PCI Bus...\n");
    pci_enumerate(amd64_pci_callback);
}

void arch_irq_init(void) {
    extern void pic_init(void);
    pic_init();
}

void arch_timer_init(void) {
    /* BSP Global Timer initialization.
     * We perform LAPIC calibration here once using the legacy PIT. */
    pr_info("HAL: Initializing global timer state (LAPIC calibration)\n");
    lapic_timer_calibrate();
}

void timer_init_percpu(void) {
    /* Initialize local APIC timer for the current CPU at configured HZ.
     * This is called by every CPU during its local initialization. */
    lapic_timer_setup(HZ);
}
