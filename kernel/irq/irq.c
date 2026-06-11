/*
 * kernel/irq/irq.c
 * Generic IRQ Framework
 *
 * Provides a chip-abstraction layer (struct irq_chip) and a 256-entry handler
 * table that sits between the hardware interrupt controller and device drivers.
 * Two dispatch entry-points exist for the two supported arches:
 *
 *   irq_handler()  -- called from the aarch64 exception vector; drives the
 *                     acknowledge/dispatch/EOI loop via current_chip ops.
 *   irq_dispatch() -- called directly from the amd64 IDT common handler
 *                     (idt.c); bypasses current_chip entirely; EOI is issued
 *                     by the IDT handler before or after this call.
 *
 * Invariants:
 *   - current_chip must be set via irq_register_chip() before irq_init() is
 *     called.  All chip ops are guarded by NULL checks.
 *   - irq_handlers[] entries are written by irq_register/irq_unregister only;
 *     they are read from IRQ context in irq_handler/irq_dispatch.
 *   - The timer IRQ (IRQ_TIMER / 30) is handled inline in irq_handler() and
 *     bypasses the irq_handlers[] table.
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
 */
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

#define MAX_IRQS 256

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
 * On amd64:   points to pic_chip (pic_pit.c) — but see IRQ-01. */
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
 * Returns 0 on success, -EINVAL if irq >= MAX_IRQS, -EBUSY if already
 * registered.  After storing the handler, calls chip->enable(irq) to unmask
 * the line in the interrupt controller.
 *
 * Locking: irq_table_lock (irqsave) for the table slot; the chip->enable()
 *          MMIO write happens after the slot is visible, so the line is
 *          never unmasked with an empty handler entry.
 * IRQ context: must NOT be called from an IRQ handler.
 */
int irq_register(uint32_t irq, irq_handler_t handler, void *data) {
  if (irq >= MAX_IRQS)
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
 * irq_send_ipi_all - broadcast SGI0 to all other CPUs.
 *
 * Delegates to chip->send_ipi_all(); for GICv2, writes GICD_SGIR with
 * TargetListFilter=0b01 (all CPUs except requestor) and SGI ID 0.
 * SGI0 is treated by irq_handler() as a panic-halt IPI.
 *
 * Locking: no locks; GICD_SGIR write is self-contained.
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
 * loop.  Called only from irq_handler() when irq == 0.
 *
 * Locking: panic_flag is a volatile int; no spinlock; intentionally racy
 *          since all CPUs are being halted simultaneously.
 * IRQ context: called from IRQ handler after chip->end(0); IRQs are
 *              effectively disabled by arch_cpu_halt() thereafter.
 */
/* Internal panic halt helper */
static void cpu_halt_from_ipi(void) {
  extern volatile int panic_flag;
  panic_flag = 1;
  arch_timer_control(0);
  arch_cpu_halt();
}

/*
 * irq_handler - main IRQ dispatch loop for aarch64 (GIC path).
 *
 * Called from the aarch64 exception vector with the saved register state.
 * Loops calling chip->acknowledge() (GIC: reads GICC_IAR) until 1023
 * (spurious / no more) is returned.  For each valid IRQ:
 *
 *   irq == 0    (SGI0): EOI, then halt this CPU via cpu_halt_from_ipi().
 *   irq == IRQ_TIMER or 30: delegate to timer_handler(regs) for scheduling;
 *                            return immediately with potentially switched regs.
 *   irq < MAX_IRQS with registered handler: call handler(irq, data), then EOI.
 *   otherwise: warn and disable the line to prevent storm.
 *
 * Returns the (potentially new) register state for the resumed context.
 *
 * NOTE(IRQ-01, resolved): this loop is the aarch64 (GIC) entry point only;
 * amd64 is vectored and enters through irq_dispatch() + irq_chip_end().
 * FIX(IRQ-02): the (handler, data) pair is copied under irq_table_lock and
 * invoked after dropping it.
 *
 * Locking: runs with IRQs implicitly masked (exception entry).
 * IRQ context: YES — this IS the IRQ entry point on aarch64.
 */
struct pt_regs *irq_handler(struct pt_regs *regs) {
  uint32_t irq;
  struct pt_regs *ret_regs = regs;

  if (!current_chip)
    return regs;

  while (1) {
    irq = current_chip->acknowledge();

    if (irq == 1023) /* Spurious or No more interrupts */
      break;

#ifdef DEBUG_IRQ
    if (irq != IRQ_TIMER && irq != 30) {
      pr_info("IRQ: Handling interrupt %u\n", irq);
    }
#endif

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
    }

    /* FIX(IRQ-02): snapshot the pair under the lock, call outside it. */
    irq_handler_t handler = NULL;
    void *data = NULL;
    if (irq < MAX_IRQS) {
      spin_lock(&irq_table_lock);
      handler = irq_handlers[irq].handler;
      data = irq_handlers[irq].data;
      spin_unlock(&irq_table_lock);
    }

    if (handler) {
      handler(irq, data);
    } else {
      pr_warn("IRQ: Unhandled interrupt %u\n", irq);
      irq_disable(irq); /* Prevent interrupt storm */
    }

    current_chip->end(irq);
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
 * Looks up irq_handlers[irq] and invokes the registered callback if present;
 * logs a warning otherwise.
 *
 * Returns @regs unchanged (no context-switch support on this path; scheduler
 * preemption on amd64 happens through a separate mechanism).
 *
 * NOTE(IRQ-01, resolved): acknowledge() is N/A on a vectored architecture —
 * the vector arrives with the frame.  EOI goes through the chip now.
 * FIX(IRQ-02): (handler, data) copied under irq_table_lock, called outside.
 *
 * Locking: IRQs are masked by the CPU at IDT entry; irq_table_lock guards
 *          the table read against concurrent register/unregister.
 * IRQ context: YES — called from the amd64 IDT handler.
 */
struct pt_regs *irq_dispatch(uint32_t irq, struct pt_regs *regs) {
  irq_handler_t handler = NULL;
  void *data = NULL;

  if (irq < MAX_IRQS) {
    spin_lock(&irq_table_lock);
    handler = irq_handlers[irq].handler;
    data = irq_handlers[irq].data;
    spin_unlock(&irq_table_lock);
  }

  if (handler) {
    handler(irq, data);
  } else {
    pr_warn("IRQ: Unhandled interrupt %u\n", irq);
  }
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
