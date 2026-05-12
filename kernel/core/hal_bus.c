/*
 * kernel/core/hal_bus.c
 * Unified Bus & Device Manager
 */
#include <kernel/hal.h>
#include <kernel/string.h>
#include <kernel/printk.h>

#define MAX_HAL_DEVICES 64

static struct hal_device hal_devices[MAX_HAL_DEVICES];
static int hal_device_count = 0;

void hal_bus_init(void) {
    hal_device_count = 0;
    pr_info("%s", "HAL: Initializing Bus Manager...\n");
    
    /* Architecture-specific bus discovery */
    arch_bus_scan();
}

int hal_device_get_count(void) {
    return hal_device_count;
}

struct hal_device *hal_device_get(int index) {
    if (index < 0 || index >= hal_device_count) return NULL;
    return &hal_devices[index];
}

struct hal_device *hal_device_find(uint16_t vendor, uint16_t device, int index) {
    int found = 0;
    for (int i = 0; i < hal_device_count; i++) {
        if (hal_devices[i].vendor_id == vendor && hal_devices[i].device_id == device) {
            if (found == index) return &hal_devices[i];
            found++;
        }
    }
    return NULL;
}

/* API for architecture-specific code to register devices found during scan */
void hal_register_device(struct hal_device *dev) {
    if (hal_device_count >= MAX_HAL_DEVICES) {
        pr_warn("HAL: Maximum device count reached, skipping %s\n", dev->name);
        return;
    }
    
    hal_devices[hal_device_count] = *dev;
    pr_info("HAL: Registered device '%s' (Bus=%d, ID=%04x:%04x, Base=0x%lx, IRQ=%u)\n",
            dev->name, dev->bus_type, dev->vendor_id, dev->device_id, dev->base, dev->irq);
    hal_device_count++;
}
