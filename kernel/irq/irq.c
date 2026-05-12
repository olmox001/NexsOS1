#include <kernel/irq.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/types.h>

#define MAX_IRQS 256

static struct {
  irq_handler_t handler;
  void *data;
} irq_handlers[MAX_IRQS];

static struct irq_chip *current_chip = NULL;

void irq_register_chip(struct irq_chip *chip) {
  current_chip = chip;
  pr_info("IRQ: Registered chip %s\n", chip->name);
}

void irq_init(void) {
  if (current_chip && current_chip->init) {
    current_chip->init();
  }
}

void irq_init_percpu(void) {
  if (current_chip && current_chip->init_percpu) {
    current_chip->init_percpu();
  }
}

int irq_register(uint32_t irq, irq_handler_t handler, void *data) {
  if (irq >= MAX_IRQS)
    return -EINVAL;

  if (irq_handlers[irq].handler)
    return -EBUSY;

  irq_handlers[irq].handler = handler;
  irq_handlers[irq].data = data;

  if (current_chip && current_chip->enable) {
    current_chip->enable(irq);
  }

  return 0;
}

void irq_unregister(uint32_t irq) {
  if (irq >= MAX_IRQS)
    return;

  if (current_chip && current_chip->disable) {
    current_chip->disable(irq);
  }

  irq_handlers[irq].handler = NULL;
  irq_handlers[irq].data = NULL;
}

void irq_enable(uint32_t irq) {
  if (current_chip && current_chip->enable) {
    current_chip->enable(irq);
  }
}

void irq_disable(uint32_t irq) {
  if (current_chip && current_chip->disable) {
    current_chip->disable(irq);
  }
}

void irq_send_ipi_all(void) {
  if (current_chip && current_chip->send_ipi_all) {
    current_chip->send_ipi_all();
  }
}

/* Internal panic halt helper */
static void cpu_halt_from_ipi(void) {
  extern volatile int panic_flag;
  panic_flag = 1;
  arch_timer_control(0);
  arch_cpu_halt();
}

struct pt_regs *irq_handler(struct pt_regs *regs) {
  uint32_t irq;
  struct pt_regs *ret_regs = regs;

  if (!current_chip)
    return regs;

  while (1) {
    irq = current_chip->acknowledge();

    if (irq == 1023) /* Spurious or No more interrupts */
      break;

    /* SGI0: panic halt IPI */
    if (irq == 0) {
      current_chip->end(irq);
      cpu_halt_from_ipi();
    }

    /* Handle IRQ */
    if (irq == IRQ_TIMER || irq == 30) {
      /* Timer Interrupt - Returns new regs if context switch occurred */
      extern struct pt_regs *timer_handler(struct pt_regs * regs);
      ret_regs = timer_handler(ret_regs);
      current_chip->end(irq);
      return ret_regs;
    } else if (irq < MAX_IRQS && irq_handlers[irq].handler) {
      irq_handlers[irq].handler(irq, irq_handlers[irq].data);
    } else {
      pr_warn("IRQ: Unhandled interrupt %u\n", irq);
    }

    current_chip->end(irq);
  }

  return ret_regs;
}
