#ifndef _KERNEL_HAL_PLATFORM_H
#define _KERNEL_HAL_PLATFORM_H

#include <kernel/types.h>
#include <kernel/pmm.h>
#include <kernel/hal_device.h>

/* Unified Memory Region info */
struct hal_mem_map {
    struct mem_region *regions;
    size_t count;
};

/* Platform-independent interface */
void hal_platform_init(void);
struct hal_mem_map hal_get_memory_map(void);
uint32_t hal_get_cpu_count(void);

/* Device Discovery */
typedef enum {
    HAL_DEV_VIRTIO_BLOCK,
    HAL_DEV_VIRTIO_NET,
    HAL_DEV_VIRTIO_CONSOLE,
    HAL_DEV_VIRTIO_GPU,
    HAL_DEV_UART,
    HAL_DEV_TIMER,
    HAL_DEV_INT_CTRL
} hal_device_id_t;

/**
 * Find a device by its unified ID.
 * Returns 0 on success, fills 'out_dev'.
 */
int hal_platform_find_device(hal_device_id_t id, hal_device_t *out_dev);

/* IRQ Abstraction */
uint32_t hal_get_timer_irq(void);
uint32_t hal_get_uart_irq(void);

#endif /* _KERNEL_HAL_PLATFORM_H */
