/*
 * kernel/include/kernel/hal.h
 * Hardware Abstraction Layer - Unified Device Access
 */
#ifndef _KERNEL_HAL_H
#define _KERNEL_HAL_H

#include <kernel/types.h>
#include <kernel/arch.h>

/* --- Device & Bus Abstraction --- */

typedef enum {
    HAL_BUS_TYPE_PLATFORM, /* FDT / Fixed MMIO */
    HAL_BUS_TYPE_PCI,
    HAL_BUS_TYPE_VIRTIO_MMIO
} hal_bus_type_t;

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

/* --- Device Register Access (8, 16, 32 bit) --- */

/**
 * hal_read8/16/32: Read from a device register.
 * On x86, this may use Port I/O or MMIO depending on the address range.
 * On AArch64, this always uses MMIO.
 */
static inline uint8_t hal_read8(uintptr_t addr) {
#ifdef ARCH_AMD64
    if (addr < 0x10000) return inb((uint16_t)addr);
#endif
    return *(volatile uint8_t *)addr;
}

static inline uint16_t hal_read16(uintptr_t addr) {
#ifdef ARCH_AMD64
    if (addr < 0x10000) return inw((uint16_t)addr);
#endif
    return *(volatile uint16_t *)addr;
}

static inline uint32_t hal_read32(uintptr_t addr) {
#ifdef ARCH_AMD64
    if (addr < 0x10000) return inl((uint16_t)addr);
#endif
    return *(volatile uint32_t *)addr;
}

static inline void hal_write8(uintptr_t addr, uint8_t val) {
#ifdef ARCH_AMD64
    if (addr < 0x10000) {
        outb((uint16_t)addr, val);
        return;
    }
#endif
    *(volatile uint8_t *)addr = val;
}

static inline void hal_write16(uintptr_t addr, uint16_t val) {
#ifdef ARCH_AMD64
    if (addr < 0x10000) {
        outw((uint16_t)addr, val);
        return;
    }
#endif
    *(volatile uint16_t *)addr = val;
}

static inline void hal_write32(uintptr_t addr, uint32_t val) {
#ifdef ARCH_AMD64
    if (addr < 0x10000) {
        outl((uint16_t)addr, val);
        return;
    }
#endif
    *(volatile uint32_t *)addr = val;
}

/* --- Interrupt Management (Aliases for arch_local_irq_*) --- */

#define hal_irq_enable()           arch_local_irq_enable()
#define hal_irq_disable()          arch_local_irq_disable()
#define hal_irq_save(flags)        arch_local_irq_save(flags)
#define hal_irq_restore(flags)     arch_local_irq_restore(flags)
#define hal_irq_save_val()         arch_local_irq_save_val()

/* --- CPU Control (Aliases for arch_*) --- */

#define hal_cpu_halt()             arch_cpu_halt()
#define hal_cpu_yield()            arch_yield()
#define hal_cpu_nop()              arch_nop()
#define hal_cpu_id()               arch_get_cpu_id()

#endif /* _KERNEL_HAL_H */
