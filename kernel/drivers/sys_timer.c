/*
 * kernel/drivers/sys_timer.c
 * System Timer Driver Glue
 *
 * Thin HAL shim that triggers arch-specific hardware timer initialisation
 * through arch_timer_init().  On aarch64 this invokes timer_init() which
 * reads CNTFRQ_EL0, precomputes tick intervals, and enables IRQ_TIMER (PPI
 * 27) in the GIC.  On amd64 it invokes pit_init_hz() to configure the 8253
 * PIT at the desired HZ rate.
 *
 * No hardware registers are touched here; all timer MMIO / MSR access lives
 * in kernel/drivers/timer/timer.c (aarch64) and kernel/drivers/timer/
 * pic_pit.c (amd64).
 *
 * Invariants:
 *   - arch_irq_init() (driver_irq_init) must complete before this call so
 *     that the interrupt controller is ready to accept timer IRQs.
 *   - Per-CPU timer arm (timer_init_percpu) is called later, once per
 *     secondary CPU, not from here.
 */
#include <kernel/drivers.h>
#include <kernel/hal.h>
#include <kernel/printk.h>

/*
 * driver_timer_init - initialise the platform system timer.
 *
 * Delegates to arch_timer_init().  After this call the timer fires at HZ
 * intervals and scheduler ticks begin on the boot CPU.
 *
 * Locking: none; called once from boot CPU before SMP.
 * MMIO side effects: delegated to arch_timer_init().
 */
void driver_timer_init(void) {
    arch_timer_init();
    pr_info("%s", "Timer driver initialized via HAL\n");
}
