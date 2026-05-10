/*
 * kernel/include/drivers/gic.h
 * ARM GICv2 Interrupt Controller
 */
#ifndef _DRIVERS_GIC_H
#define _DRIVERS_GIC_H

#include <kernel/types.h>

/* GICv2 addresses for QEMU virt machine */
#define GICD_BASE 0x08000000UL /* Distributor */
#define GICC_BASE 0x08010000UL /* CPU Interface */

/* Distributor registers */
#define GICD_CTLR 0x000                        /* Control */
#define GICD_TYPER 0x004                       /* Type */
#define GICD_IIDR 0x008                        /* Implementer ID */
#define GICD_IGROUPR(n) (0x080 + ((n) * 4))    /* Group */
#define GICD_ISENABLER(n) (0x100 + ((n) * 4))  /* Set Enable */
#define GICD_ICENABLER(n) (0x180 + ((n) * 4))  /* Clear Enable */
#define GICD_ISPENDR(n) (0x200 + ((n) * 4))    /* Set Pending */
#define GICD_ICPENDR(n) (0x280 + ((n) * 4))    /* Clear Pending */
#define GICD_ISACTIVER(n) (0x300 + ((n) * 4))  /* Set Active */
#define GICD_ICACTIVER(n) (0x380 + ((n) * 4))  /* Clear Active */
#define GICD_IPRIORITYR(n) (0x400 + ((n) * 4)) /* Priority */
#define GICD_ITARGETSR(n) (0x800 + ((n) * 4))  /* Target */
#define GICD_ICFGR(n) (0xC00 + ((n) * 4))      /* Config */
#define GICD_SGIR 0xF00 /* Software Generated Interrupt */

/* CPU Interface registers */
#define GICC_CTLR 0x00  /* Control */
#define GICC_PMR 0x04   /* Priority Mask */
#define GICC_BPR 0x08   /* Binary Point */
#define GICC_IAR 0x0C   /* Interrupt Acknowledge */
#define GICC_EOIR 0x10  /* End of Interrupt */
#define GICC_RPR 0x14   /* Running Priority */
#define GICC_HPPIR 0x18 /* Highest Priority Pending */

/* GIC constants */
#define GIC_SPI_START 32 /* First SPI interrupt */
#define GIC_MAX_IRQS 256
#define GIC_SPURIOUS_IRQ 1023

/* IRQ numbers for QEMU virt */
#define IRQ_TIMER_VIRT 27 /* Virtual timer */
#define IRQ_TIMER_PHYS 30 /* Physical timer */
#define IRQ_UART0 33      /* PL011 UART */

/* Interrupt handler type */
typedef void (*irq_handler_t)(uint32_t irq, void *data);

/* Functions */
void gic_init(void);
void gic_init_percpu(void);
void gic_enable_irq(uint32_t irq);
void gic_disable_irq(uint32_t irq);
void gic_set_priority(uint32_t irq, uint8_t priority);
void gic_set_target(uint32_t irq, uint8_t cpu_mask);
uint32_t gic_acknowledge_irq(void);
void gic_end_irq(uint32_t irq);
void gic_send_sgi(uint32_t irq, uint8_t target_list);
void gic_send_ipi_all(void); /* Send SGI0 to all other CPUs (panic halt) */

/* IRQ registration */
int irq_register(uint32_t irq, irq_handler_t handler, void *data);
void irq_unregister(uint32_t irq);

#include <kernel/sched.h>

/* IRQ handling entry point */
struct pt_regs *irq_handler(struct pt_regs *regs);

#endif /* _DRIVERS_GIC_H */
