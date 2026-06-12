/*
 * kernel/drivers/gic/gic.c
 * ARM GICv2 Interrupt Controller Driver — QEMU virt (aarch64)
 *
 * Implements the struct irq_chip interface for the ARM Generic Interrupt
 * Controller version 2 (GICv2) as found on the QEMU 'virt' board.
 *
 * GICv2 overview (relevant to this driver):
 *   Distributor (GICD, at GICD_BASE): global; manages SPI routing, priority,
 *   enable/disable, and pending state for all interrupts.
 *   CPU Interface (GICC, at GICC_BASE): per-CPU; controls priority masking,
 *   acknowledges (IAR read) and EOI (EOIR write) for this CPU.
 *
 * Interrupt categories:
 *   SGI (IDs  0-15): Software-Generated Interrupts; per-CPU, used for IPIs.
 *   PPI (IDs 16-31): Private Peripheral Interrupts; per-CPU (timer at 27).
 *   SPI (IDs 32+):   Shared Peripheral Interrupts; routed to CPUs via ITARGETSR.
 *
 * Invariants:
 *   - gic_init_dist() is called once on the boot CPU (via irq_init()).
 *   - gic_init_cpu() is called on every CPU (via irq_init_percpu()).
 *   - GICC_PMR = 0xFF: all interrupt priorities are accepted.
 *   - All SPIs are hard-routed to CPU 0 (GICD_ITARGETSR = 0x01010101).
 *
 * Known issues:
 *   DRV-GIC-01  (W3 WRONG-DESIGN) All SPIs permanently targeted to CPU 0
 *               via GICD_ITARGETSR = 0x01010101 in gic_init_dist().  No
 *               affinity hints, no round-robin — all device interrupts
 *               serialise on core 0; blocks SMP load distribution.
 *   DRV-GIC-02  (W2 BUG) gic_eoi() writes only the IRQ number to GICC_EOIR.
 *               For SGIs (irq < 16), GICv2 requires bits [12:10] to hold the
 *               source CPU ID; writing an IRQ number without those bits may
 *               leave the SGI active on the distributor.
 */
#include <drivers/gic.h>
#include "gic_regs.h"
#include <kernel/irq.h>
#include <kernel/memlayout.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/types.h>

/* GICD_REG(off): dereference GICD MMIO register at GICD_BASE + off (32-bit).
 * GICC_REG(off): dereference GICC MMIO register at GICC_BASE + off (32-bit).
 * Both bases come from gic_regs.h (physical addresses); the access happens
 * at their direct-map kernel VA via phys_to_virt (identity while
 * KERNEL_VIRT_BASE == 0). */
/* MMIO access */
#define GICD_REG(off) (*(volatile uint32_t *)phys_to_virt(GICD_BASE + (off)))
#define GICC_REG(off) (*(volatile uint32_t *)phys_to_virt(GICC_BASE + (off)))

/* gic_num_irqs: total interrupt lines reported by GICD_TYPER, clamped to
 * GIC_MAX_IRQS.  Set once in gic_init_dist(); read-only thereafter. */
/* Number of interrupt lines */
static uint32_t gic_num_irqs;

/*
 * gic_init_dist - initialise the GIC distributor (boot CPU only).
 *
 * Called once via irq_init() → chip->init().  Programs the distributor:
 *   1. Disable distributor (GICD_CTLR = 0) before programming.
 *   2. Read GICD_TYPER bits[4:0] to determine number of interrupt lines
 *      (ITLinesNumber): gic_num_irqs = (ITLinesNumber + 1) * 32, capped at
 *      GIC_MAX_IRQS.
 *   3. Write 0xFFFFFFFF to every GICD_ICENABLER register to disable all IRQs.
 *   4. Write 0xFFFFFFFF to every GICD_ICPENDR register to clear pending state.
 *   5. Set priority 0xA0 for all SPIs (IDs >= GIC_SPI_START = 32) via
 *      GICD_IPRIORITYR; 4 priorities per 32-bit register.
 *   6. Target all SPIs to CPU 0 via GICD_ITARGETSR = 0x01010101.
 *      NOTE(DRV-GIC-01): hard-codes all SPIs to CPU 0; no SMP distribution.
 *   7. Configure all SPIs as level-triggered (GICD_ICFGR = 0 for word i>=2).
 *   8. Enable distributor (GICD_CTLR = 1).
 *
 * MMIO registers written: GICD_CTLR, GICD_ICENABLER[], GICD_ICPENDR[],
 *   GICD_IPRIORITYR[], GICD_ITARGETSR[], GICD_ICFGR[].
 * MMIO registers read: GICD_TYPER.
 *
 * Locking: called before SMP; no concurrent access.
 * IRQ context: NO.
 */
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
  /* NOTE(DRV-GIC-01): 0x01010101 packs four CPU-target bytes each = 0x01
   * (CPU 0 only).  All SPIs arrive exclusively on core 0; no SMP balancing. */
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
 * gic_init_cpu - initialise the GIC CPU interface (called on each CPU).
 *
 * Called via irq_init_percpu() → chip->init_percpu() on every CPU at SMP
 * bring-up.  Programs this CPU's GICC interface:
 *   1. Write 0xFFFFFFFF to GICD_ICENABLER(0) to disable all SGIs and PPIs
 *      for this CPU (distributor register 0 covers IDs 0-31, which are
 *      per-CPU and therefore banked).
 *   2. Write priority 0xA0 for all SGIs/PPIs via GICD_IPRIORITYR (banked
 *      registers for IDs 0-31; GIC_SPI_START = 32).
 *   3. GICC_PMR = 0xFF: priority mask allows all interrupts through.
 *   4. GICC_BPR = 0: no priority grouping / preemption splitting.
 *   5. GICC_CTLR = 1: enable this CPU's interface.
 *
 * MMIO registers written: GICD_ICENABLER(0), GICD_IPRIORITYR(0..7),
 *   GICC_PMR, GICC_BPR, GICC_CTLR.
 *
 * Locking: operates on per-CPU (banked) GICC registers; no shared-state lock.
 * IRQ context: NO — called from CPU bring-up code.
 */
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
 * gic_enable - unmask a single interrupt line at the distributor.
 *
 * @irq: interrupt ID to enable (0..gic_num_irqs-1).
 *
 * Writes a single bit to the appropriate GICD_ISENABLER register.  Each
 * GICD_ISENABLER word controls 32 interrupts; bit position = irq % 32.
 * Writing 0 bits has no effect (set-enable register).
 *
 * MMIO register written: GICD_ISENABLER(irq / 32).
 *
 * Locking: none; single 32-bit write; atomic at hardware level for same-word
 *          enable operations (GICv2 spec §4.3.6).
 * IRQ context: safe.
 */
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
 * gic_disable - mask a single interrupt line at the distributor.
 *
 * @irq: interrupt ID to disable.
 *
 * Writes a single bit to GICD_ICENABLER (clear-enable register; writing 0
 * has no effect).  Called by irq_unregister() and irq_disable() from irq.c
 * to prevent interrupt storms for unhandled IRQs.
 *
 * MMIO register written: GICD_ICENABLER(irq / 32).
 *
 * Locking: none (same atomicity argument as gic_enable).
 * IRQ context: safe.
 */
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

/*
 * gic_set_prio - set the priority of a single interrupt.
 *
 * @irq:      interrupt ID to modify.
 * @priority: 8-bit priority value (lower = higher priority in GICv2).
 *
 * Each GICD_IPRIORITYR register holds 4 priority bytes (one per interrupt).
 * The byte position within the word is (irq % 4) * 8.  Performs a
 * read-modify-write to update only the target byte.
 *
 * MMIO registers touched: GICD_IPRIORITYR(irq / 4) read then write.
 *
 * Locking: read-modify-write is not atomic with respect to concurrent
 *          priority changes for the same register word.  Safe at boot.
 * IRQ context: safe for isolated calls, but see atomicity caveat above.
 */
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

/*
 * gic_send_ipi - broadcast SGI0 to all CPUs except the sender.
 *
 * Writes GICD_SGIR with TargetListFilter = 0b01 (bits [25:24]) which means
 * "all PEs except the requesting PE" (GICv2 spec §4.3.15), and SGI ID = 0
 * (bits [3:0]).  irq_handler() in irq.c treats SGI0 as a panic-halt IPI.
 *
 * MMIO register written: GICD_SGIR (write-only in GICv2).
 *
 * Locking: none; GICD_SGIR write is self-contained.
 * IRQ context: safe.
 */
static void gic_send_ipi(void) {
  /* TargetListFilter bits[25:24] = 0b01 means "all CPUs except requestor" */
  GICD_REG(GICD_SGIR) = (1U << 24) | 0; /* filter=broadcast-except-self, SGI0 */
}

/*
 * gic_ack - acknowledge the current interrupt and return its ID.
 *
 * Reads GICC_IAR (Interrupt Acknowledge Register, CPU interface offset
 * GICC_IAR).  This read both signals the acknowledgement to the GIC and
 * returns a 10-bit interrupt ID in bits [9:0].  Value 1023 = spurious.
 *
 * Must be called at the start of interrupt processing; the GIC transitions
 * the interrupt to the Active state upon IAR read.
 *
 * MMIO register read: GICC_IAR.
 * Returns: interrupt ID (0..1022) or 1023 (spurious/no interrupt).
 *
 * Locking: per-CPU GICC register; no cross-CPU contention.
 * IRQ context: YES — must be called from IRQ handler.
 */
static uint32_t gic_ack(void) { return GICC_REG(GICC_IAR) & 0x3FF; }

/*
 * gic_eoi - signal End-Of-Interrupt to the GIC CPU interface.
 *
 * @irq: interrupt ID returned by gic_ack() for this interrupt.
 *
 * Writes @irq to GICC_EOIR (End Of Interrupt Register) to transition the
 * interrupt from Active to Inactive, allowing lower-priority interrupts to
 * be signalled.  Must be called after the handler finishes processing.
 *
 * MMIO register written: GICC_EOIR.
 *
 * NOTE(DRV-GIC-02): For SGIs (irq < 16) the GICv2 spec requires bits [12:10]
 * of GICC_EOIR to contain the source CPU ID (from GICC_IAR bits [12:10]).
 * Writing only the IRQ number (bits[9:0]) with bits[12:10]=0 may leave the
 * SGI active on the distributor for the wrong source CPU.
 *
 * Locking: per-CPU GICC register; no lock needed.
 * IRQ context: YES — must be called from the IRQ dispatch path.
 */
static void gic_eoi(uint32_t irq) {
  volatile uint32_t *eoir_reg =
      (volatile uint32_t *)phys_to_virt(GICC_BASE + GICC_EOIR);
  *eoir_reg = irq;
}

/* gic_chip: irq_chip vtable for the ARM GICv2 on aarch64.
 * Registered via gic_register() → irq_register_chip(); becomes current_chip
 * in irq.c.  All ops are filled; the chip is fully used on aarch64. */
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

/*
 * gic_register - make the GICv2 the active irq_chip.
 *
 * Calls irq_register_chip(&gic_chip) to store gic_chip as current_chip in
 * irq.c.  Must be called before irq_init().  Typically invoked from the
 * aarch64 HAL's arch_irq_init() during early boot.
 *
 * Locking: none; called before SMP.
 */
void gic_register(void) {
  irq_register_chip(&gic_chip);
}
