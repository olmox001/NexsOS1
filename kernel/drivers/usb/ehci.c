/*
 * kernel/drivers/usb/ehci.c
 * EHCI (USB 2.0) host controller driver.
 *
 * STATUS: registration stub. EHCI only drives high-speed devices directly;
 * low/full-speed boot keyboards/mice are routed to companion UHCI/OHCI
 * controllers. Full implementation (async/periodic schedules, qTD/QH) lands
 * after the xHCI path is verified. The probe currently declines so the device
 * stays available for a future driver.
 */

#include <drivers/usb/usb.h>
#include <kernel/driver.h>
#include <kernel/printk.h>

static int ehci_probe(struct hal_device *dev) {
    pr_info("EHCI: controller %s present (driver not yet implemented)\n", dev->name);
    return -1; /* decline for now */
}

static struct device_driver ehci_driver = {
    .name = "ehci",
    .vendor = DRV_ANY_ID,
    .device = DRV_ANY_ID,
    .class_code = 0x0C,
    .subclass = 0x03,
    .prog_if = 0x20, /* EHCI */
    .probe = ehci_probe,
};

void ehci_register(void) {
    driver_register(&ehci_driver);
}
