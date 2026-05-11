#include <kernel/drivers.h>
#include <kernel/printk.h>

#ifdef ARCH_AARCH64
extern void gic_register(void);
#elif defined(ARCH_AMD64)
extern void pic_init(void);
#endif

void driver_irq_init(void) {
#ifdef ARCH_AARCH64
    gic_register();
#elif defined(ARCH_AMD64)
    pic_init();
#endif
    pr_info("%s", "IRQ driver initialized\n");
}
