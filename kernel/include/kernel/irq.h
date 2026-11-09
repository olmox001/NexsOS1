#ifndef _KERNEL_IRQ_H
#define _KERNEL_IRQ_H

#include <hal/frame.h>
#include <hal/irq.h>
#include <kernel/nx_contract.h>
#include <kernel/types.h>

/* Forward declaration for pt_regs */

/* Interrupt handler type */
typedef nxhal_irq_handler_t irq_handler_t;

/* Core IRQ management functions */
void irq_init(void);
void irq_init_percpu(void);

int irq_register(uint32_t irq, irq_handler_t handler, void *data) NX_MUST_USE;
void irq_unregister(uint32_t irq);
void irq_enable(uint32_t irq);
void irq_disable(uint32_t irq);
void irq_send_ipi_all(void);

/* Main entry point from architecture exception vectors */
hal_frame_t *irq_handler(hal_frame_t *regs);
hal_frame_t *irq_dispatch(uint32_t irq, hal_frame_t *regs);

/* irq_chip_end - route an end-of-interrupt through the registered chip.
 * The single EOI mechanism for vectored dispatchers (amd64 IDT path);
 * the aarch64 acknowledge-loop calls chip->end() directly. */
void irq_chip_end(uint32_t irq);

#include <kernel/platform.h>

/* Architecture-specific but generic-mapped IRQ numbers */
#define IRQ_TIMER PLATFORM_IRQ_TIMER_VIRT

#endif /* _KERNEL_IRQ_H */
