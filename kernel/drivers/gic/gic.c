/*
 * kernel/drivers/gic/gic.c
 * ARM GICv2 Driver for QEMU virt machine
 */
#include <drivers/gic.h>
#include "gic_regs.h"
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/types.h>

/* MMIO access */
#define GICD_REG(off) (*(volatile uint32_t *)(GICD_BASE + (off)))
#define GICC_REG(off) (*(volatile uint32_t *)(GICC_BASE + (off)))

/* Number of interrupt lines */
static uint32_t gic_num_irqs;

/*
 * Initialize GIC distributor (called once on boot CPU)
 */
static void gic_init_dist(void) {
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
static void gic_init_cpu(void) {
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
static void gic_enable(uint32_t irq) {
  if (irq >= gic_num_irqs)
    return;

  uint32_t reg = irq / 32;
  uint32_t bit = irq % 32;

  GICD_REG(GICD_ISENABLER(reg)) = (1U << bit);
}

/*
 * Disable an interrupt
 */
static void gic_disable(uint32_t irq) {
  if (irq >= gic_num_irqs)
    return;

  uint32_t reg = irq / 32;
  uint32_t bit = irq % 32;

  GICD_REG(GICD_ICENABLER(reg)) = (1U << bit);
}

static void gic_set_prio(uint32_t irq, uint8_t priority) {
  if (irq >= gic_num_irqs)
    return;

  uint32_t reg = irq / 4;
  uint32_t shift = (irq % 4) * 8;
  uint32_t val = GICD_REG(GICD_IPRIORITYR(reg));

  val &= ~(0xFFU << shift);
  val |= ((uint32_t)priority << shift);

  GICD_REG(GICD_IPRIORITYR(reg)) = val;
}

static void gic_send_ipi(void) {
  /* TargetListFilter bits[25:24] = 0b01 means "all CPUs except requestor" */
  GICD_REG(GICD_SGIR) = (1U << 24) | 0; /* filter=broadcast-except-self, SGI0 */
}

static uint32_t gic_ack(void) { return GICC_REG(GICC_IAR) & 0x3FF; }

static void gic_eoi(uint32_t irq) {
  volatile uint32_t *eoir_reg = (volatile uint32_t *)(GICC_BASE + GICC_EOIR);
  *eoir_reg = irq;
}

/* irq_chip implementation */
static struct irq_chip gic_chip = {
  .name = "ARM GICv2",
  .init = gic_init_dist,
  .init_percpu = gic_init_cpu,
  .enable = gic_enable,
  .disable = gic_disable,
  .set_priority = gic_set_prio,
  .send_ipi_all = gic_send_ipi,
  .acknowledge = gic_ack,
  .end = gic_eoi,
};

void gic_register(void) {
  irq_register_chip(&gic_chip);
}
