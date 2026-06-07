/*
 * kernel/core/hal_bus.c
 * Hardware Abstraction Layer — Device Registry
 *
 * This file is the *thin* device registry half of the HAL: it maintains a
 * fixed-size array of hal_device descriptors that arch-specific bus-scan code
 * populates at boot.  The upper half of the HAL (register-accessor macros and
 * their heuristic dispatch) lives in kernel/include/kernel/hal.h and is
 * deliberately kept separate.
 *
 * Role / layering:
 *   boot -> arch_bus_scan() (pci.c / platform MMIO) -> hal_register_device()
 *        -> arch_virtio_scan() (virtio.c, queries registry)           -> drivers
 *   Generic kernel code calls hal_device_find() / hal_device_get() to look up
 *   devices by vendor:device pair; it never accesses hal_devices[] directly.
 *
 * Key invariants:
 *   - hal_devices[] is a flat array; devices are never removed after registration.
 *   - hal_device_count is only modified during boot, before SMP secondaries start;
 *     no lock is taken here.  Do not call hal_register_device() after SMP init.
 *   - MAX_HAL_DEVICES (64) is a hard limit; registration beyond it is silently
 *     dropped with a pr_warn (see hal_register_device).
 *
 * Known issues:
 *   HAL-02  (W2 BAD-IMPL) The broader HAL is spread over four headers
 *           (hal.h, hal_unified.h, hal_device.h, hal_platform.h); this file
 *           is the one piece the review explicitly calls "genuinely thin and
 *           fine."  The defect is in the header layer, not here.
 *   HAL-01  (W3 WRONG-DESIGN/PERF) hal_read32() in hal.h synthesises a
 *           compound-literal hal_device_t and runs address-heuristic dispatch
 *           on every MMIO access; not a defect in this file.
 */
#include <kernel/hal.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <drivers/virtio.h>

#define MAX_HAL_DEVICES 64

/* hal_devices[]: flat boot-time registry of discovered hardware devices.
 * Populated by arch_bus_scan() (PCI) and arch_virtio_scan(); immutable after
 * SMP init.  Access without a lock is safe only during the single-threaded
 * boot phase. */
static struct hal_device hal_devices[MAX_HAL_DEVICES];
/* hal_device_count: number of valid entries in hal_devices[].
 * Monotonically increasing; never decremented. */
static int hal_device_count = 0;

/*
 * hal_bus_init - initialise the device registry and trigger bus discovery.
 *
 * Called once from the boot path (CPU 0, single-threaded) before any driver
 * or SMP secondary starts.  Resets hal_device_count, then calls
 * arch_bus_scan() to enumerate PCI/platform devices (each device calls
 * hal_register_device()), followed by arch_virtio_scan() to probe VirtIO
 * devices found on the bus.
 *
 * Locking: none — must be called before SMP is active.
 * IRQ context: no — called from boot path with IRQs disabled.
 */
void hal_bus_init(void) {
    hal_device_count = 0;
    pr_info("%s", "HAL: Initializing Bus Manager...\n");
    
    /* Architecture-specific bus discovery (PCI, Platform MMIO, etc) */
    arch_bus_scan();

    /* Discover VirtIO devices based on found bus devices */
    arch_virtio_scan();
}

/*
 * hal_device_get_count - return the number of registered devices.
 *
 * Returns hal_device_count.  Safe to call after boot; no lock needed because
 * hal_device_count is not modified after SMP init.
 */
int hal_device_get_count(void) {
    return hal_device_count;
}

/*
 * hal_device_get - return a pointer to the device at position 'index'.
 *
 * Returns NULL if index is out of range [0, hal_device_count).
 * The returned pointer is into the static hal_devices[] array; it is stable
 * for the lifetime of the kernel.
 */
struct hal_device *hal_device_get(int index) {
    if (index < 0 || index >= hal_device_count) return NULL;
    return &hal_devices[index];
}

/*
 * hal_device_find - find the Nth device matching a vendor:device pair.
 *
 * Scans hal_devices[] linearly and returns a pointer to the 'index'th
 * (zero-based) entry whose vendor_id==vendor and device_id==device.
 * Returns NULL if fewer than (index+1) matching devices exist.
 *
 * Used by driver probes (e.g. virtio_blk) to locate their PCI/MMIO device
 * descriptor without knowing the slot number.
 */
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

/*
 * hal_register_device - add a device descriptor to the global registry.
 *
 * Called by arch_bus_scan() / arch_virtio_scan() for each discovered device.
 * Copies *dev into hal_devices[hal_device_count] and increments the count.
 * If the registry is full (>= MAX_HAL_DEVICES), emits pr_warn and returns
 * without registering the device — the caller receives no error indication.
 *
 * Locking: none — must be called during single-threaded boot only.
 * IRQ context: no.
 *
 * Side effects: updates hal_device_count; logs device details via pr_info.
 */
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
