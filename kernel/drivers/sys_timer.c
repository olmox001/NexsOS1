/*
 * kernel/drivers/sys_timer.c
 * System Timer Driver Glue
 *
 * Thin HAL shim for the architecture-neutral time/clockevent model.
 * NX-HAL publishes a clocksource plus a per-CPU clockevent; NX-CORE chooses
 * absolute deadline programming when the provider supports it and retains a
 * bounded periodic fallback contract.
 *
 * No hardware registers are touched here.  AArch64 uses the architected
 * virtual timer; amd64 selects TSC-deadline when available or calibrated LAPIC
 * one-shot countdown otherwise, with periodic LAPIC retained as fallback.
 *
 * Invariants:
 *   - arch_irq_init() (driver_irq_init) must complete before this call so
 *     that the interrupt controller is ready to accept timer IRQs.
 *   - Per-CPU timer arm (timer_init_percpu) is called later, once per
 *     secondary CPU, not from here.
 */
#include <kernel/drivers.h>
#include <hal/core.h>
#include <kernel/printk.h>
#include <kernel/timer.h>

/*
 * driver_timer_init - initialise the platform system timer.
 *
 * Initializes the HAL time provider and Core timer policy.  Per-CPU
 * clockevents are armed later after each CPU has scheduler state, so this
 * global step does not assume a periodic HZ interrupt.
 *
 * Locking: none; called once from the boot CPU before SMP.
 * Hardware side effects remain inside NX-HAL providers.
 */
void driver_timer_init(void) {
    hal_timer_global_init();
    OS1libkernel_time_init();
    OS1libkernel_timer_core_init();
    pr_info("Timer driver initialized via HAL: counter=%lu Hz, scheduler=%lu Hz\n",
            OS1libkernel_time_counter_frequency(),
            (uint64_t)OS1LIBKERNEL_TIMER_HZ);
}
