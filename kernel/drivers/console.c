#include <kernel/drivers.h>
#include <kernel/printk.h>

#ifdef ARCH_AARCH64
extern void uart_init(void);
#elif defined(ARCH_AMD64)
extern void uart_init(void);
#endif

void driver_console_init(void) {
#ifdef ARCH_AARCH64
    uart_init();
#elif defined(ARCH_AMD64)
    uart_init();
#endif
    pr_info("%s", "Console driver initialized\n");
}
