/*
 * kernel/HAL/arch/amd64/cpu/apic.c
 * Local APIC (LAPIC) Initialization, EOI, IPI, and Timer Calibration
 *
 * Responsibilities:
 *   - Enable the LAPIC via IA32_APIC_BASE MSR (bit 11 = APIC global enable).
 *   - Set the Spurious Interrupt Vector Register (SVR) to enable the LAPIC
 *     and assign vector 0xFF as the spurious vector.
 *   - Configure LINT0 as ExtINT, on the BSP only (FIX(APIC-01)), to forward
 *     legacy 8259 PIC interrupts when no I/O APIC is present; every AP
 *     explicitly masks LINT0 instead.
 *   - Provide lapic_eoi() for hardware interrupt acknowledgement.
 *   - Provide lapic_send_ipi() for inter-processor interrupts (SMP startup,
 *     future TLB shootdown, etc.).
 *   - Calibrate the LAPIC timer against the 8254 PIT to determine
 *     ticks_per_ms, then provide TSC-deadline, LAPIC one-shot countdown,
 *     and periodic fallback clockevent modes.
 *
 * Invariants:
 *   - LAPIC registers are accessed via lapic_read/lapic_write (defined in
 *     arch/amd64/apic.h) using the memory-mapped register window at
 *     LAPIC_DEFAULT_BASE (0xFEE00000), which must be identity-mapped before
 *     this file's functions are called.
 *   - ticks_per_ms is set once by lapic_timer_calibrate() and never cleared;
 *     the guard 'if (ticks_per_ms != 0) return' makes calibration idempotent.
 *   - LINT0-as-ExtINT is enabled on the BSP only; every AP masks it
 *     (FIX(APIC-01), lapic_init()) — see PIC-01 in pic_pit.c for why no
 *     amd64 IRQ line can be software-distributed across CPUs the way
 *     GICv2 SPIs are on aarch64 (gic.c, DRV-GIC-01): this driver has no
 *     I/O APIC and therefore no per-line destination register to program.
 *
 * Known issues:
 *   APIC-01 RESOLVED (this pass): lapic_init() used to write LINT0 =
 *     ExtINT-not-masked unconditionally on every CPU, BSP and APs alike.
 *     Per the Intel MultiProcessor Specification, ExtINT-via-LINT0 virtual-
 *     wire mode is a BSP-only configuration ("NMI and INTR must be
 *     connected to the BSP"); enabling it on an AP too risks duplicate
 *     interrupt delivery on hardware whose LINT0 wiring reaches more than
 *     one core (documented failure mode in the Linux kernel mailing list's
 *     virtual-wire-on-shutdown fix).  Fixed: only the CPU whose
 *     lapic_get_id() == 0 (the BSP) programs LINT0 as ExtINT; every AP
 *     explicitly masks LINT0 instead of leaving it at whatever the AP
 *     happened to reset into.
 *   EXC-AMD64-03 RESOLVED (Phase A step 14): the double-tick hazard came
 *     from the PIT being left free-running (mode 2) after calibration while
 *     LINT0 ExtINT can deliver PIC IRQ 0 on vector 32 — the same vector as
 *     the LAPIC periodic timer.  lapic_timer_calibrate() now halts the PIT
 *     (mode-0 control word, no count) before the LAPIC timer starts; PIC
 *     IRQ 0 additionally stays masked (pic_init).  LINT0 remains ExtINT on
 *     the BSP on purpose: it is the only delivery path for legacy PIC
 *     lines (PCI INTx).
 */
#include <hal/diagnostic_protocol.h>
#include <hal/core_protocol.h>
#include <hal/timer_protocol.h>
#include <arch/pt_regs.h>
#include <hal/core.h>
#include <arch/arch.h>
#include <arch/amd64/apic.h>
#include <arch/amd64_internal.h>
#include <drivers/timer.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/spinlock.h>

/* ticks_per_ms: LAPIC timer decrements per millisecond at LAPIC_TIMER_DIV16.
 * Set by lapic_timer_calibrate(); used by periodic fallback and udelay(). */
uint32_t ticks_per_ms = 0;

int lapic_tsc_deadline_supported(void) {
  uint32_t eax = 0U;
  uint32_t ebx = 0U;
  uint32_t ecx = 0U;
  uint32_t edx = 0U;

  arch_isa_cpuid(1U, 0U, &eax, &ebx, &ecx, &edx);
  return (ecx & (UINT32_C(1) << 24U)) != 0U;
}

/* calib_lock - serialises tsc_calibrate()/lapic_timer_calibrate() against
 * the shared, single-instance 8254 PIT.
 *
 * FIX(TIMER-CALIB-RACE-01): both functions guard their own body with a bare
 * "if (already_done) return" on tsc_hz / ticks_per_ms.  That check-then-act
 * sequence is idempotent ONLY as long as no two CPUs can ever be inside it
 * at once — a property this file never enforced itself; it relied entirely
 * on an invariant living in kernel/main.c and kernel/core/smp.c (BSP
 * calibrates before any AP is woken, and APs are brought up strictly one at
 * a time, ack-gated).  That invariant is real today, but it is invisible
 * from this file, easy to violate by a future change to the boot sequence
 * (or a bring-up path that does not go through smp_bringup_secondary), and
 * its violation is exactly the class of bug that produces this system's
 * reported symptom: two CPUs interleaving their PIT programming
 * (PIT_CMD/PIT_CH0 writes) corrupts the ~10 ms measurement window for BOTH,
 * silently publishing a garbage ticks_per_ms/tsc_hz that every CPU then
 * trusts forever — a wrong (or zero-length) LAPIC periodic interval reads
 * exactly like "the clock stopped", intermittently, depending on how the
 * two calibration attempts happened to race.  Locking the whole
 * check-and-calibrate sequence removes the dependency on that cross-file
 * invariant instead of merely trusting it, at zero cost on the fast path
 * (real calibration runs exactly once per boot; every other caller takes
 * the lock only to observe the already-published value and return). */
static spinlock_t calib_lock = SPINLOCK_INIT;

/* tsc_hz: measured TSC frequency in counts/second (docs/TIMER-MODEL.md §1).
 * This is the real-time reference the whole 3-tier clock is built on:
 * arch_impl_timer_get_freq() (arch/arch.h) returns it, and the arch-neutral
 * mono_ns() in kernel/core/timer.c divides the free-running RDTSC by it.
 *
 * Set ONCE on the BSP by tsc_calibrate() (called from lapic_timer_calibrate())
 * against the same 8254 PIT window the LAPIC timer is calibrated with.  Until
 * that runs, arch_impl_timer_get_freq() returns a safe 1 GHz fallback so early
 * callers never divide by zero; tsc_calibrate() then publishes the real value.
 * The 0 sentinel makes calibration idempotent (APs reuse the BSP value; the
 * invariant/constant TSC of the QEMU targets makes that correct). */
uint64_t tsc_hz = 0;

/*
 * lapic_init - enable and configure the LAPIC for the calling CPU.
 *
 * Steps:
 *   1. Read IA32_APIC_BASE (MSR 0x1B); set bit 11 (AEN) if not already set.
 *   2. Write SVR: set Enable bit (bit 8) and spurious vector 0xFF.
 *   3. Configure LINT0 — ONLY on the BSP — as ExtINT (delivery mode 0x700)
 *      so the legacy 8259 PIC can deliver its IRQs via the LAPIC when no
 *      I/O APIC is present.  Every AP explicitly masks LINT0 instead.
 *      FIX(APIC-01): see the note below this doc-comment for why this
 *      guard was missing and what it could cause.
 *
 * NOTE(EXC-AMD64-03, resolved): the PIT is halted after calibration and PIC
 * IRQ 0 stays masked, so vector 32 only ever comes from the LAPIC timer.
 *
 * FIX(APIC-01): LINT0-as-ExtINT used to be written unconditionally on every
 * CPU that called this function — BSP and every AP alike.  Per the Intel
 * MultiProcessor Specification's definition of virtual-wire mode, ExtINT
 * delivery through LINT0 is a BSP-only configuration ("NMI and INTR must be
 * connected to the BSP"); programming it on an AP as well is documented
 * (Linux kernel mailing list, kexec virtual-wire fix, 2004) to risk
 * DUPLICATE interrupt delivery depending on the board's actual LINT0
 * wiring — the 8259's single INTR line can, on some chipsets, reach more
 * than one CPU's LINT0 pin, and an AP with ExtINT enabled would then also
 * accept and (mis)dispatch a PIC interrupt meant to be serviced once, by
 * the BSP, through nxhal_core_irq_dispatch()/pic_chip.  QEMU's virt-wire wiring
 * happens to route INTR to the BSP only, which is why this went
 * unobserved in emulation; it is not something this driver should rely on
 * for real hardware.  Every AP now explicitly masks LINT0 (delivery mode
 * left at ExtINT for documentation purposes, but bit 16 = 1 disables
 * delivery entirely) instead of leaving it in whatever state the AP
 * happened to reset into.
 *
 */
void lapic_init(void) {
  /* Ensure APIC is enabled in MSR (bit 11 = APIC global enable) */
  uint64_t apic_msr = rdmsr(0x1B); /* IA32_APIC_BASE */
  if (!(apic_msr & 0x800)) {
    wrmsr(0x1B, apic_msr | 0x800);
  }

  /* Set Spurious Interrupt Vector (0xFF) and enable LAPIC (SVR bit 8) */
  lapic_write(LAPIC_SVR, lapic_read(LAPIC_SVR) | 0xFF | LAPIC_SVR_ENABLE);

  /* FIX(APIC-01): LINT0-as-ExtINT is a BSP-only configuration (Intel MP
   * Spec virtual-wire mode: "NMI and INTR must be connected to the
   * BSP") — see the function-header note for the duplicate-delivery
   * risk of programming it on every CPU. */
  if (lapic_get_id() == 0) {
    /* Configure LINT0 for ExtINT (Legacy PIC) — necessary if no IOAPIC
     * is used.  Delivery mode 0x700 = ExtINT; not masked (bit 16 = 0).
     * NOTE(EXC-AMD64-03, resolved): PIC IRQ 0 could reach vector 32
     * through here, but the PIT is halted after calibration and IRQ 0
     * stays masked. */
    lapic_write(LAPIC_LVT_LINT0, 0x00000700); /* ExtINT, not masked */
  } else {
    /* FIX(APIC-01): explicitly masked rather than left at reset state
     * — this AP must never accept an ExtINT-delivered PIC interrupt;
     * the BSP above is the sole ExtINT receiver. Bit 16 = mask. */
    lapic_write(LAPIC_LVT_LINT0, 0x00010700); /* ExtINT mode, MASKED */
  }

  nxhal_diag_info("AMD64: LAPIC %u initialized at 0x%lx\n", lapic_get_id(),
          LAPIC_DEFAULT_BASE);
}

/*
 * lapic_eoi - signal end-of-interrupt to the local APIC.
 *
 * Must be called after every hardware IRQ vector (32-255) is serviced.  The
 * LAPIC will not deliver further interrupts at the same or lower priority until
 * the EOI write completes.  Writing any value (here 0) to LAPIC_EOI suffices.
 *
 * Called from amd64_isr_dispatch after each hardware interrupt handler.
 * Does NOT satisfy the 8259 PIC EOI for legacy vectors 32-47; idt.c calls
 * pic_send_eoi() separately for those.
 */
void lapic_eoi(void) { lapic_write(LAPIC_EOI, 0); }

/*
 * lapic_get_id - return the LAPIC ID of the calling CPU.
 *
 * The LAPIC ID register bits [31:24] hold the 8-bit APIC ID.  Used to index
 * cpu_data[] and to address IPIs.
 */
uint32_t lapic_get_id(void) { return lapic_read(LAPIC_ID) >> 24; }

/*
 * lapic_send_ipi - send an inter-processor interrupt.
 *
 * Params:
 *   lapic_id - destination APIC ID (placed in ICR_HIGH bits [31:24]).
 *   flags    - ICR_LOW encoding (vector, delivery mode, level, trigger, dest).
 *
 * Spins on ICR_LOW.ICR_SEND_PENDING (bit 12) before writing to ensure any
 * previous IPI has been accepted by the interconnect.  The write to ICR_HIGH
 * must precede the write to ICR_LOW because the latter triggers the IPI.
 *
 * Used by arch_impl_smp_wake_secondary in platform.c for INIT-SIPI sequence.
 * NOTE(BOOT-03): platform.c sends only a single STARTUP IPI; the SDM
 * recommends INIT + two SIPIs for reliability.
 */
enum {
  LAPIC_IPI_POLL_BUDGET = 65536
};

static inline uint64_t rdtsc64(void);

int lapic_send_ipi(uint32_t lapic_id, uint32_t flags) {
  uint64_t start = rdtsc64();
  uint64_t timeout = tsc_hz == 0U ? 0U : tsc_hz / UINT64_C(100);

  for (uint32_t poll = 0U; poll < LAPIC_IPI_POLL_BUDGET; ++poll) {
    if ((lapic_read(LAPIC_ICR_LOW) & ICR_SEND_PENDING) == 0U) {
      lapic_write(LAPIC_ICR_HIGH, lapic_id << 24);
      lapic_write(LAPIC_ICR_LOW, flags);
      return 0;
    }
    if (timeout != 0U && (rdtsc64() - start) >= timeout)
      break;
    hal_cpu_yield();
  }
  nxhal_diag_error("LAPIC: IPI transport timeout dest=%u flags=0x%x\n",
                   lapic_id, flags);
  return -1;
}

/* 8254 PIT channel 0 reference used only during early calibration.
 * The poll budget is a boot-resource bound: four observations for each value
 * in the 16-bit hardware counter domain. Exhaustion is calibration failure,
 * never permission to publish a guessed clock. */
enum {
  PIT_CH0 = 0x40,
  PIT_CMD = 0x43,
  PIT_REFERENCE_HZ = 1193182,
  PIT_CALIBRATION_COUNTS = 11932,
  PIT_CALIBRATION_POLL_BUDGET = 262144,
  PIT_CALIBRATION_MAX_COUNTS = PIT_CALIBRATION_COUNTS * 2,
  TSC_CALIBRATION_SAMPLES = 7,
  TSC_CALIBRATION_QUORUM = 3,
  TSC_MAX_DEVIATION_DIVISOR = 20
};

static inline uint64_t rdtsc64(void) {
  uint32_t lo;
  uint32_t hi;

  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

static void pit_calibration_start(void) {
  outb(PIT_CMD, UINT8_C(0x34));
  outb(PIT_CH0, UINT8_C(0xff));
  outb(PIT_CH0, UINT8_C(0xff));
}

static void pit_calibration_stop(void) {
  outb(PIT_CMD, UINT8_C(0x30));
}

static uint16_t pit_calibration_read(void) {
  uint16_t count;

  outb(PIT_CMD, UINT8_C(0x00));
  count = (uint16_t)inb(PIT_CH0);
  count |= (uint16_t)((uint16_t)inb(PIT_CH0) << 8);
  return count;
}

/* The PIT is the actual reference. A 10 ms target is not the observed
 * duration: host scheduling may delay the final latch and TSC sample.
 * Detect reloads and reject an overlong window rather than normalizing to
 * an unobserved duration. Both bounds come from the physical PIT counter. */
static int pit_wait_calibration_window(uint32_t *elapsed_out) {
  uint16_t previous = UINT16_MAX;

  if (elapsed_out == NULL)
    return -1;
  for (uint32_t poll = 0U; poll < PIT_CALIBRATION_POLL_BUDGET; ++poll) {
    uint16_t current = pit_calibration_read();
    uint32_t elapsed = (uint32_t)(UINT16_MAX - current);
    if (current > previous || elapsed > PIT_CALIBRATION_MAX_COUNTS)
      return -1;
    if (elapsed >= PIT_CALIBRATION_COUNTS) {
      *elapsed_out = elapsed;
      return 0;
    }
    previous = current;
    hal_cpu_nop();
  }
  return -1;
}

static uint64_t pit_measure_tsc_hz(void) {
  uint32_t pit_elapsed = 0U;
  uint64_t start;
  uint64_t end;
  int status;

  pit_calibration_start();
  start = rdtsc64();
  status = pit_wait_calibration_window(&pit_elapsed);
  end = rdtsc64();
  pit_calibration_stop();
  if (status != 0 || end <= start || pit_elapsed == 0U)
    return 0U;
  if ((end - start) > UINT64_MAX / PIT_REFERENCE_HZ)
    return 0U;
  return ((end - start) * PIT_REFERENCE_HZ) / pit_elapsed;
}

static void tsc_samples_sort(uint64_t *sorted, uint32_t valid) {
  for (uint32_t i = 0U; i < TSC_CALIBRATION_SAMPLES; ++i) {
    for (uint32_t j = 0U; j < TSC_CALIBRATION_SAMPLES; ++j) {
      if (i < valid && j > i && j < valid && sorted[j] < sorted[i]) {
        uint64_t value = sorted[i];
        sorted[i] = sorted[j];
        sorted[j] = value;
      }
    }
  }
}

/* Select only a real 3-sample cluster, not a hard-coded CPU frequency.
 * Invalid PIT windows are omitted and a missing quorum is an actual error.
 * Sorting uses fixed-capacity bounded loops for early boot safety. */
static int tsc_samples_select(const uint64_t *samples, uint64_t *frequency) {
  uint64_t sorted[TSC_CALIBRATION_SAMPLES];
  uint32_t valid = 0U;

  if (samples == NULL || frequency == NULL)
    return -1;
  for (uint32_t i = 0U; i < TSC_CALIBRATION_SAMPLES; ++i) {
    if (samples[i] != 0U)
      sorted[valid++] = samples[i];
  }
  if (valid < TSC_CALIBRATION_QUORUM)
    return -1;
  /* Every sorting/index step has a static seven-sample bound.
   * The valid count only masks unused slots, never controls the loop. */
  tsc_samples_sort(sorted, valid);
  uint64_t best_span = UINT64_MAX;
  uint64_t selected = 0U;
  for (uint32_t i = 0U; i < TSC_CALIBRATION_SAMPLES; ++i) {
    if (i + TSC_CALIBRATION_QUORUM > valid)
      continue;
    uint64_t low = sorted[i];
    uint64_t high = sorted[i + TSC_CALIBRATION_QUORUM - 1U];
    uint64_t span = high - low;
    if (span <= high / TSC_MAX_DEVIATION_DIVISOR &&
        span < best_span) {
      best_span = span;
      selected = sorted[i + TSC_CALIBRATION_QUORUM / 2U];
    }
  }
  if (selected == 0U)
    return -1;
  *frequency = selected;
  return 0;
}

static int tsc_calibrate_locked(void) {
  uint64_t samples[TSC_CALIBRATION_SAMPLES];
  uint64_t selected = 0U;

  if (nxhal_diag_info("%s", "TSC: bounded PIT calibration start\n") < 0)
    return -1;
  for (uint32_t i = 0U; i < TSC_CALIBRATION_SAMPLES; ++i) {
    samples[i] = pit_measure_tsc_hz();
    if (samples[i] == 0U) {
      if (nxhal_diag_warn("TSC: invalid PIT sample %u (ignored)\n", i) < 0)
        return -1;
    } else if (nxhal_diag_debug("TSC: PIT sample %u=%lu Hz\n",
                               i, samples[i]) < 0) {
      return -1;
    }
  }
  if (tsc_samples_select(samples, &selected) != 0) {
    if (nxhal_diag_error("%s",
                         "TSC: no validated PIT calibration quorum\n") < 0)
      return -1;
    return -1;
  }
  /* A real quorum is usable; it does not erase the discrepant samples.
   * They remain visible as warnings (and NXPAX diagnostics) for audit. */
  for (uint32_t i = 0U; i < TSC_CALIBRATION_SAMPLES; ++i) {
    if (samples[i] == 0U)
      continue;
    uint64_t deviation = samples[i] > selected ? samples[i] - selected
                                               : selected - samples[i];
    if (deviation > selected / TSC_MAX_DEVIATION_DIVISOR &&
        nxhal_diag_warn("TSC: PIT sample %u outlier=%lu Hz selected=%lu Hz\n",
                        i, samples[i], selected) < 0)
      return -1;
  }
  tsc_hz = selected;
  if (nxhal_diag_info("TSC: validated %lu Hz (%lu MHz)\n",
                      tsc_hz, tsc_hz / UINT64_C(1000000)) < 0)
    return -1;
  return 0;
}

void tsc_calibrate(void) {
  uint64_t calib_flags;

  spin_lock_irqsave(&calib_lock, &calib_flags);
  if (tsc_hz == 0U)
    (void)tsc_calibrate_locked();
  spin_unlock_irqrestore(&calib_lock, calib_flags);
}

static int lapic_measure_ticks_per_ms(uint32_t *out_ticks_per_ms) {
  uint32_t pit_elapsed = 0U;
  uint32_t lapic_elapsed;
  int status;
  uint64_t event_hz;

  if (out_ticks_per_ms == NULL)
    return -1;
  pit_calibration_start();
  lapic_write(LAPIC_TDCR, LAPIC_TIMER_DIV16);
  lapic_write(LAPIC_TIC, UINT32_MAX);
  status = pit_wait_calibration_window(&pit_elapsed);
  lapic_elapsed = UINT32_MAX - lapic_read(LAPIC_TCC);
  pit_calibration_stop();
  lapic_timer_stop();
  if (status != 0 || lapic_elapsed == 0U || pit_elapsed == 0U)
    return -1;
  event_hz = ((uint64_t)lapic_elapsed * PIT_REFERENCE_HZ) / pit_elapsed;
  if (event_hz < UINT64_C(1000) ||
      event_hz / UINT64_C(1000) > UINT32_MAX)
    return -1;
  *out_ticks_per_ms = (uint32_t)(event_hz / UINT64_C(1000));
  return *out_ticks_per_ms == 0U ? -1 : 0;
}

void lapic_timer_calibrate(void) {
  uint64_t calib_flags;
  uint32_t measured = 0U;

  spin_lock_irqsave(&calib_lock, &calib_flags);
  if (ticks_per_ms != 0U) {
    spin_unlock_irqrestore(&calib_lock, calib_flags);
    return;
  }
  if (tsc_hz == 0U && tsc_calibrate_locked() != 0) {
    spin_unlock_irqrestore(&calib_lock, calib_flags);
    return;
  }
  if (lapic_measure_ticks_per_ms(&measured) != 0) {
    nxhal_diag_error("%s", "LAPIC: bounded PIT calibration failed\n");
    spin_unlock_irqrestore(&calib_lock, calib_flags);
    return;
  }
  ticks_per_ms = measured;
  nxhal_diag_info("LAPIC: calibrated %u ticks/ms\n", ticks_per_ms);
  spin_unlock_irqrestore(&calib_lock, calib_flags);
}

/*
 * lapic_timer_setup - start the LAPIC periodic timer at hz interrupts/second.
 *
 * Ensures calibration has run (calls lapic_timer_calibrate if needed).
 * Programs LVT timer register with vector 32, periodic mode, divisor /16.
 * Sets the initial count to ticks_per_ms * (1000/hz).
 *
 * NOTE(EXC-AMD64-03, resolved): by the time this runs the PIT has been
 * halted by lapic_timer_calibrate(), so vector 32 has a single source.
 *
 * Params:
 *   hz - desired interrupt frequency (e.g. HZ = 1000 for 1 kHz timer).
 */
int lapic_timer_setup(uint64_t hz) {
  uint64_t event_hz;
  uint64_t interval;

  if (ticks_per_ms == 0U)
    lapic_timer_calibrate();
  if (ticks_per_ms == 0U || hz == 0U)
    return -1;

  event_hz = (uint64_t)ticks_per_ms * UINT64_C(1000);
  if (hz > event_hz)
    return -1;
  interval = event_hz / hz;
  if ((event_hz % hz) != 0U)
    ++interval;
  if (interval == 0U || interval > UINT32_MAX)
    return -1;

  lapic_timer_stop();
  lapic_write(LAPIC_LVT_TIMER, UINT32_C(32) | LAPIC_LVT_PERIODIC);
  lapic_write(LAPIC_TDCR, LAPIC_TIMER_DIV16);
  lapic_write(LAPIC_TIC, (uint32_t)interval);
  if (nxhal_timer_arm_current() == 0U)
    return -1;
  nxhal_diag_info("LAPIC: CPU %u timer started at %lu Hz (%lu ticks/interval)\n",
                  lapic_get_id(), hz, interval);
  return 0;
}

static int lapic_muldiv_floor(uint64_t left, uint64_t right,
                               uint64_t divisor, uint64_t *quotient,
                               uint64_t *remainder) {
  uint64_t high;
  uint64_t low;
  uint64_t q;
  uint64_t r;

  if (divisor == 0U || quotient == NULL || remainder == NULL)
    return -1;
  __asm__ __volatile__("mulq %3"
                       : "=a"(low), "=d"(high)
                       : "a"(left), "r"(right)
                       : "cc");
  if (high >= divisor)
    return -1;
  __asm__ __volatile__("divq %4"
                       : "=a"(q), "=d"(r)
                       : "a"(low), "d"(high), "r"(divisor)
                       : "cc");
  *quotient = q;
  *remainder = r;
  return 0;
}

static uint64_t lapic_oneshot_event_hz(void) {
  if (ticks_per_ms == 0U)
    return 0U;
  return (uint64_t)ticks_per_ms * UINT64_C(1000);
}

uint64_t lapic_timer_oneshot_min_tsc_delta(void) {
  uint64_t event_hz = lapic_oneshot_event_hz();
  if (tsc_hz == 0U || event_hz == 0U)
    return 0U;
  uint64_t delta = tsc_hz / event_hz;
  if ((tsc_hz % event_hz) != 0U)
    ++delta;
  return delta == 0U ? 1U : delta;
}

uint64_t lapic_timer_oneshot_max_tsc_delta(void) {
  uint64_t event_hz = lapic_oneshot_event_hz();
  uint64_t quotient = 0U;
  uint64_t remainder = 0U;
  if (tsc_hz == 0U || event_hz == 0U)
    return 0U;
  if (lapic_muldiv_floor(UINT32_MAX, tsc_hz, event_hz,
                         &quotient, &remainder) != 0)
    return UINT64_MAX;
  return quotient == 0U ? 1U : quotient;
}

static uint32_t lapic_oneshot_ticks_for_tsc_delta(uint64_t delta) {
  uint64_t event_hz = lapic_oneshot_event_hz();
  uint64_t quotient = 0U;
  uint64_t remainder = 0U;
  if (delta == 0U || event_hz == 0U || tsc_hz == 0U)
    return 1U;
  if (lapic_muldiv_floor(delta, event_hz, tsc_hz,
                         &quotient, &remainder) != 0)
    return UINT32_MAX;
  if (remainder != 0U && quotient != UINT64_MAX)
    ++quotient;
  if (quotient == 0U)
    quotient = 1U;
  if (quotient > UINT32_MAX)
    quotient = UINT32_MAX;
  return (uint32_t)quotient;
}

int lapic_timer_oneshot_setup(void) {
  if (tsc_hz == 0U || ticks_per_ms == 0U)
    return -1;
  lapic_timer_stop();
  lapic_write(LAPIC_LVT_TIMER, UINT32_C(32));
  lapic_write(LAPIC_TDCR, LAPIC_TIMER_DIV16);
  nxhal_diag_info("LAPIC: CPU %u one-shot countdown mode enabled\n",
                  lapic_get_id());
  return 0;
}

int lapic_timer_oneshot_program_tsc(uint64_t deadline) {
  if (deadline == 0U || tsc_hz == 0U || ticks_per_ms == 0U)
    return -1;
  uint64_t now = rdtsc64();
  uint64_t delta = deadline > now ? deadline - now : 1U;
  lapic_write(LAPIC_TIC, lapic_oneshot_ticks_for_tsc_delta(delta));
  return 0;
}

int lapic_timer_deadline_setup(void) {
  if (tsc_hz == 0U || lapic_tsc_deadline_supported() == 0)
    return -1;

  lapic_timer_stop();
  lapic_write(LAPIC_LVT_TIMER,
              UINT32_C(32) | LAPIC_LVT_TSC_DEADLINE);
  /* Intel SDM: in xAPIC mode LVTT and IA32_TSC_DEADLINE writes are not
   * serialized.  Match the reference sequence and order the mode write before
   * the first deadline-MSR access. */
  __asm__ __volatile__("mfence" ::: "memory");
  wrmsr(IA32_TSC_DEADLINE_MSR, 0U);
  nxhal_diag_info("LAPIC: CPU %u TSC-deadline mode enabled\n", lapic_get_id());
  return 0;
}

int lapic_timer_deadline_program(uint64_t deadline) {
  if (deadline == 0U || tsc_hz == 0U ||
      lapic_tsc_deadline_supported() == 0)
    return -1;
  wrmsr(IA32_TSC_DEADLINE_MSR, deadline);
  return 0;
}

/* Stop either selected LAPIC timer mode.  Masking alone is insufficient on
 * CPUs affected by historical deadline-timer errata, so clear both backing
 * count registers when the TSC-deadline feature exists. */
void lapic_timer_stop(void) {
  lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
  lapic_write(LAPIC_TIC, 0U);
  if (lapic_tsc_deadline_supported() != 0)
    wrmsr(IA32_TSC_DEADLINE_MSR, 0U);
}
