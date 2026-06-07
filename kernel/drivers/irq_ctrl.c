/*
 * kernel/drivers/irq_ctrl.c
 * IRQ Controller Driver Glue
 *
 * Thin HAL shim that triggers arch-specific interrupt controller
 * initialisation through the HAL's arch_irq_init() hook.  On aarch64 this
 * drives gic_register() + irq_init() (GICv2 distributor setup); on amd64 it
 * drives pic_init() + LAPIC initialisation.
 *
 * No registers are accessed here directly; all MMIO lives in gic.c (aarch64)
 * and pic_pit.c / idt.c (amd64).
 *
 * Invariants:
 *   - Must be called after the MMU / identity map is established so that the
 *     controller's MMIO window is accessible.
 *   - Must be called before any irq_register() call.
 */
#include <kernel/drivers.h>
#include <kernel/hal.h>
#include <kernel/printk.h>

/*
 * driver_irq_init - initialise the platform interrupt controller.
 *
 * Delegates to arch_irq_init() which calls the correct chip's init sequence.
 * On aarch64: gic_register() sets current_chip, then irq_init() programs the
 * GIC distributor.  On amd64: pic_init() remaps PIC vectors to 32–47 and
 * registers pic_chip; LAPIC is enabled separately in the HAL.
 *
 * Locking: none; called once from boot CPU before SMP.
 * MMIO side effects: delegated to arch_irq_init().
 */
void driver_irq_init(void) {
    arch_irq_init();
    pr_info("%s", "IRQ driver initialized via HAL\n");
}
