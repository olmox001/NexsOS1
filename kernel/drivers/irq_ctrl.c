#include <kernel/drivers.h>
#include <kernel/hal.h>
#include <kernel/printk.h>

void driver_irq_init(void) {
    arch_irq_init();
    pr_info("%s", "IRQ driver initialized via HAL\n");
}
