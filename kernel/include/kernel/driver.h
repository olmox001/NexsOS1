#ifndef _KERNEL_DRIVER_H
#define _KERNEL_DRIVER_H

/*
 * kernel/include/kernel/driver.h
 * Driver-binding contract (ASTRA wiring layer).
 *
 * A device_driver declares what it can drive — either a specific vendor:device
 * pair (virtio, Bochs VBE, ...) or a generic PCI class triplet
 * (xHCI = 0x0C/0x03/0x30, AHCI = 0x01/0x06/0x01, VGA = 0x03/0x00/..., ...).
 * driver_match_all() walks the hal_device registry and calls probe() on every
 * still-unbound device a driver matches.  This is the seam that lets a "board"
 * be just a bag of class-identified providers, with no hardcoded model list.
 */

#include <kernel/types.h>
#include <kernel/hal.h>

#define DRV_ANY_ID    0xFFFF  /* wildcard for vendor/device */
#define DRV_ANY_CLASS 0xFF    /* wildcard for class_code/subclass/prog_if */

struct device_driver {
    const char *name;

    /* ID match (either-or with class match): set to DRV_ANY_ID to ignore. */
    uint16_t vendor;
    uint16_t device;

    /* Class match: set any field to DRV_ANY_CLASS to ignore that field. */
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;

    /* Called once per matching, still-unbound device. Return 0 to claim the
     * device (it becomes bound), non-zero to decline (stays available). */
    int (*probe)(struct hal_device *dev);

    struct device_driver *next; /* internal: linked-list link */
};

/* Register a driver. Safe to call before or after the bus scan; matching is
 * performed by driver_match_all(), not at registration time. */
void driver_register(struct device_driver *drv);

/* Probe every registered driver against every unbound device in the hal
 * registry. Idempotent: already-bound devices are skipped, so it may be called
 * again after registering more drivers (e.g. once per subsystem init). */
void driver_match_all(void);

#endif /* _KERNEL_DRIVER_H */
