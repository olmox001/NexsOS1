/*
 * kernel/drivers/console.c
 * Console Driver Glue
 *
 * Thin HAL shim that delegates early-console initialisation to the
 * arch-selected UART driver (PL011 on aarch64, 16550 on amd64).  No device
 * registers are touched here; all MMIO/I/O-port access lives in the
 * respective uart driver.
 *
 * Invariants:
 *   - uart_init() (arch-specific) must complete before any printk output.
 *   - This file contains no shared state; it is safe to call from the boot
 *     CPU before SMP is active.
 *
 * Known issues:
 *   DRV-CONSOLE-01  (W2 WRONG-DESIGN) platform.h (included transitively via
 *                   hal.h) exposes aarch64-specific constants such as
 *                   PLATFORM_GICD_BASE and PLATFORM_UART_BASE to amd64 builds
 *                   where they have no meaning.
 */
#include <kernel/drivers.h>
#include <kernel/hal.h>
#include <kernel/printk.h>

/*
 * driver_console_init - initialise the primary serial console.
 *
 * Calls the arch UART driver's uart_init() to configure baud rate, FIFOs,
 * and (on aarch64) register the RX IRQ handler.  After this call printk()
 * is fully operational.
 *
 * Locking: none; must be called once from boot CPU before SMP.
 * MMIO/IO side effects: delegated entirely to uart_init().
 */
void driver_console_init(void) {
    /* Console UART is initialized very early, but we can wrap it if needed.
     * For now, we assume arch_platform_early_init or similar handled it,
     * or we add arch_console_init() to HAL.
     */
    extern void uart_init(void);
    uart_init();
    pr_info("%s", "Console driver initialized\n");
}
