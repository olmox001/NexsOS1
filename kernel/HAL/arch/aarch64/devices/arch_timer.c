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
 *     consumed by the arch-neutral nxhal_timer_tick_current()/mono_ns().
 *   - timer_init_percpu() arms the EL1 virtual timer on each CPU via
 *     nxhal_timer_arm_current() (sets CNTV_CVAL) and enables it via CNTV_CTL_EL0.
 *   - timer_handler() is called directly from nxhal_core_irq_entry() in irq.c when
 *     IRQ_TIMER (PPI 27) fires; it calls nxhal_timer_tick_current() (fractional drift
 *     accounting + CNTV_CVAL reprogram via hal_timer_set_compare) then
 *     nxhal_core_timer_tick() for scheduling.
 *
 * Timer register access is via arch_ wrappers (hal_timer_get_freq,
 * hal_timer_get_count, hal_timer_set_compare, hal_timer_control) which
 * translate to MRS/MSR system register instructions.
 *
 * Invariants:
 *   - timer_freq must be non-zero before timer_handler() is called.
 *   - Core-owned next tick target is per-CPU; updated only by timer_handler() on
 *     the owning CPU.
 *   - CPU 0 reconciles global jiffies in kernel/core/timer.c; all CPUs read
 *     atomic snapshots and process their own local software timer lists.
 *
 * Known issues:
 *   IRQ-03  (W1 DOC) The comment in timer_init() says "We handle IRQ 27
 *           explicitly in gic.c dispatch"; the actual special handling is in
 *           nxhal_core_irq_entry() in irq.c, not gic.c.  Stale cross-reference.
 */
#include <hal/diagnostic_protocol.h>
#include <hal/core_protocol.h>
#include <hal/timer_protocol.h>
#include <arch/pt_regs.h>
#include <hal/core.h>
#include <hal/irq.h>
#include <hal/provider.h>
#include <kernel/irq.h>
#include <drivers/timer.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/list.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

/* Disable optimization */

#include <kernel/vmm.h>

/* timer_freq: CNTFRQ_EL0 value in ticks/second; read once at timer_init().
 * Used to compute tick intervals and by timer_get_us(). */
/* Timer frequency */
/* Timer frequency */
static uint64_t timer_freq;

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
static inline uint64_t read_cntfrq(void) { return hal_timer_get_freq(); }

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
static inline uint64_t read_cntvct(void) { return hal_timer_get_count(); }

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
  hal_timer_set_compare(val);
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
  hal_timer_control(val);
}

/*
 * Initialize timer (called once on boot CPU)
 */
/*
 * timer_init - initialise the ARM generic timer on the boot CPU.
 *
 * Reads CNTFRQ_EL0 into timer_freq (the HAL counter frequency consumed by the
 * arch-neutral nxhal_timer_tick_current()/mono_ns()) and enables IRQ_TIMER (PPI 27)
 * at the GIC via irq_enable(). Does NOT arm the per-CPU compare register; that
 * is done by timer_init_percpu() on each CPU.
 *
 * NOTE(IRQ-03): The comment "We handle IRQ 27 explicitly in gic.c dispatch"
 * is stale — the special handling is in nxhal_core_irq_entry() in irq.c, not gic.c.
 *
 * Locking: none; called once from boot CPU before SMP.
 * IRQ context: NO.
 */
void timer_init(void) {
  timer_freq = read_cntfrq();

  nxhal_diag_info("Timer: Frequency %lu Hz\n", timer_freq);
  nxhal_diag_info("Timer: System tick rate %lu Hz\n", (uint64_t)HZ);

  /* Register virtual timer interrupt (IRQ 27 on QEMU virt) */
  /* We handle IRQ 27 explicitly in gic.c dispatch to pass regs */
  hal_irq_line_enable(IRQ_TIMER);
}

/*
 * timer_init_percpu - arm the virtual timer on the calling CPU.
 *
 * Initialises per-CPU timer state and arms the EL1 virtual timer:
 *   1. Set Core-owned next tick target = CNTVCT_EL0 + one tick interval.
 *   2. Zero Core-owned fractional accumulator (fractional remainder accumulator).
 *   3. Write next_tick_target to CNTV_CVAL_EL0.
 *   4. Write 1 to CNTV_CTL_EL0 (ENABLE=1, IMASK=0) to start the timer.
 *   5. Call hal_irq_line_enable(IRQ_TIMER) to unmask PPI 27 in the GIC for this CPU.
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
void nxhal_provider_timer_percpu_init(void) {
  /* Seed the Core-owned per-CPU tick schedule without exposing cpu_info. */
  uint64_t next_tick_target = nxhal_timer_arm_current();

  /* Enable virtual timer (ENABLE=1, IMASK=0) */
  write_cntv_ctl(1);

  /* Enable virtual timer IRQ in GIC (PPI, already per-CPU) */
  hal_irq_line_enable(IRQ_TIMER);

  nxhal_diag_info("Timer: Per-CPU virtual timer enabled (IRQ %d). Next: 0x%lx, Ctl: "
          "0x1\n",
          IRQ_TIMER, next_tick_target);
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
