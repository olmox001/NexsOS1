/*
 * kernel/include/kernel/hal_platform.h
 * Unified Hardware Abstraction Layer for Platform Resources
 */
#ifndef _KERNEL_HAL_PLATFORM_H
#define _KERNEL_HAL_PLATFORM_H

#include <kernel/types.h>
#include <kernel/pmm.h>

/* Unified Memory Region info */
struct hal_mem_map {
    struct mem_region *regions;
    size_t count;
};

/* Platform-independent interface */
void hal_platform_init(void);
struct hal_mem_map hal_get_memory_map(void);
uint32_t hal_get_cpu_count(void);

/* IRQ Abstraction */
uint32_t hal_get_timer_irq(void);
uint32_t hal_get_uart_irq(void);

/* Resource discovery */
struct hal_resource {
    uintptr_t base;
    size_t size;
    uint32_t irq;
};

#endif /* _KERNEL_HAL_PLATFORM_H */
