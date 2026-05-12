#include <kernel/drivers.h>
#include <kernel/hal.h>
#include <kernel/printk.h>

void driver_timer_init(void) {
    arch_timer_init();
    pr_info("%s", "Timer driver initialized via HAL\n");
}
