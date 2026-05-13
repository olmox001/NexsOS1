#ifndef _KERNEL_HAL_H
#define _KERNEL_HAL_H

#include <kernel/types.h>
#include <kernel/hal_unified.h>
#include <kernel/hal_device.h>
#include <kernel/hal_platform.h>

/* --- Device & Bus Abstraction --- */

struct hal_device {
    char name[32];
    hal_bus_type_t bus_type;
    uintptr_t base;
    uint32_t irq;
    uint16_t vendor_id;
    uint16_t device_id;
    union {
        uint32_t pci_bdf;
        void *priv;
    };
};

void hal_bus_init(void);
int hal_device_get_count(void);
struct hal_device *hal_device_get(int index);
struct hal_device *hal_device_find(uint16_t vendor, uint16_t device, int index);

/* Internal HAL APIs for architecture-specific code */
void hal_register_device(struct hal_device *dev);
void arch_bus_scan(void);
void arch_irq_init(void);
void arch_timer_init(void);

/* Helper for automatic resource type detection */
static inline hal_res_type_t hal_auto_type(uintptr_t addr) {
#ifdef ARCH_AMD64
    if (addr < 0x10000) return HAL_RES_PORT;
#else
    (void)addr;
#endif
    return HAL_RES_MMIO;
}

static inline hal_bus_type_t hal_auto_bus(uintptr_t addr) {
#ifdef ARCH_AMD64
    if (addr < 0x10000) return HAL_BUS_TYPE_PCI;
#else
    (void)addr;
#endif
    /* Default to platform if not a small port on x86 */
    return HAL_BUS_TYPE_PLATFORM;
}

/* --- Legacy Compatibility Aliases (Optional) --- */
#define hal_read8(addr)  hal_dev_read8(&(hal_device_t){hal_auto_type((uintptr_t)(addr)), hal_auto_bus((uintptr_t)(addr)), (uintptr_t)(addr), 0}, 0)
#define hal_read16(addr) hal_dev_read16(&(hal_device_t){hal_auto_type((uintptr_t)(addr)), hal_auto_bus((uintptr_t)(addr)), (uintptr_t)(addr), 0}, 0)
#define hal_read32(addr) hal_dev_read32(&(hal_device_t){hal_auto_type((uintptr_t)(addr)), hal_auto_bus((uintptr_t)(addr)), (uintptr_t)(addr), 0}, 0)
#define hal_write8(addr, v)  hal_dev_write8(&(hal_device_t){hal_auto_type((uintptr_t)(addr)), hal_auto_bus((uintptr_t)(addr)), (uintptr_t)(addr), 0}, 0, v)
#define hal_write16(addr, v) hal_dev_write16(&(hal_device_t){hal_auto_type((uintptr_t)(addr)), hal_auto_bus((uintptr_t)(addr)), (uintptr_t)(addr), 0}, 0, v)
#define hal_write32(addr, v) hal_dev_write32(&(hal_device_t){hal_auto_type((uintptr_t)(addr)), hal_auto_bus((uintptr_t)(addr)), (uintptr_t)(addr), 0}, 0, v)

#endif /* _KERNEL_HAL_H */
