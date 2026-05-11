/*
 * kernel/drivers/gic/gic_regs.h
 * Private GICv2 register definitions
 */
#ifndef _GIC_REGS_H
#define _GIC_REGS_H

#include <kernel/platform.h>

/* GICv2 addresses */
#define GICD_BASE PLATFORM_GICD_BASE
#define GICC_BASE PLATFORM_GICC_BASE

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
#define GIC_SPI_START 32
#define GIC_MAX_IRQS 256
#define GIC_SPURIOUS_IRQ 1023

#endif /* _GIC_REGS_H */
