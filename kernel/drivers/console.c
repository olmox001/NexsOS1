#include <kernel/drivers.h>
#include <kernel/hal.h>
#include <kernel/printk.h>

void driver_console_init(void) {
    /* Console UART is initialized very early, but we can wrap it if needed.
     * For now, we assume arch_platform_early_init or similar handled it,
     * or we add arch_console_init() to HAL.
     */
    extern void uart_init(void);
    uart_init();
    pr_info("%s", "Console driver initialized\n");
}
