/*
 * kernel/drivers/timer/timer.c
 * ARM Generic Timer Driver — aarch64 (EL1 virtual timer)
 *
 * Drives the ARMv8 generic timer using the EL1 virtual timer (CNTV_*
 * system registers).  The virtual counter (CNTVCT_EL0) is used instead of
 * the physical counter so that the driver works correctly in QEMU -kernel
 * mode where the hypervisor timer may not be accessible.
 *
 * Architecture (this file is now a thin ARM provider; the tick accounting is
 * arch-neutral in kernel/core/timer.c, driven through the HAL timer primitives):
 *   - timer_init() reads the counter frequency (CNTFRQ_EL0) into timer_freq,
 *     consumed by the arch-neutral timer_percpu_tick()/mono_ns().
 *   - timer_init_percpu() arms the EL1 virtual timer on each CPU via
 *     timer_percpu_arm() (sets CNTV_CVAL) and enables it via CNTV_CTL_EL0.
 *   - timer_handler() is called directly from irq_handler() in irq.c when
 *     IRQ_TIMER (PPI 27) fires; it calls timer_percpu_tick() (fractional drift
 *     accounting + CNTV_CVAL reprogram via arch_timer_set_compare) then
 *     kernel_timer_tick() for scheduling.
 *
 * Timer register access is via arch_ wrappers (arch_timer_get_freq,
 * arch_timer_get_count, arch_timer_set_compare, arch_timer_control) which
 * translate to MRS/MSR system register instructions.
 *
 * Invariants:
 *   - timer_freq must be non-zero before timer_handler() is called.
 *   - cpu->next_tick_target is per-CPU; updated only by timer_handler() on
 *     the owning CPU.
 *   - jiffies is global and volatile; incremented by kernel_timer_tick().
 *
 * Known issues:
 *   IRQ-03  (W1 DOC) The comment in timer_init() says "We handle IRQ 27
 *           explicitly in gic.c dispatch"; the actual special handling is in
 *           irq_handler() in irq.c, not gic.c.  Stale cross-reference.
 */
#include <kernel/irq.h>
#include <drivers/timer.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/list.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

/* Disable optimization */

#include <kernel/vmm.h>

/* timer_freq: CNTFRQ_EL0 value in ticks/second; read once at timer_init().
 * Used to compute tick intervals and by timer_get_us(). */
/* Timer frequency */
/* Timer frequency */
uint64_t timer_freq;

/* jiffies: global monotonic tick counter incremented by kernel_timer_tick()
 * on every timer IRQ on the boot CPU.  Declared volatile because it is read
 * from non-IRQ contexts without a lock. */
/* System tick counter */
volatile uint64_t jiffies = 0;

/*
 * read_cntfrq - read the ARM generic timer frequency register.
 *
 * Returns: CNTFRQ_EL0 value (ticks per second); set by firmware / QEMU.
 *
 * IRQ context: safe (system register read).
 */
/*
 * Read counter frequency
 */
static inline uint64_t read_cntfrq(void) { return arch_timer_get_freq(); }

/*
 * read_cntvct - read the EL1 virtual counter.
 *
 * Returns: current value of CNTVCT_EL0 (64-bit free-running counter).
 *
 * IRQ context: safe.
 */
/*
 * Read virtual counter
 */
static inline uint64_t read_cntvct(void) { return arch_timer_get_count(); }

/*
 * write_cntv_cval - set the EL1 virtual timer compare value.
 *
 * @val: absolute counter value at which the timer should fire next.
 *
 * Writes CNTV_CVAL_EL0.  The virtual timer fires when CNTVCT_EL0 >= val.
 * Must be called with interrupts enabled on the owning CPU.
 *
 * IRQ context: safe (system register write).
 */
/*
 * Set virtual timer compare value (EL1 virtual timer)
 */
static inline void write_cntv_cval(uint64_t val) {
  arch_timer_set_compare(val);
}

/*
 * write_cntv_ctl - set the EL1 virtual timer control register.
 *
 * @val: CNTV_CTL_EL0 value.  Bit 0 (ENABLE) = 1 to start the timer;
 *       bit 1 (IMASK) = 0 to allow the interrupt to fire.
 *
 * IRQ context: safe (system register write).
 */
/*
 * Set virtual timer control (EL1 virtual timer)
 */
static inline void write_cntv_ctl(uint64_t val) {
  arch_timer_control(val);
}

/*
 * Timer interrupt handler (Internal C handler called by GIC)
 * Now accepts regs for Preemption.
 */
extern void compositor_tick(void);

/* Global panic flag set by panic() to halt all CPUs */
extern volatile int panic_flag;

extern struct pt_regs *kernel_timer_tick(struct pt_regs *regs);

/*
 * timer_handler - ARM generic timer IRQ handler (EL1 virtual timer, PPI 27).
 *
 * @regs: saved register state from the exception entry; passed through to
 *        kernel_timer_tick() for potential context-switch use.
 *
 * Called directly from irq_handler() in irq.c when irq == IRQ_TIMER (27) or 30.
 * Delegates the precision per-CPU tick accounting + CNTV_CVAL reprogram to the
 * arch-neutral, HAL-driven timer_percpu_tick() (kernel/core/timer.c), then runs
 * kernel_timer_tick(regs) for the scheduler tick + preemption.
 *
 * Returns the (potentially switched) register state from kernel_timer_tick().
 *
 * Locking: per-CPU data (cpu_info); no cross-CPU locking needed.
 * IRQ context: YES — called from the IRQ dispatch loop in irq_handler().
 */
struct pt_regs *timer_handler(struct pt_regs *regs) {
  /* Tier 2 per-CPU drift accounting + CNTV_CVAL reprogram is the arch-neutral,
   * HAL-driven timer_percpu_tick() (kernel/core/timer.c): it advances
   * next_tick_target against CNTVCT with fractional correction and a catch-up
   * clamp, then arch_timer_set_compare() writes CNTV_CVAL. The arch handler is
   * now just: account, then run the generic scheduler tick. */
  timer_percpu_tick(get_cpu_info());
  return kernel_timer_tick(regs);
}

/* Legacy static handler used by irq_register?
   No, we call timer_handler directly from GIC now.
   We can remove the static wrapper or keep it for compatibility if other IRQs
   use it. But GIC logic above handles IRQ 27 explicitly.
*/

/*
 * Initialize timer (called once on boot CPU)
 */
/*
 * timer_init - initialise the ARM generic timer on the boot CPU.
 *
 * Reads CNTFRQ_EL0 into timer_freq (the HAL counter frequency consumed by the
 * arch-neutral timer_percpu_tick()/mono_ns()) and enables IRQ_TIMER (PPI 27)
 * at the GIC via irq_enable(). Does NOT arm the per-CPU compare register; that
 * is done by timer_init_percpu() on each CPU.
 *
 * NOTE(IRQ-03): The comment "We handle IRQ 27 explicitly in gic.c dispatch"
 * is stale — the special handling is in irq_handler() in irq.c, not gic.c.
 *
 * Locking: none; called once from boot CPU before SMP.
 * IRQ context: NO.
 */
void timer_init(void) {
  timer_freq = read_cntfrq();

  pr_info("Timer: Frequency %lu Hz\n", timer_freq);
  pr_info("Timer: System tick rate %d Hz\n", HZ);

  /* Register virtual timer interrupt (IRQ 27 on QEMU virt) */
  /* We handle IRQ 27 explicitly in gic.c dispatch to pass regs */
  irq_enable(IRQ_TIMER);
}

/*
 * timer_init_percpu - arm the virtual timer on the calling CPU.
 *
 * Initialises per-CPU timer state and arms the EL1 virtual timer:
 *   1. Set cpu->next_tick_target = CNTVCT_EL0 + one tick interval.
 *   2. Zero cpu->tick_error_acc (fractional remainder accumulator).
 *   3. Write next_tick_target to CNTV_CVAL_EL0.
 *   4. Write 1 to CNTV_CTL_EL0 (ENABLE=1, IMASK=0) to start the timer.
 *   5. Call irq_enable(IRQ_TIMER) to unmask PPI 27 in the GIC for this CPU.
 *
 * Must be called after gic_init_cpu() has enabled the GICC interface on this
 * CPU.  Called once per secondary CPU during SMP bring-up.
 *
 * Locking: writes only per-CPU state and per-CPU system registers; no lock.
 * IRQ context: NO.
 */
/*
 * Initialize timer on each CPU
 * NOTE: Timer disabled temporarily - causes SError in QEMU -kernel mode
 * Timer works correctly with UEFI boot (./build_iso.sh run)
 */
void timer_init_percpu(void) {
  struct cpu_info *cpu = get_cpu_info();

  /* Seed the per-CPU tick schedule + first CNTV_CVAL via the arch-neutral
   * HAL-driven helper (kernel/core/timer.c). */
  timer_percpu_arm(cpu);

  /* Enable virtual timer (ENABLE=1, IMASK=0) */
  write_cntv_ctl(1);

  /* Enable virtual timer IRQ in GIC (PPI, already per-CPU) */
  irq_enable(IRQ_TIMER);

  pr_info("Timer: Per-CPU virtual timer enabled (IRQ %d). Next: 0x%lx, Ctl: "
          "0x1\n",
          IRQ_TIMER, cpu->next_tick_target);
}

/*
 * timer_get_ticks - return the current value of the virtual counter.
 *
 * Returns: CNTVCT_EL0 (64-bit free-running counter in timer_freq ticks/sec).
 *
 * IRQ context: safe.
 */
/*
 * Get current timer ticks
 */
uint64_t timer_get_ticks(void) { return read_cntvct(); }

/*
 * timer_get_us - return microseconds elapsed since boot.
 *
 * Computes (CNTVCT_EL0 * 1000000) / timer_freq.  May overflow for very large
 * counter values if timer_freq is small, but at 54 MHz (typical QEMU) the
 * 64-bit numerator overflows after ~340 years.
 *
 * Returns: elapsed microseconds since counter was reset (at boot).
 *
 * IRQ context: safe.
 */
/*
 * Get microseconds since boot
 */
uint64_t timer_get_us(void) {
  return (read_cntvct() * USEC_PER_SEC) / timer_freq;
}

/*
 * timer_delay_us - busy-wait for at least @us microseconds.
 *
 * @us: delay in microseconds.
 *
 * Reads CNTVCT_EL0, computes the target tick count, and spins until the
 * counter advances by the required amount.  Blocks the calling CPU entirely;
 * no yield or sleep.
 *
 * Locking: none; reads only per-CPU system registers.
 * IRQ context: safe (does not sleep), but holding IRQs off for long delays
 *              will delay timer/device interrupts.
 */
/*
 * Delay for specified microseconds
 */
void timer_delay_us(uint64_t us) {
  uint64_t start = read_cntvct();
  uint64_t ticks = (us * timer_freq) / USEC_PER_SEC;

  while ((read_cntvct() - start) < ticks)
    ;
}

/*
 * timer_delay_ms - busy-wait for at least @ms milliseconds.
 *
 * @ms: delay in milliseconds.
 *
 * Convenience wrapper; delegates to timer_delay_us(ms * 1000).
 *
 * Locking: same as timer_delay_us.
 * IRQ context: same as timer_delay_us.
 */
/*
 * Delay for specified milliseconds
 */
void timer_delay_ms(uint64_t ms) { timer_delay_us(ms * 1000); }

/* Software timer functions are now in kernel/core/timer.c */
