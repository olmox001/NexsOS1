#include <kernel/drivers.h>
#include <kernel/printk.h>

#ifdef ARCH_AARCH64
extern void timer_init(void);
#elif defined(ARCH_AMD64)
extern void pit_init_hz(uint32_t hz);
#endif

void driver_timer_init(void) {
#ifdef ARCH_AARCH64
    timer_init();
#elif defined(ARCH_AMD64)
    pit_init_hz(100);
#endif
    pr_info("%s", "Timer driver initialized\n");
}
