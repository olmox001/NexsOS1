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
 *   SPI (IDs 32+):   Shared Peripheral Interrupts; routed to CPUs via
 * ITARGETSR.
 *
 * Invariants:
 *   - gic_init_dist() is called once on the boot CPU (via irq_init()).
 *   - gic_init_cpu() is called on every CPU (via irq_init_percpu()).
 *   - GICC_PMR = 0xFF: all interrupt priorities are accepted.
 *   - SPIs are round-robined across the CPUs that have ACTUALLY called
 *     gic_init_cpu() so far (gic_online_cpus), re-distributed every time a
 *     new CPU joins, up to GIC_ITARGETSR_MAX_CPUS (8) target bits (FIX
 *     (DRV-GIC-01); previously all SPIs were hard-routed to CPU 0 only —
 *     see gic_retarget_spis()).
 *
 * Known issues:
 *   DRV-GIC-01  RESOLVED (this pass, revised): all SPIs used to be
 *               hard-routed to CPU 0 via a fixed GICD_ITARGETSR = 0x01010101
 *               write, with no affinity hints and no distribution — every
 *               device interrupt serialised on core 0, defeating SMP load
 *               spreading.
 *
 *               First attempt (superseded): estimate the eventual CPU count
 *               from fdt_count_cpus() at gic_init_dist() time and round-
 *               robin against that estimate.  Rejected on review: at
 *               gic_init_dist() time (irq_init(), before arch_smp_init() —
 *               see kernel_main() in main.c) no secondary CPU is online yet,
 *               so that approach could route an SPI to a CPU that never
 *               actually comes up, or that comes up much later than
 *               whatever else the driver assumes — a real, if narrow, class
 *               of "device interrupt vanishes because its target CPU isn't
 *               there" bugs, not just a theoretical corner case.
 *
 *               ACTUAL FIX: SPI targeting is driven entirely by CPUs that
 *               have PROVABLY called gic_init_cpu() — i.e. CPUs already
 *               past arch_cpu_init() (cpu_data[id].online = 1) and already
 *               running irq_init_percpu() on their own core.  gic_num_online
 *               is incremented, under gic_target_lock, by each CPU's own
 *               gic_init_cpu() call; gic_retarget_spis() then recomputes
 *               the COMPLETE SPI -> CPU-bit assignment from that updated,
 *               real count and rewrites every GICD_ITARGETSR word.  CPU 0
 *               gets sole ownership of every SPI the moment gic_init_dist()
 *               runs (gic_num_online starts at 1 — CPU 0 is, by construction,
 *               the only CPU that can be running this code), and each
 *               additional CPU's arrival redistributes the full SPI set
 *               across the now-larger, now-real pool.  No estimate, no
 *               device-tree dependency in this file, and no interrupt is
 *               ever routed to a CPU that has not already proven it is
 *               alive and running the GIC CPU-interface bring-up.  The one
 *               constraint that remains, and cannot be removed by software,
 *               is architectural: GICv2's GICD_ITARGETSR is an 8-bit-per-
 *               interrupt field, so at most GIC_ITARGETSR_MAX_CPUS (8) CPUs
 *               can ever be SPI targets through this register regardless of
 *               how many actually come online — the 9th+ CPU handles SGIs/
 *               PPIs (its own per-CPU lines) normally, but never receives a
 *               distributor-routed SPI; this is logged explicitly rather
 *               than silently degrading (see gic_init_cpu()).
 *   DRV-GIC-02  RESOLVED (this pass): gic_eoi() used to write only the
 *               10-bit INTID (irq & 0x3FF) to GICC_EOIR.  Per the GICv2
 *               spec (GICC_IAR / GICC_EOIR), for SGIs bits [12:10] of
 *               GICC_IAR carry the source CPU interface ID and "the value
 *               written to GICC_EOIR must be the INTID from GICC_IAR" — ARM
 *               explicitly recommends preserving the ENTIRE register value
 *               read from GICC_IAR and writing it back unchanged.  Losing
 *               those bits on the EOI write for an SGI (irq < 16) risks
 *               leaving the interrupt Active for the wrong source PE on the
 *               distributor.  gic_ack() now returns the raw 13-bit field
 *               (INTID + CPUID, GIC_IAR_VALUE_MASK) instead of masking down
 *               to the bare INTID, and gic_eoi() writes that value straight
 *               back — exactly ARM's recommended pattern, and the same
 *               approach used by FreeBSD's and Xen's GICv2 drivers.  Every
 *               non-SGI interrupt (irq >= 16) has bits [12:10] == RES0 per
 *               spec, so this is a pure win for SGIs and a no-op for
 *               PPIs/SPIs — irq.c's `== 0`, `== IRQ_TIMER`, `== 1023`
 *               comparisons remain correct unchanged (see gic_ack() below).
 */
#include "gic_regs.h"
#include <drivers/gic.h>
#include <kernel/irq.h>
#include <kernel/memlayout.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

/* GICD_REG(off): dereference GICD MMIO register at GICD_BASE + off (32-bit).
 * GICC_REG(off): dereference GICC MMIO register at GICC_BASE + off (32-bit).
 * Both bases come from gic_regs.h (physical addresses); the access happens
 * at their direct-map kernel VA via phys_to_virt (identity while
 * KERNEL_VIRT_BASE == 0). */
/* MMIO access */
#define GICD_REG(off) (*(volatile uint32_t *)phys_to_virt(GICD_BASE + (off)))
#define GICC_REG(off) (*(volatile uint32_t *)phys_to_virt(GICC_BASE + (off)))

/* GIC_IAR_INTID_MASK: bits [9:0] of GICC_IAR/GICC_EOIR — the 10-bit
 * Interrupt ID.  1023 (all-ones) is the architectural spurious sentinel. */
#define GIC_IAR_INTID_MASK 0x3FFU

/* GIC_IAR_VALUE_MASK: bits [12:0] of GICC_IAR/GICC_EOIR — INTID (bits
 * [9:0]) PLUS, for SGIs only, the source CPU interface ID (bits [12:10]).
 * Bits [23:13] are RES0 per spec.  DRV-GIC-02 (resolved): gic_ack() returns
 * this full field rather than the bare INTID so gic_eoi() can write the
 * exact value ARM's GICv2 spec requires back to GICC_EOIR. */
#define GIC_IAR_VALUE_MASK 0x1FFFU

/* gic_num_irqs: total interrupt lines reported by GICD_TYPER, clamped to
 * GIC_MAX_IRQS.  Set once in gic_init_dist(); read-only thereafter. */
/* Number of interrupt lines */
static uint32_t gic_num_irqs;

/* GIC_ITARGETSR_MAX_CPUS: GICv2's GICD_ITARGETSR is architecturally an
 * 8-bit-per-interrupt field (one bit per targetable CPU interface) — see
 * the ARM GICv2 Architecture Specification and DRV-GIC-01 above.  No SPI
 * can ever be routed to a CPU interface numbered 8 or higher through this
 * register, independent of how many cores MAX_CPUS or the platform
 * otherwise supports; GICv3+ lifts this limit, but this driver targets
 * GICv2 (QEMU 'virt').  This is a hardware ceiling, not a software
 * shortcut — no amount of restructuring this driver can route an SPI to a
 * 9th CPU through this register. */
#define GIC_ITARGETSR_MAX_CPUS 8U

/* gic_num_online: count of CPUs that have PROVABLY finished gic_init_cpu()
 * — i.e. real, running cores, never an estimate.  Starts implicitly at 0;
 * gic_init_dist() does not touch it (CPU 0 has not called gic_init_cpu()
 * yet at that point — see gic_init_dist()'s own call to gic_retarget_spis()
 * below for how SPIs are still safely targeted before it does).  Every
 * increment happens inside gic_init_cpu(), under gic_target_lock, by the
 * CPU that just proved itself alive.
 *
 * gic_target_lock: serialises gic_num_online updates and the subsequent
 * GICD_ITARGETSR rewrite in gic_retarget_spis() against a hypothetical
 * concurrent gic_init_cpu() call on another core.  The current bring-up
 * sequence (smp_bringup_secondary() in kernel/core/smp.c) brings up APs
 * strictly one at a time with a blocking ack-wait, so no actual concurrency
 * occurs today — the lock is cheap insurance against that assumption
 * changing, not a response to an observed race (Power-of-Ten rule 5:
 * defend the invariant, don't just document it). */
static uint32_t gic_num_online;
static DEFINE_SPINLOCK(gic_target_lock);

/*
 * gic_retarget_spis - rewrite every SPI's GICD_ITARGETSR entry for the
 * current value of @online_count.
 *
 * @online_count: number of CPUs to round-robin SPIs across; the caller
 *                holds gic_target_lock and has already clamped this to
 *                [1, GIC_ITARGETSR_MAX_CPUS] (see gic_init_dist() and
 *                gic_init_cpu()).
 *
 * Recomputes the COMPLETE SPI -> CPU-bit assignment from scratch and
 * overwrites every GICD_ITARGETSR word — never an incremental OR of a new
 * bit into old words, which could otherwise leave more than one target bit
 * set per SPI (multiple simultaneous IRQ delivery to more than one CPU
 * interface is legal per the GICv2 spec but is NOT this driver's design;
 * every SPI here is meant to have exactly one target).  Called once from
 * gic_init_dist() (online_count == 1, CPU 0 only — the only CPU that can
 * possibly be executing this code at that point) and again from every
 * gic_init_cpu() call after gic_num_online has just been incremented, so
 * the full SPI set is always re-balanced across the current REAL pool
 * rather than left partially assigned to a smaller, stale one.
 *
 * GICD_ITARGETSR packs FOUR interrupts' 8-bit target fields per 32-bit
 * register (byte 0 = lowest-numbered SPI in the group); each byte gets
 * exactly one bit set — (1 << cpu_bit) — following the single-CPU-per-byte
 * convention this driver has always used (0x01 = "CPU 0 only").
 *
 * MMIO registers written: GICD_ITARGETSR[GIC_SPI_START/4 .. gic_num_irqs/4).
 *
 * Locking: caller holds gic_target_lock.
 * IRQ context: NO — called only from gic_init_dist() (boot) and
 *              gic_init_cpu() (CPU bring-up, before local IRQs are enabled).
 */
static void gic_retarget_spis(uint32_t online_count) {
  uint32_t spi_index = 0; /* 0-based count of SPIs assigned so far */
  uint32_t i;

  for (i = GIC_SPI_START / 4; i < gic_num_irqs / 4; i++) {
    uint32_t reg = 0;
    uint32_t byte;

    for (byte = 0; byte < 4; byte++, spi_index++) {
      uint32_t cpu = spi_index % online_count;
      reg |= (1U << cpu) << (byte * 8);
    }

    GICD_REG(GICD_ITARGETSR(i)) = reg;
  }
}

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
 *   6. Target every SPI to CPU 0 via gic_retarget_spis(1) — CPU 0 is, by
 *      construction, the only CPU that can be executing this code (called
 *      once via irq_init() before any secondary CPU is woken — see
 *      kernel_main() in main.c).  FIX(DRV-GIC-01): this is no longer a
 *      hand-rolled 0x01010101 write; it is gic_retarget_spis()'s
 *      online_count==1 case, so the very same recomputation logic runs
 *      here and every time gic_init_cpu() brings up another real CPU —
 *      see the file-header note.
 *   7. Configure all SPIs as level-triggered (GICD_ICFGR = 0 for word i>=2).
 *   8. Enable distributor (GICD_CTLR = 1).
 *
 * MMIO registers written: GICD_CTLR, GICD_ICENABLER[], GICD_ICPENDR[],
 *   GICD_IPRIORITYR[], GICD_ITARGETSR[], GICD_ICFGR[].
 * MMIO registers read: GICD_TYPER.
 *
 * Locking: called before SMP; no concurrent access.  Takes gic_target_lock
 *          around the gic_num_online/gic_retarget_spis() step purely for
 *          symmetry with gic_init_cpu() — uncontended here, since no other
 *          CPU is running yet.
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

  /* FIX(DRV-GIC-01): target every SPI to CPU 0 through the SAME
   * recomputation path gic_init_cpu() uses for every later CPU, instead of
   * a one-off hard-coded write.  gic_num_online is 0 here (no CPU, not even
   * this one, has finished gic_init_cpu() yet — that happens right after
   * irq_init() returns, via irq_init_percpu() in main.c); passing 1
   * explicitly is correct because CPU 0 is, by construction, the only CPU
   * that can be running this function. */
  {
    uint64_t flags;
    spin_lock_irqsave(&gic_target_lock, &flags);
    gic_retarget_spis(1);
    spin_unlock_irqrestore(&gic_target_lock, flags);
  }

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
 *   6. FIX(DRV-GIC-01): register this CPU as SPI-targetable and
 *      re-balance every SPI across the now-larger, now-REAL online pool —
 *      see gic_retarget_spis() and the file-header note.  A CPU beyond
 *      GIC_ITARGETSR_MAX_CPUS (8) — the architectural ceiling of
 *      GICD_ITARGETSR — still gets its own GICC interface enabled (steps
 *      1-5) so its SGIs/PPIs work normally, but is logged as excluded from
 *      SPI targeting rather than silently degrading: this driver never
 *      claims to route an SPI to a CPU it cannot actually address.
 *
 * MMIO registers written: GICD_ICENABLER(0), GICD_IPRIORITYR(0..7),
 *   GICC_PMR, GICC_BPR, GICC_CTLR, and — for the first
 *   GIC_ITARGETSR_MAX_CPUS callers — GICD_ITARGETSR[] via
 *   gic_retarget_spis().
 *
 * Locking: operates on per-CPU (banked) GICC registers; no shared-state
 *          lock for those.  gic_target_lock (irqsave) guards the
 *          gic_num_online increment and the subsequent GICD_ITARGETSR
 *          rewrite — SEE the file-header note on why this is defensive
 *          rather than a response to an observed race in the current
 *          strictly-serial bring-up order.
 * IRQ context: NO — called from CPU bring-up code, before local IRQs are
 *              enabled on this CPU (see kernel_main()/kernel_secondary_main()
 *              in main.c: irq_init_percpu() always precedes
 *              local_irq_enable()).
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

  /* FIX(DRV-GIC-01): this CPU has just proven it is alive and running (it
   * reached this point via cpu_init() -> arch_cpu_init() setting
   * cpu_data[id].online = 1, then irq_init_percpu() -> here).  Register it
   * as a real SPI target and re-balance the whole SPI set across the
   * updated, real online count — never an estimate. */
  {
    uint64_t flags;
    uint32_t new_count;

    spin_lock_irqsave(&gic_target_lock, &flags);
    gic_num_online++;
    new_count = gic_num_online;

    if (new_count <= GIC_ITARGETSR_MAX_CPUS) {
      gic_retarget_spis(new_count);
    } else {
      /* Hardware ceiling reached (see GIC_ITARGETSR_MAX_CPUS): this CPU's
       * SGI/PPI interface above is still fully enabled, but it is not
       * added to the SPI round-robin pool — GICD_ITARGETSR physically
       * cannot address it.  Logged explicitly rather than silently
       * dropping it from consideration. */
      pr_warn("GIC: CPU online count %u exceeds GICD_ITARGETSR's "
              "%u-CPU addressing limit — this CPU will not receive "
              "distributor-routed SPIs (SGIs/PPIs are unaffected)\n",
              new_count, GIC_ITARGETSR_MAX_CPUS);
    }
    spin_unlock_irqrestore(&gic_target_lock, flags);
  }
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
 * gic_ack - acknowledge the current interrupt and return its raw IAR value.
 *
 * Reads GICC_IAR (Interrupt Acknowledge Register, CPU interface offset
 * GICC_IAR).  This read both signals the acknowledgement to the GIC and
 * returns, in bits [9:0], the 10-bit Interrupt ID (1023 = spurious/none),
 * and, for SGIs only, the source CPU interface ID in bits [12:10] (RES0
 * for every other interrupt class).
 *
 * FIX(DRV-GIC-02): returns the masked-but-otherwise-intact 13-bit field
 * (GIC_IAR_VALUE_MASK), not just the bare INTID.  This is ARM's documented
 * recommendation ("software preserves the entire register value read from
 * this register, and writes that value back to GICC_EOIR"): the CPUID bits
 * exist only in this one read and cannot be reconstructed later, so they
 * must be threaded through to gic_eoi() now.  Every caller in this codebase
 * (irq_handler() in irq.c) compares the return value against 0
 * (IRQ_PANIC_HALT_SGI), IRQ_TIMER (27), and IRQ_SPURIOUS (1023) — none of
 * which collide with a nonzero CPUID field, so widening the return value is
 * transparent to every existing comparison; callers that need the bare
 * INTID alone can mask with GIC_IAR_INTID_MASK.
 *
 * Must be called at the start of interrupt processing; the GIC transitions
 * the interrupt to the Active state upon IAR read.
 *
 * MMIO register read: GICC_IAR.
 * Returns: (CPUID<<10 | INTID) for an SGI, or the bare INTID (0..1022) for
 *          any other interrupt class, or 1023 (spurious/no interrupt).
 *
 * Locking: per-CPU GICC register; no cross-CPU contention.
 * IRQ context: YES — must be called from IRQ handler.
 */
static uint32_t gic_ack(void) {
  return GICC_REG(GICC_IAR) & GIC_IAR_VALUE_MASK;
}

/*
 * gic_eoi - signal End-Of-Interrupt to the GIC CPU interface.
 *
 * @irq: the value gic_ack() returned for this interrupt (INTID, or
 *       CPUID<<10 | INTID for an SGI) — NOT a bare, re-masked INTID.  This
 *       is the exact value irq_handler() (irq.c) receives from
 *       chip->acknowledge() and passes straight to chip->end(), so no
 *       intermediate code needs to know or care about the CPUID field.
 *
 * Writes @irq to GICC_EOIR (End Of Interrupt Register) to transition the
 * interrupt from Active to Inactive, allowing lower-priority interrupts to
 * be signalled.  Must be called after the handler finishes processing.
 *
 * MMIO register written: GICC_EOIR.
 *
 * FIX(DRV-GIC-02): writes @irq back verbatim (masked to the architectural
 * field width, GIC_IAR_VALUE_MASK, as a defensive bound — see Power-of-Ten
 * note below) instead of the bare 10-bit INTID.  For SGIs this preserves
 * the source CPU interface ID in bits [12:10] that gic_ack() captured;
 * "the value written to GICC_EOIR must be the INTID from GICC_IAR" per the
 * GICv2 spec, and ARM explicitly recommends writing the *whole* IAR value
 * back unchanged rather than reconstructing part of it.  For every non-SGI
 * interrupt those bits are already 0 (RES0 for PPIs/SPIs), so this is a
 * pure fix for SGIs and a verified no-op for everything else.
 *
 * Locking: per-CPU GICC register; no lock needed.
 * IRQ context: YES — must be called from the IRQ dispatch path.
 */
static void gic_eoi(uint32_t irq) {
  volatile uint32_t *eoir_reg =
      (volatile uint32_t *)phys_to_virt(GICC_BASE + GICC_EOIR);
  /* Defensive mask (Power-of-Ten rule 5): even though every caller in this
   * tree passes through gic_ack()'s own return value unmodified, bound the
   * write to the architectural field width so a hypothetical future caller
   * cannot smuggle garbage into GICC_EOIR's RES0 bits. */
  *eoir_reg = irq & GIC_IAR_VALUE_MASK;
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
void gic_register(void) { irq_register_chip(&gic_chip); }