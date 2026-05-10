/*
 * kernel/drivers/gic/gic.c
 * ARM GICv2 Driver for QEMU virt machine
 */
#include <drivers/gic.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/timer.h>
#include <kernel/types.h>

/* Disable optimization to prevent register corruption in IRQ handler loop */

/* MMIO access */
#define GICD_REG(off) (*(volatile uint32_t *)(GICD_BASE + (off)))
#define GICC_REG(off) (*(volatile uint32_t *)(GICC_BASE + (off)))

/* IRQ handler table */
static struct {
  irq_handler_t handler;
  void *data;
} irq_handlers[GIC_MAX_IRQS];

/* Number of interrupt lines */
static uint32_t gic_num_irqs;

/*
 * Initialize GIC distributor (called once on boot CPU)
 */
void gic_init(void) {
  uint32_t typer;
  uint32_t i;

  /* Disable distributor */
  GICD_REG(GICD_CTLR) = 0;

  /* Get number of interrupt lines */
  typer = GICD_REG(GICD_TYPER);
  gic_num_irqs = ((typer & 0x1F) + 1) * 32;
  if (gic_num_irqs > GIC_MAX_IRQS)
    gic_num_irqs = GIC_MAX_IRQS;

  pr_info("GIC: %u interrupt lines\n", gic_num_irqs);

  /* Disable all interrupts */
  for (i = 0; i < gic_num_irqs / 32; i++)
    GICD_REG(GICD_ICENABLER(i)) = 0xFFFFFFFF;

  /* Clear all pending interrupts */
  for (i = 0; i < gic_num_irqs / 32; i++)
    GICD_REG(GICD_ICPENDR(i)) = 0xFFFFFFFF;

  /* Set all SPIs to lowest priority */
  for (i = GIC_SPI_START / 4; i < gic_num_irqs / 4; i++)
    GICD_REG(GICD_IPRIORITYR(i)) = 0xA0A0A0A0;

  /* Target all SPIs to CPU 0 */
  for (i = GIC_SPI_START / 4; i < gic_num_irqs / 4; i++)
    GICD_REG(GICD_ITARGETSR(i)) = 0x01010101;

  /* Configure all SPIs as level-triggered */
  for (i = 2; i < gic_num_irqs / 16; i++)
    GICD_REG(GICD_ICFGR(i)) = 0;

  /* Enable distributor */
  GICD_REG(GICD_CTLR) = 1;

  pr_info("%s", "GIC: Distributor initialized\n");
}

/*
 * Initialize GIC CPU interface (called on each CPU)
 */
void gic_init_percpu(void) {
  uint32_t i;

  /* Disable all SGIs and PPIs */
  GICD_REG(GICD_ICENABLER(0)) = 0xFFFFFFFF;

  /* Set priority for SGIs and PPIs */
  for (i = 0; i < GIC_SPI_START / 4; i++)
    GICD_REG(GICD_IPRIORITYR(i)) = 0xA0A0A0A0;

  /* Set priority mask - accept all priorities */
  GICC_REG(GICC_PMR) = 0xFF;

  /* No priority grouping */
  GICC_REG(GICC_BPR) = 0;

  /* Enable CPU interface */
  GICC_REG(GICC_CTLR) = 1;
}

/*
 * Enable an interrupt
 */
void gic_enable_irq(uint32_t irq) {
  if (irq >= gic_num_irqs)
    return;

  uint32_t reg = irq / 32;
  uint32_t bit = irq % 32;

  GICD_REG(GICD_ISENABLER(reg)) = (1U << bit);
}

/*
 * Disable an interrupt
 */
void gic_disable_irq(uint32_t irq) {
  if (irq >= gic_num_irqs)
    return;

  uint32_t reg = irq / 32;
  uint32_t bit = irq % 32;

  GICD_REG(GICD_ICENABLER(reg)) = (1U << bit);
}

/*
 * Set interrupt priority (0 = highest)
 */
void gic_set_priority(uint32_t irq, uint8_t priority) {
  if (irq >= gic_num_irqs)
    return;

  uint32_t reg = irq / 4;
  uint32_t shift = (irq % 4) * 8;
  uint32_t val = GICD_REG(GICD_IPRIORITYR(reg));

  val &= ~(0xFFU << shift);
  val |= ((uint32_t)priority << shift);

  GICD_REG(GICD_IPRIORITYR(reg)) = val;
}

/*
 * Set interrupt target CPUs
 */
void gic_set_target(uint32_t irq, uint8_t cpu_mask) {
  if (irq < GIC_SPI_START || irq >= gic_num_irqs)
    return;

  uint32_t reg = irq / 4;
  uint32_t shift = (irq % 4) * 8;
  uint32_t val = GICD_REG(GICD_ITARGETSR(reg));

  val &= ~(0xFFU << shift);
  val |= ((uint32_t)cpu_mask << shift);

  GICD_REG(GICD_ITARGETSR(reg)) = val;
}

/*
 * Acknowledge interrupt (returns IRQ number)
 */
uint32_t gic_acknowledge_irq(void) { return GICC_REG(GICC_IAR) & 0x3FF; }

/*
 * Signal end of interrupt
 */
void gic_end_irq(uint32_t irq) {
  /* Explicitly load address to avoid register corruption/caching issues in IRQ
   * loop */
  volatile uint32_t *eoir_reg = (volatile uint32_t *)(GICC_BASE + GICC_EOIR);
  *eoir_reg = irq;
}

/*
 * Send Software Generated Interrupt
 */
void gic_send_sgi(uint32_t irq, uint8_t target_list) {
  if (irq > 15)
    return;

  /* SGI: target list in bits 16-23, IRQ in bits 0-3 */
  GICD_REG(GICD_SGIR) = ((uint32_t)target_list << 16) | irq;
}

/*
 * Register IRQ handler
 */
int irq_register(uint32_t irq, irq_handler_t handler, void *data) {
  if (irq >= GIC_MAX_IRQS)
    return -EINVAL;

  if (irq_handlers[irq].handler)
    return -EBUSY;

  irq_handlers[irq].handler = handler;
  irq_handlers[irq].data = data;

  gic_enable_irq(irq);

  return 0;
}

/*
 * Unregister IRQ handler
 */
void irq_unregister(uint32_t irq) {
  if (irq >= GIC_MAX_IRQS)
    return;

  gic_disable_irq(irq);

  irq_handlers[irq].handler = NULL;
  irq_handlers[irq].data = NULL;
}

#include <kernel/sched.h>

/* ... */

/*
 * Main IRQ handler (called from exception vector)
 */
struct pt_regs *irq_handler(struct pt_regs *regs) {
  uint32_t irq;
  struct pt_regs *ret_regs = regs;

  while (1) {
    irq = gic_acknowledge_irq();

    if (irq == GIC_SPURIOUS_IRQ)
      break;

    /* Handle IRQ */
    if (irq == 27 || irq == 30) {
      /* Timer Interrupt - Special Case to pass regs and return new regs */
      /* CRITICAL: If a context switch occurs, we must return the new regs
       * immediately and not continue the loop, as the 'regs' pointer is now
       * stale and the environment (stack, etc) might have changed for the new
       * process. */
      ret_regs = timer_handler(ret_regs);
      gic_end_irq(irq);
      return ret_regs;
    } else if (irq < GIC_MAX_IRQS && irq_handlers[irq].handler) {
      /* Standard Driver Handler */
      irq_handlers[irq].handler(irq, irq_handlers[irq].data);
    } else {
      pr_warn("GIC: Unhandled IRQ %u\n", irq);
    }

    gic_end_irq(irq);
  }

  return ret_regs;
}
