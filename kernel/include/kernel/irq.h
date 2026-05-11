#ifndef _KERNEL_IRQ_H
#define _KERNEL_IRQ_H

#include <kernel/types.h>

/* Forward declaration for pt_regs */
struct pt_regs;

/* Interrupt handler type */
typedef void (*irq_handler_t)(uint32_t irq, void *data);

/**
 * struct irq_chip - Hardware Abstraction for Interrupt Controllers
 */
struct irq_chip {
  const char *name;
  void (*init)(void);
  void (*init_percpu)(void);
  void (*enable)(uint32_t irq);
  void (*disable)(uint32_t irq);
  void (*set_priority)(uint32_t irq, uint8_t priority);
  void (*send_ipi_all)(void); /* Broadcast panic halt IPI */
  uint32_t (*acknowledge)(void);
  void (*end)(uint32_t irq);
};

/* Core IRQ management functions */
void irq_init(void);
void irq_init_percpu(void);
void irq_register_chip(struct irq_chip *chip);

int irq_register(uint32_t irq, irq_handler_t handler, void *data);
void irq_unregister(uint32_t irq);
void irq_enable(uint32_t irq);
void irq_disable(uint32_t irq);
void irq_send_ipi_all(void);

/* Main entry point from architecture exception vectors */
struct pt_regs *irq_handler(struct pt_regs *regs);
void irq_dispatch(uint32_t irq);

#include <kernel/platform.h>

/* Architecture-specific but generic-mapped IRQ numbers */
#define IRQ_TIMER PLATFORM_IRQ_TIMER_VIRT

#endif /* _KERNEL_IRQ_H */
