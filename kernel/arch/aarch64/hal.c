/*
 * kernel/arch/aarch64/hal.c
 * AArch64 Architecture Specific HAL Implementation
 */
#include <kernel/hal.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <drivers/virtio.h>

#define VIRTIO_MMIO_BASE 0x0a000000
#define VIRTIO_MMIO_STRIDE 0x200
#define VIRTIO_COUNT 32

void arch_bus_scan(void) {
    pr_info("%s", "HAL: Scanning Platform Bus (MMIO)...\n");
    
    for (int i = 0; i < VIRTIO_COUNT; i++) {
        uintptr_t base = VIRTIO_MMIO_BASE + i * VIRTIO_MMIO_STRIDE;
        uint32_t magic = hal_read32(base + VIRTIO_MMIO_MAGIC_VALUE);
        
        if (magic == 0x74726976) {
            uint32_t dev_id = hal_read32(base + VIRTIO_MMIO_DEVICE_ID);
            if (dev_id != 0) {
                struct hal_device dev;
                memset(&dev, 0, sizeof(dev));
                
                dev.bus_type = HAL_BUS_TYPE_VIRTIO_MMIO;
                dev.vendor_id = 0x1AF4;
                dev.device_id = (uint16_t)dev_id;
                dev.base = base;
                dev.irq = 48 + i;
                
                snprintf(dev.name, sizeof(dev.name), "VirtIO-%d", dev_id);
                
                hal_register_device(&dev);
            }
        }
    }
}

void arch_irq_init(void) {
    extern void gic_register(void);
    gic_register();
}

void arch_timer_init(void) {
    extern void timer_init(void);
    timer_init();
}
