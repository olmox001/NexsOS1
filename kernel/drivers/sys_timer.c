#include <kernel/drivers.h>
#include <kernel/printk.h>

#ifdef ARCH_AARCH64
extern void timer_init(void);
#elif defined(ARCH_AMD64)
extern void pit_init(void);
#endif

void driver_timer_init(void) {
#ifdef ARCH_AARCH64
    timer_init();
#elif defined(ARCH_AMD64)
    pit_init();
#endif
    pr_info("%s", "Timer driver initialized\n");
}
