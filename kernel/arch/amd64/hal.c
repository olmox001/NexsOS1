/*
 * kernel/arch/amd64/hal.c
 * AMD64 Hardware Abstraction Layer — PCI Bus Scan and Timer Init
 *
 * Responsibilities:
 *   - amd64_pci_callback: called by pci_enumerate for each discovered PCI
 *     function; enables Bus Master + I/O + Memory access, reads BAR0 and BAR4,
 *     translates VirtIO device IDs, assigns the IRQ, and registers the device
 *     with the HAL device table via hal_register_device.
 *   - arch_bus_scan: invokes pci_enumerate(amd64_pci_callback) to build the
 *     HAL device list.
 *   - arch_irq_init: initialises the legacy 8259 PIC.
 *   - arch_timer_init: runs LAPIC calibration once on the BSP.
 *   - timer_init_percpu: starts the per-CPU LAPIC periodic timer at HZ.
 *
 * Known issues:
 *   DRV-VIRTIO-01 (W5 BUG) pci_get_bar (pci.c:106) returns uint32_t.  For
 *     64-bit BAR entries (type bits [2:1] = 0b10), the high 32 bits of the
 *     MMIO base address are in the NEXT BAR register and are not read.  When
 *     QEMU places the virtio-blk BAR above 4 GB ('-m 4G'), the low 32 bits
 *     returned by pci_get_bar(bdf, 4) are 0 or an alias, so dev->base is
 *     wrong.  The virtio driver reads QUEUE_NUM_MAX from the wrong address and
 *     gets 0 → "Invalid queue size (0)!" → downstream divide-by-zero crash.
 *     Cross-ref: DRV-PCI-02 (pci.c:106-111).
 *     Fix: in amd64_pci_callback, after reading b4, check bits [2:1]; if 10b,
 *     read BAR5 and compose a uint64_t base address.
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
        /* FIX(DRV-VIRTIO-01): read the FULL 64-bit BAR. For a 64-bit memory BAR
         * (type bits [2:1] == 0b10) the high 32 bits live in BAR5; reading only
         * BAR4 truncates the base when QEMU places it above 4GB ('-m 4G'),
         * yielding a wrong dev.base -> virtio reads QUEUE_NUM_MAX as 0 -> crash. */
        dev.base = (uintptr_t)(b4 & ~0xFU);
        if ((b4 & 0x6) == 0x4) { /* 64-bit memory BAR */
            uint32_t b5 = pci_get_bar(bdf, 5);
            dev.base |= ((uintptr_t)b5 << 32);
        }
        /* FIX(AMMU-07): map the BAR MMIO so it is reachable even above 4GB
         * (the fixed 0xFE000000-0xFFFFFFFF identity window does not cover it). */
        extern int arch_vmm_map_device(uint64_t base, uint64_t size);
        arch_vmm_map_device(dev.base, 0x10000);
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
