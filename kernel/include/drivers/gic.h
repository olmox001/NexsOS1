/*
 * kernel/include/drivers/gic.h
 * ARM GICv2 Interrupt Controller Public Interface
 */
#ifndef _DRIVERS_GIC_H
#define _DRIVERS_GIC_H

#include <kernel/types.h>

/**
 * Register the GICv2 driver with the generic IRQ framework.
 * This should be called early during boot initialization.
 */
void gic_register(void);

#endif /* _DRIVERS_GIC_H */
