/*
 * kernel/irq/irq.c
 * Generic IRQ Framework
 *
 * Provides a chip-abstraction layer (struct irq_chip) and a 256-entry handler
 * table that sits between the hardware interrupt controller and device
 * drivers.  Two dispatch entry-points exist for the two supported arches:
 *
 *   irq_handler()  -- called from the aarch64 exception vector; drives the
 *                     acknowledge/dispatch/EOI loop via current_chip ops
 *                     (GICv2: gic.c).
 *   irq_dispatch() -- called directly from the amd64 IDT common handler
 *                     (idt.c) with the vector already known from the trap
 *                     frame; EOI is issued by the IDT handler via
 *                     irq_chip_end() AFTER this call returns.
 *
 * Invariants:
 *   - current_chip must be set via irq_register_chip() before irq_init() is
 *     called.  Every chip op is optional and NULL-checked before use, so a
 *     chip that leaves an op unset (e.g. pic_chip->init == NULL) degrades
 *     to a no-op rather than a crash.
 *   - irq_handlers[] entries are written by irq_register()/irq_unregister()
 *     only; they are read from IRQ context in irq_handler()/irq_dispatch().
 *     irq_table_lock (IRQ-02) makes every read/write of a slot atomic with
 *     respect to the other CPUs.
 *   - The scheduler tick source (IRQ_TIMER, PPI 27 on aarch64 — see
 *     platform.h and kernel/drivers/timer/timer.c) is handled INLINE in
 *     irq_handler() and bypasses irq_handlers[] entirely: the fixed-function
 *     scheduler path must not go through a registerable, unregisterable
 *     table.  amd64's periodic tick is a different vector (32, LAPIC) and is
 *     dispatched by idt.c directly to kernel_timer_tick(), never through
 *     this file — irq_handler() is aarch64-only.
 *
 * Known issues:
 *   IRQ-01  RESOLVED (Phase A step 15, by contract redefinition): the two
 *           dispatch models are now explicit.  aarch64 enters through
 *           irq_handler() (GIC acknowledge-loop, vector from GICC_IAR);
 *           amd64 enters through irq_dispatch() (vector already known from
 *           the IDT stub — an IAR-style acknowledge() makes no sense on x86,
 *           exactly as in other kernels' vectored dispatch).  What WAS broken
 *           is fixed: EOI on amd64 no longer bypasses the chip — the IDT
 *           handler calls irq_chip_end(), and pic_chip->end() owns the full
 *           LAPIC+PIC EOI sequence.  pic_chip_acknowledge() keeps returning
 *           1023 so a stray irq_handler() call on amd64 stays a no-op.
 *   IRQ-02  RESOLVED (Phase A step 15): irq_table_lock protects
 *           irq_handlers[]; dispatch paths copy the (handler, data) pair
 *           under the lock and invoke the handler outside it, so a concurrent
 *           irq_unregister can no longer produce a torn pair or a stale
 *           pointer dereference.
 *   IRQ-03  RESOLVED (this pass): irq_handler()'s timer fast-path matched
 *           `irq == IRQ_TIMER || irq == 30` — i.e. both PPI 27 (IRQ_TIMER,
 *           the EL1 *virtual* timer this kernel actually arms; see
 *           timer_init()/timer_init_percpu() in kernel/drivers/timer/timer.c)
 *           AND the literal 30 (PPI 30, the EL1 *physical* timer).  Nothing
 *           in this tree ever enables PPI 30 — irq_enable() is called only
 *           for IRQ_TIMER — so the second arm was unreachable in normal
 *           operation, but it was live code with real consequences: a
 *           misrouted or spoofed ID 30 (GIC config error, future physical-
 *           timer user, malicious SGI-range confusion) would silently be
 *           handed to the scheduler tick path instead of falling through to
 *           the "unhandled IRQ, mask the line" default, hiding exactly the
 *           class of misconfiguration this framework exists to surface. The
 *           stale cross-reference this masked ("handled explicitly in
 *           gic.c") is also gone — gic.c has never special-cased any IRQ
 *           number; the special-casing has only ever lived here. Fixed by
 *           matching IRQ_TIMER alone; PPI 30 now falls through to the
 *           normal registered-handler / unhandled-IRQ path like any other
 *           line, and will light up correctly the day something legitimately
 *           registers a handler for it.
 *   DRV-GIC-02  RESOLVED (companion fix, gic.c, this pass): gic_ack() now
 *           returns an SGI's full raw GICC_IAR value (INTID + source-CPU
 *           bits) instead of a bare INTID, because the GICv2 spec requires
 *           writing that whole value back to GICC_EOIR.  irq_handler()
 *           below is the code that must keep that raw value distinct from
 *           the bare INTID used for dispatch — see IRQ_INTID_MASK.
 *
 * NASA/JPL "Power of Ten" compliance notes for this file:
 *   Rule 1  (simple control flow): no goto, no recursion; irq_handler()'s
 *           acknowledge loop is the only loop and is bounded (rule 2).
 *   Rule 2  (bounded loops): the GIC acknowledge loop in irq_handler() is
 *           bounded by IRQ_ACK_LOOP_MAX, a static upper bound on how many
 *           interrupts one entry can legitimately drain in one physical
 *           IRQ exception (see IRQ_ACK_LOOP_MAX below) — it cannot spin
 *           forever even if a chip bug or hostile device keeps posting
 *           "valid" IDs and never returns the spurious sentinel.
 *   Rule 3  (no dynamic memory after init): this file performs no heap
 *           allocation at any time; irq_handlers[] is static storage.
 *   Rule 4  (short functions): every function here fits on one screen and
 *           has a single, named responsibility; the "look up handler, call
 *           it or warn+mask" sequence common to both dispatchers is
 *           factored into irq_run_registered_or_warn() /
 *           irq_lookup_and_clear_pending() to keep irq_handler() and
 *           irq_dispatch() themselves short and their control flow flat.
 *   Rule 5  (assertion / defensive checks): every table access is bounds-
 *           checked (irq < MAX_IRQS) before use; every chip-op call is
 *           NULL-checked; irq_register()/irq_unregister() reject
 *           out-of-range IRQ numbers explicitly rather than trusting the
 *           caller.
 *   Rule 6  (minimal scope): irq_handlers[] and irq_table_lock are file-
 *           static; current_chip is file-static; no global mutable state is
 *           exported from this file except through the declared API.
 *   Rule 7  (check return values): irq_register()'s -EINVAL/-EBUSY are
 *           NX_MUST_USE at the declaration (irq.h) and every internal call
 *           in this file that can fail is checked.
 *   Rule 8  (limited preprocessor use): only header guards and the existing
 *           DEBUG_IRQ conditional; no macro hides control flow.
 *   Rule 9  (restricted pointer use): irq_handler_t is used purely as a
 *           direct function pointer, one level of indirection, invoked
 *           through a local snapshot copied out of the locked table — never
 *           a pointer to a pointer, never arithmetic on it.
 *   Rule 10 (compile clean): builds warning-clean under -Wall -Wextra
 *           -Wmissing-prototypes; every non-static entry point is declared
 *           in irq.h.
 */
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

#define MAX_IRQS 256

/* IRQ_ACK_LOOP_MAX: hard upper bound on interrupts drained per entry to
 * irq_handler() (NASA Power-of-Ten rule 2 — every loop needs a fixed bound).
 * MAX_IRQS is already a generous ceiling (no real GIC configuration on this
 * platform reports more than MAX_IRQS lines — see GIC_MAX_IRQS in
 * gic_regs.h, which matches this value); one full sweep of the ID space is
 * more iterations than the GIC can legitimately hand back as distinct
 * *pending* IDs inside a single exception. Hitting the bound means the chip
 * is misbehaving (never returning the spurious sentinel) rather than that
 * legitimate interrupt traffic was cut short. */
#define IRQ_ACK_LOOP_MAX MAX_IRQS

/* GIC "no more interrupts pending" sentinel returned by chip->acknowledge().
 * Named here (rather than left as a bare 1023) so the two call sites agree
 * on the value by construction; matches GIC_SPURIOUS_IRQ in gic_regs.h,
 * which is architectural GICv2 encoding (10-bit interrupt ID field, all-ones
 * = 1023 = "spurious"), not this file's own choice. */
#define IRQ_SPURIOUS 1023U

/* SGI0: the fixed software-generated interrupt ID this kernel reserves for
 * the panic-halt broadcast (irq_send_ipi_all() / gic_send_ipi()). aarch64
 * counterpart of amd64's HALT_IPI_VECTOR (pic_pit.c). */
#define IRQ_PANIC_HALT_SGI 0U

/* IRQ_INTID_MASK: bits [9:0] — the bare 10-bit interrupt ID, with any
 * chip-specific out-of-band bits stripped off.
 *
 * Companion to gic_ack()'s DRV-GIC-02 fix (gic.c): on aarch64,
 * chip->acknowledge() now returns the FULL GICC_IAR value for an SGI —
 * INTID in bits [9:0] plus the source CPU interface ID in bits [12:10] —
 * because that whole value must be written back unchanged to GICC_EOIR
 * (chip->end()).  Every comparison and every irq_handlers[] table lookup in
 * this file, however, must operate on the bare INTID alone: an SGI's raw
 * IAR value can be as large as (7<<10)|15, far past MAX_IRQS, and would
 * silently miss its table slot (or, worse, alias a different one) if used
 * as an index directly.  irq_handler() therefore keeps two values per
 * iteration — the raw acked value (for chip->end()) and the masked INTID
 * (for every `==` comparison and every table lookup) — and this mask is
 * what derives the second from the first.  amd64's irq_dispatch() needs no
 * such split: the vector arrives already bare from the IDT stub. */
#define IRQ_INTID_MASK 0x3FFU

/* irq_handlers[]: sparse table mapping IRQ number -> (handler, opaque data).
 * Entries are set by irq_register() and cleared by irq_unregister().
 * FIX(IRQ-02): guarded by irq_table_lock; dispatchers copy the pair under
 * the lock and call the handler after dropping it (a handler may re-register
 * or disable lines without self-deadlocking). */
static struct {
  irq_handler_t handler;
  void *data;
} irq_handlers[MAX_IRQS];

/* irq_table_lock: protects irq_handlers[] against concurrent
 * register/unregister/dispatch across CPUs (IRQ-02). */
static DEFINE_SPINLOCK(irq_table_lock);

/* current_chip: pointer to the active irq_chip implementation.
 * Set once at boot via irq_register_chip(); never changed afterwards.
 * On aarch64: points to gic_chip (gic.c).
 * On amd64:   points to pic_chip (pic_pit.c) — but see IRQ-01 (resolved). */
static struct irq_chip *current_chip = NULL;

/*
 * irq_register_chip - register the platform interrupt controller.
 *
 * Must be called before irq_init().  Stores the chip pointer in current_chip
 * and logs its name.  Not thread-safe (called once from boot path only).
 *
 * Locking: none required; called before SMP is active.
 */
void irq_register_chip(struct irq_chip *chip) {
  current_chip = chip;
  pr_info("IRQ: Registered chip %s\n", chip->name);
}

/*
 * irq_init - call the chip's global (distributor) initialisation hook.
 *
 * Delegates to current_chip->init() which, for GICv2, initialises the
 * GICD distributor, disables all interrupts, clears pending state, and sets
 * target/priority registers.  For the 8259 PIC chip init is NULL (pic_init()
 * is called directly from the amd64 HAL).
 *
 * Locking: called from boot CPU before SMP; no concurrent access.
 */
void irq_init(void) {
  if (current_chip && current_chip->init) {
    current_chip->init();
  }
}

/*
 * irq_init_percpu - call the chip's per-CPU initialisation hook.
 *
 * For GICv2, initialises the GICC CPU interface on the calling core:
 * sets priority mask (GICC_PMR = 0xFF), binary point (GICC_BPR = 0), and
 * enables the CPU interface (GICC_CTLR = 1).  Called on every CPU at SMP
 * bring-up via the boot sequence.
 *
 * Locking: operates only on per-CPU MMIO registers; no shared state.
 */
void irq_init_percpu(void) {
  if (current_chip && current_chip->init_percpu) {
    current_chip->init_percpu();
  }
}

/*
 * irq_register - bind a handler to an IRQ line and enable it.
 *
 * @irq:     interrupt number (0..MAX_IRQS-1).
 * @handler: function called from IRQ context with (irq, data).
 * @data:    opaque pointer passed to handler; may be NULL.
 *
 * Returns 0 on success, -EINVAL if irq >= MAX_IRQS or handler == NULL,
 * -EBUSY if already registered.  After storing the handler, calls
 * chip->enable(irq) to unmask the line in the interrupt controller.
 *
 * Locking: irq_table_lock (irqsave) for the table slot; the chip->enable()
 *          MMIO write happens after the slot is visible, so the line is
 *          never unmasked with an empty handler entry.
 * IRQ context: must NOT be called from an IRQ handler.
 */
int irq_register(uint32_t irq, irq_handler_t handler, void *data) {
  if (irq >= MAX_IRQS || handler == NULL)
    return -EINVAL;

  uint64_t flags;
  spin_lock_irqsave(&irq_table_lock, &flags);

  if (irq_handlers[irq].handler) {
    spin_unlock_irqrestore(&irq_table_lock, flags);
    return -EBUSY;
  }

  irq_handlers[irq].handler = handler;
  irq_handlers[irq].data = data;

  spin_unlock_irqrestore(&irq_table_lock, flags);

  if (current_chip && current_chip->enable) {
    current_chip->enable(irq);
  }

  return 0;
}

/*
 * irq_unregister - disable an IRQ line and remove its handler.
 *
 * @irq: interrupt number to deregister.
 *
 * Calls chip->disable(irq) first to mask the line at the controller, then
 * clears the handler and data pointers in irq_handlers[].  Silently returns
 * if irq >= MAX_IRQS.
 *
 * Locking: irq_table_lock (irqsave) for the table slot.  A dispatch that
 *          already copied the pair may still complete one final handler call
 *          after unregister returns (no synchronize_irq yet); the line is
 *          masked first, so no NEW interrupts dispatch the stale entry.
 * IRQ context: must NOT be called from an IRQ handler.
 */
void irq_unregister(uint32_t irq) {
  if (irq >= MAX_IRQS)
    return;

  if (current_chip && current_chip->disable) {
    current_chip->disable(irq);
  }

  uint64_t flags;
  spin_lock_irqsave(&irq_table_lock, &flags);
  irq_handlers[irq].handler = NULL;
  irq_handlers[irq].data = NULL;
  spin_unlock_irqrestore(&irq_table_lock, flags);
}

/*
 * irq_enable - unmask a single IRQ line at the interrupt controller.
 *
 * @irq: interrupt number to enable.
 *
 * Thin wrapper around chip->enable(); no handler registration side-effect.
 * Used by timer_init() to enable the ARM timer IRQ independently of
 * irq_register().
 *
 * Locking: chip->enable() on GICv2 writes GICD_ISENABLER; safe to call
 *          from any context as long as the MMIO window is valid.
 */
void irq_enable(uint32_t irq) {
  if (current_chip && current_chip->enable) {
    current_chip->enable(irq);
  }
}

/*
 * irq_disable - mask a single IRQ line at the interrupt controller.
 *
 * @irq: interrupt number to disable.
 *
 * Thin wrapper around chip->disable().  Called from irq_handler() to silence
 * an unhandled IRQ and prevent an interrupt storm.
 *
 * Locking: same as irq_enable; MMIO write is atomic at the register level.
 */
void irq_disable(uint32_t irq) {
  if (current_chip && current_chip->disable) {
    current_chip->disable(irq);
  }
}

/*
 * irq_send_ipi_all - broadcast the panic-halt IPI to all other CPUs.
 *
 * Delegates to chip->send_ipi_all(); for GICv2, writes GICD_SGIR with
 * TargetListFilter=0b01 (all CPUs except requestor) and SGI ID
 * IRQ_PANIC_HALT_SGI (0). SGI0 is treated by irq_handler() as a panic-halt
 * IPI; the amd64 chip (pic_chip) uses a fixed LAPIC vector for the same
 * purpose (HALT_IPI_VECTOR, pic_pit.c) — same contract, arch-appropriate
 * mechanism.
 *
 * Locking: no locks; the chip's own IPI-send register write is
 *          self-contained.
 * IRQ context: may be called from any context.
 */
void irq_send_ipi_all(void) {
  if (current_chip && current_chip->send_ipi_all) {
    current_chip->send_ipi_all();
  }
}

/*
 * cpu_halt_from_ipi - halt this CPU after receiving a panic IPI (SGI0).
 *
 * Sets the global panic_flag, stops the arch timer (prevents further timer
 * IRQs on this core), and calls arch_cpu_halt() to enter an infinite WFI
 * loop.  Called only from irq_handler() when irq == IRQ_PANIC_HALT_SGI.
 * Mirrors halt_ipi_handler() on the amd64/PIC path (pic_pit.c) — same
 * contract, one per arch because each arch's IPI delivery is different
 * (GIC SGI acknowledge-loop entry here vs. a plain registered LAPIC-vector
 * handler there).
 *
 * Locking: panic_flag is a volatile int; no spinlock; intentionally racy
 *          since all CPUs are being halted simultaneously and there is no
 *          "after" state to protect.
 * IRQ context: called from IRQ handler after chip->end(IRQ_PANIC_HALT_SGI);
 *              IRQs are effectively disabled by arch_cpu_halt() thereafter.
 */
static void cpu_halt_from_ipi(void) {
  extern volatile int panic_flag;
  panic_flag = 1;
  arch_timer_control(0);
  arch_cpu_halt(); /* noreturn in practice: infinite WFI/HLT loop */
}

/*
 * irq_lookup_and_clear_pending - snapshot the (handler, data) pair for @irq.
 *
 * @irq: interrupt number; must satisfy irq < MAX_IRQS (checked by caller —
 *       see the callers' `if (irq < MAX_IRQS)` guard; the function is
 *       otherwise defensive and simply reports "no handler" for an
 *       out-of-range id rather than trusting the caller unconditionally).
 * @out_handler: receives the registered handler, or NULL if none/OOB.
 * @out_data:    receives the registered opaque data pointer.
 *
 * FIX(IRQ-02), factored out: both dispatch paths (irq_handler(),
 * irq_dispatch()) need the identical "copy the pair under the lock, use it
 * after dropping the lock" sequence.  A single implementation means the two
 * dispatchers cannot drift out of sync on the locking discipline that makes
 * concurrent irq_unregister() safe.
 *
 * Locking: acquires/releases irq_table_lock internally (not irqsave — the
 *          caller is already executing with IRQs masked at exception
 *          entry, so an irqsave/irqrestore pair here would be redundant
 *          work on every single interrupt, aarch64 and amd64 alike).
 * IRQ context: YES — this is on the hot dispatch path for every device IRQ.
 */
static inline void irq_lookup_and_clear_pending(uint32_t irq,
                                                 irq_handler_t *out_handler,
                                                 void **out_data) {
  *out_handler = NULL;
  *out_data = NULL;

  if (irq >= MAX_IRQS)
    return;

  spin_lock(&irq_table_lock);
  *out_handler = irq_handlers[irq].handler;
  *out_data = irq_handlers[irq].data;
  spin_unlock(&irq_table_lock);
}

/*
 * irq_run_registered_or_warn - invoke @irq's handler if one is registered.
 *
 * @irq: interrupt number, already acknowledged at the controller.
 *
 * Looks up (handler, data) via irq_lookup_and_clear_pending() and calls the
 * handler outside the lock.  If no handler is registered, logs a warning
 * and masks the line at the controller (irq_disable()) to prevent an
 * interrupt storm from a device nobody is servicing — a stuck/unmasked line
 * with no consumer would otherwise refire indefinitely and starve every
 * other interrupt on the same priority tier.
 *
 * Shared by irq_handler() (aarch64) and irq_dispatch() (amd64) so the
 * "unhandled IRQ" policy — log once, then mask — is defined in exactly one
 * place rather than duplicated per arch.
 *
 * Locking: see irq_lookup_and_clear_pending(); irq_disable() itself takes
 *          no lock (a single MMIO/port write).
 * IRQ context: YES.
 */
static inline void irq_run_registered_or_warn(uint32_t irq) {
  irq_handler_t handler;
  void *data;

  irq_lookup_and_clear_pending(irq, &handler, &data);

  if (handler) {
    handler(irq, data);
  } else {
    pr_warn("IRQ: Unhandled interrupt %u\n", irq);
    irq_disable(irq); /* Prevent interrupt storm */
  }
}

/*
 * irq_handler - main IRQ dispatch loop for aarch64 (GIC path).
 *
 * Called from the aarch64 exception vector with the saved register state.
 * Loops calling chip->acknowledge() (GIC: reads GICC_IAR) until
 * IRQ_SPURIOUS (1023, "no more") is returned, or until IRQ_ACK_LOOP_MAX
 * iterations have run (Power-of-Ten rule 2 — see the macro comment; this
 * bound is not expected to be hit in normal operation). For each valid IRQ:
 *
 *   irq == IRQ_PANIC_HALT_SGI (0): EOI, then halt this CPU via
 *                                  cpu_halt_from_ipi() (never returns).
 *   irq == IRQ_TIMER (PPI 27):     delegate to timer_handler(regs) for
 *                                  scheduling; return immediately with
 *                                  potentially switched regs.
 *   irq < MAX_IRQS with a registered handler: call handler(irq, data), then
 *     EOI (via irq_run_registered_or_warn(), shared with irq_dispatch()).
 *   otherwise: warn and disable the line to prevent a storm.
 *
 * Returns the (potentially new) register state for the resumed context.
 *
 * NOTE(IRQ-01, resolved): this loop is the aarch64 (GIC) entry point only;
 * amd64 is vectored and enters through irq_dispatch() + irq_chip_end().
 * NOTE(IRQ-03, resolved): the timer fast-path now matches IRQ_TIMER alone —
 * see the file-header comment for why the old `|| irq == 30` arm was live
 * but unreachable, and what it was hiding.
 * FIX(IRQ-02): the (handler, data) pair is copied under irq_table_lock and
 * invoked after dropping it (irq_run_registered_or_warn()).
 * NOTE(DRV-GIC-02 companion): chip->acknowledge() may now return an SGI's
 * raw IAR value (INTID + source-CPU bits, see gic_ack() in gic.c) rather
 * than a bare INTID.  This loop keeps that raw value ONLY for the
 * chip->end() call it must be echoed back to, and derives `intid` (masked
 * with IRQ_INTID_MASK) for every comparison and table lookup — see
 * IRQ_INTID_MASK's comment for why conflating the two would be wrong.
 *
 * Locking: runs with IRQs implicitly masked (exception entry).
 * IRQ context: YES — this IS the IRQ entry point on aarch64.
 */
struct pt_regs *irq_handler(struct pt_regs *regs) {
  struct pt_regs *ret_regs = regs;
  uint32_t iterations;

  if (!current_chip)
    return regs;

  for (iterations = 0; iterations < IRQ_ACK_LOOP_MAX; iterations++) {
    /* `irq_raw`: exactly what chip->acknowledge() returned — echoed back
     * verbatim to chip->end() so any out-of-band bits it carries (the SGI
     * source-CPU field on aarch64) survive the round trip.  `intid`: the
     * bare interrupt ID — the only form used for comparisons or as a
     * irq_handlers[] table index. */
    uint32_t irq_raw = current_chip->acknowledge();
    uint32_t intid = irq_raw & IRQ_INTID_MASK;

    if (intid == IRQ_SPURIOUS) /* Spurious or no more pending interrupts */
      break;

#ifdef DEBUG_IRQ
    if (intid != IRQ_TIMER) {
      pr_info("IRQ: Handling interrupt %u\n", intid);
    }
#endif

    /* Panic-halt IPI: EOI then halt.  Does not return. */
    if (intid == IRQ_PANIC_HALT_SGI) {
      current_chip->end(irq_raw);
      cpu_halt_from_ipi();
      /* unreachable: cpu_halt_from_ipi() halts this core permanently */
    }

    /* Scheduler tick (fixed-function; bypasses irq_handlers[] by design —
     * see the file-header invariants).  FIX(IRQ-03): IRQ_TIMER only. */
    if (intid == IRQ_TIMER) {
      extern struct pt_regs *timer_handler(struct pt_regs * regs);

      ret_regs = timer_handler(ret_regs);
      current_chip->end(irq_raw);
      return ret_regs;
    }

    irq_run_registered_or_warn(intid);
    current_chip->end(irq_raw);
  }

  if (iterations == IRQ_ACK_LOOP_MAX) {
    /* The chip kept returning "valid" IDs for an entire sweep of the ID
     * space without ever reporting IRQ_SPURIOUS.  That is a controller or
     * device misbehaving, not legitimate interrupt traffic; stop draining
     * and return so the exception path can make forward progress instead
     * of holding this core in an unbounded loop with IRQs masked. */
    pr_err("IRQ: acknowledge loop hit IRQ_ACK_LOOP_MAX (%u) without "
           "IRQ_SPURIOUS — possible chip malfunction or storm\n",
           IRQ_ACK_LOOP_MAX);
  }

  return ret_regs;
}

/*
 * irq_dispatch - IRQ dispatch entry-point for amd64 (IDT path).
 *
 * @irq:  interrupt vector number derived from the IDT stub (0..255).
 * @regs: saved register state from the IDT common handler.
 *
 * Called by the amd64 IDT common handler in idt.c; the IDT handler then
 * issues the EOI through irq_chip_end() (chip-owned EOI — IRQ-01 fix).
 * Looks up irq_handlers[irq] and invokes the registered callback if present
 * (via irq_run_registered_or_warn(), shared with irq_handler()); logs a
 * warning and masks the line otherwise.
 *
 * Returns @regs unchanged: this path has no context-switch support.
 * Scheduler preemption on amd64 happens on vector 32 in idt.c directly
 * (kernel_timer_tick()), never through this function.
 *
 * NOTE(IRQ-01, resolved): acknowledge() is N/A on a vectored architecture —
 * the vector arrives with the frame.  EOI goes through the chip now.
 * FIX(IRQ-02): (handler, data) copied under irq_table_lock, called outside
 * (irq_run_registered_or_warn()).
 *
 * Locking: IRQs are masked by the CPU at IDT entry; irq_table_lock guards
 *          the table read against concurrent register/unregister.
 * IRQ context: YES — called from the amd64 IDT handler.
 */
struct pt_regs *irq_dispatch(uint32_t irq, struct pt_regs *regs) {
  irq_run_registered_or_warn(irq);
  return regs;
}

/*
 * irq_chip_end - single EOI mechanism for vectored dispatchers (IRQ-01 fix).
 *
 * Routes the end-of-interrupt through the registered chip: on amd64 the
 * pic_chip->end() implementation owns the complete LAPIC + 8259 sequence,
 * so idt.c no longer hand-rolls EOI writes behind the chip's back.
 *
 * Locking: none; chip->end() is a self-contained MMIO/port write sequence.
 * IRQ context: YES.
 */
void irq_chip_end(uint32_t irq) {
  if (current_chip && current_chip->end) {
    current_chip->end(irq);
  }
}