/*
 * kernel/arch/amd64/cpu/apic.c
 * Local APIC (LAPIC) Initialization, EOI, IPI, and Timer Calibration
 *
 * Responsibilities:
 *   - Enable the LAPIC via IA32_APIC_BASE MSR (bit 11 = APIC global enable).
 *   - Set the Spurious Interrupt Vector Register (SVR) to enable the LAPIC
 *     and assign vector 0xFF as the spurious vector.
 *   - Configure LINT0 as ExtINT to forward legacy 8259 PIC interrupts when no
 *     I/O APIC is present.
 *   - Provide lapic_eoi() for hardware interrupt acknowledgement.
 *   - Provide lapic_send_ipi() for inter-processor interrupts (SMP startup,
 *     future TLB shootdown, etc.).
 *   - Calibrate the LAPIC timer against the 8254 PIT to determine
 *     ticks_per_ms, then set up a periodic timer at a given HZ rate.
 *
 * Invariants:
 *   - LAPIC registers are accessed via lapic_read/lapic_write (defined in
 *     arch/amd64/apic.h) using the memory-mapped register window at
 *     LAPIC_DEFAULT_BASE (0xFEE00000), which must be identity-mapped before
 *     this file's functions are called.
 *   - ticks_per_ms is set once by lapic_timer_calibrate() and never cleared;
 *     the guard 'if (ticks_per_ms != 0) return' makes calibration idempotent.
 *
 * Known issues:
 *   EXC-AMD64-03 RESOLVED (Phase A step 14): the double-tick hazard came
 *     from the PIT being left free-running (mode 2) after calibration while
 *     LINT0 ExtINT can deliver PIC IRQ 0 on vector 32 — the same vector as
 *     the LAPIC periodic timer.  lapic_timer_calibrate() now halts the PIT
 *     (mode-0 control word, no count) before the LAPIC timer starts; PIC
 *     IRQ 0 additionally stays masked (pic_init).  LINT0 remains ExtINT on
 *     purpose: it is the only delivery path for legacy PIC lines (PCI INTx).
 */
#include <arch/amd64/apic.h>
#include <kernel/printk.h>
#include <kernel/arch.h>
#include <arch/amd64_internal.h>

/* ticks_per_ms: LAPIC timer decrements per millisecond at LAPIC_TIMER_DIV16.
 * Set by lapic_timer_calibrate(); used by lapic_timer_setup() and udelay(). */
uint32_t ticks_per_ms = 0;

/*
 * lapic_init - enable and configure the LAPIC for the calling CPU.
 *
 * Steps:
 *   1. Read IA32_APIC_BASE (MSR 0x1B); set bit 11 (AEN) if not already set.
 *   2. Write SVR: set Enable bit (bit 8) and spurious vector 0xFF.
 *   3. Configure LINT0 as ExtINT (delivery mode 0x700, not masked) so the
 *      legacy 8259 PIC can deliver its IRQs via the LAPIC when no I/O APIC
 *      is present.
 *
 * NOTE(EXC-AMD64-03, resolved): the PIT is halted after calibration and PIC
 * IRQ 0 stays masked, so vector 32 only ever comes from the LAPIC timer.
 *
 * The 'outb L' at the start is a debug breadcrumb on COM1 serial port
 * (0x3F8 = COM1 data register).
 */
void lapic_init(void) {
    /* Debug: Print 'L' using %dx for 16-bit port */
    __asm__ __volatile__("movw $0x3f8, %%dx; movb $'L', %%al; outb %%al, %%dx" ::: "ax", "dx");

    /* Ensure APIC is enabled in MSR (bit 11 = APIC global enable) */
    uint64_t apic_msr = rdmsr(0x1B); /* IA32_APIC_BASE */
    if (!(apic_msr & 0x800)) {
        wrmsr(0x1B, apic_msr | 0x800);
    }

    /* Set Spurious Interrupt Vector (0xFF) and enable LAPIC (SVR bit 8) */
    lapic_write(LAPIC_SVR, lapic_read(LAPIC_SVR) | 0xFF | LAPIC_SVR_ENABLE);

    /* Configure LINT0 for ExtINT (Legacy PIC) — necessary if no IOAPIC is used.
     * Delivery mode 0x700 = ExtINT; not masked (bit 16 = 0).
     * NOTE(EXC-AMD64-03, resolved): PIC IRQ 0 could reach vector 32 through
     * here, but the PIT is halted after calibration and IRQ 0 stays masked. */
    lapic_write(LAPIC_LVT_LINT0, 0x00000700); /* ExtINT, not masked */

    pr_info("AMD64: LAPIC %u initialized at 0x%lx\n", lapic_get_id(), LAPIC_DEFAULT_BASE);
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
void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

/*
 * lapic_get_id - return the LAPIC ID of the calling CPU.
 *
 * The LAPIC ID register bits [31:24] hold the 8-bit APIC ID.  Used to index
 * cpu_data[] and to address IPIs.
 */
uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

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
 * Used by arch_cpu_wake_secondary in platform.c for INIT-SIPI sequence.
 * NOTE(BOOT-03): platform.c sends only a single STARTUP IPI; the SDM
 * recommends INIT + two SIPIs for reliability.
 */
void lapic_send_ipi(uint32_t lapic_id, uint32_t flags) {
    /* Wait for previous IPI to complete (ICR_SEND_PENDING bit must be clear) */
    while (lapic_read(LAPIC_ICR_LOW) & ICR_SEND_PENDING) {
        arch_yield();
    }

    lapic_write(LAPIC_ICR_HIGH, lapic_id << 24); /* destination APIC ID */
    lapic_write(LAPIC_ICR_LOW, flags);            /* triggers IPI delivery */
}

/* PIT Constants for calibration */
#ifndef PIT_CH0
#define PIT_CH0 0x40 /* I/O port: 8254 PIT Channel 0 counter */
#endif /* PIT_CH0 */
#ifndef PIT_CMD
#define PIT_CMD 0x43 /* I/O port: 8254 PIT command/mode register */
#endif /* PIT_CMD */

/*
 * lapic_timer_calibrate - determine LAPIC timer frequency using the PIT.
 *
 * Algorithm:
 *   1. Program the PIT Channel 0 in Rate Generator mode (mode 2) with
 *      count 0xFFFF (the maximum, ~54.9 ms at 1.19318 MHz PIT clock).
 *   2. Set LAPIC timer divisor to /16 and initial count to 0xFFFFFFFF.
 *   3. Busy-poll the PIT current count until it has decreased by 11932 ticks
 *      (11932 / 1193.18 kHz ≈ 10 ms).
 *   4. The LAPIC ticks elapsed = 0xFFFFFFFF - LAPIC_TCC.
 *   5. ticks_per_ms = elapsed / 10.
 *
 * The calibration is idempotent: if ticks_per_ms is already non-zero the
 * function returns immediately.  BSP calls this once in arch_timer_init();
 * APs do not re-calibrate (ticks_per_ms is a shared global — no race since
 * APs start after BSP calibration completes).
 *
 * NOTE(ARCH-03): timer_get_us() (platform.c) uses jiffies*1000 rather than
 * the calibrated LAPIC TCC, so the microsecond timestamp is inaccurate.
 */
void lapic_timer_calibrate(void) {
    if (ticks_per_ms != 0) return;

    pr_info("LAPIC: Calibrating timer against PIT...\n");

    /* Program PIT Channel 0: lobyte/hibyte access, Rate Generator (mode 2) */
    outb(PIT_CMD, 0x34); /* Channel 0, lobyte/hibyte, rate generator (Mode 2) */
    outb(PIT_CH0, 0xFF); /* low byte of initial count */
    outb(PIT_CH0, 0xFF); /* high byte of initial count */

    /* Set LAPIC Timer to Divide by 16 */
    lapic_write(LAPIC_TDCR, LAPIC_TIMER_DIV16);

    /* Record PIT start count (conceptually 0xFFFF; actual read not stored) */
    uint16_t start_tick = 0xFFFF;

    /* Start LAPIC Timer with maximum initial count */
    lapic_write(LAPIC_TIC, 0xFFFFFFFF);

    /* Busy-poll PIT until it has ticked down by 11932 counts (~10 ms).
     * PIT latch command (0x00 to PIT_CMD) freezes the counter for reading;
     * two reads from PIT_CH0 give the 16-bit current count (lo then hi). */
    uint16_t current_tick;
    do {
        outb(PIT_CMD, 0x00); /* Latch Channel 0 count */
        current_tick = inb(PIT_CH0);
        current_tick |= (inb(PIT_CH0) << 8);
    } while ((start_tick - current_tick) < 11932);

    /* Read LAPIC Timer current count; elapsed = initial - current */
    uint32_t ticks = 0xFFFFFFFF - lapic_read(LAPIC_TCC);
    ticks_per_ms = ticks / 10; /* elapsed in 10 ms → convert to per-ms */

    /* FIX(EXC-AMD64-03): silence the PIT now that calibration is done.
     * Mode 2 left the counter free-running, pulsing the IRQ0 line forever;
     * vector 32 must come from the LAPIC periodic timer ONLY.  Writing the
     * mode-0 control word without loading a count halts the counter (the
     * 8254 waits for a count after a control-word write), so the line stays
     * quiet even if PIC IRQ0 were ever unmasked.  LINT0 stays ExtINT — it is
     * the delivery path for every legacy PIC line (PCI INTx included) and
     * must NOT be masked. */
    outb(PIT_CMD, 0x30); /* channel 0, lobyte/hibyte, mode 0, no count loaded */

    pr_info("LAPIC: Timer calibrated: %u ticks per ms\n", ticks_per_ms);
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
void lapic_timer_setup(uint32_t hz) {
    if (ticks_per_ms == 0) {
        lapic_timer_calibrate();
    }

    /* Stop current timer before reconfiguring */
    lapic_timer_stop();

    /* Set up LAPIC Timer for periodic interrupts.
     * Vector 32 (IRQ 0 equivalent), periodic mode (LAPIC_LVT_PERIODIC).
     * NOTE(EXC-AMD64-03): same vector 32 as LAPIC LINT0 ExtINT path. */
    lapic_write(LAPIC_LVT_TIMER, 32 | LAPIC_LVT_PERIODIC);
    lapic_write(LAPIC_TDCR, LAPIC_TIMER_DIV16);

    /* Calculate ticks per interrupt: at hz=1000, interval_ms=1 */
    uint32_t interval_ms = 1000 / hz;
    lapic_write(LAPIC_TIC, ticks_per_ms * interval_ms);

    pr_info("LAPIC: CPU %u timer started at %u Hz (%u ticks/interval)\n",
            lapic_get_id(), hz, ticks_per_ms * interval_ms);
}

/*
 * lapic_timer_stop - mask and zero the LAPIC timer.
 *
 * Sets LAPIC_LVT_MASKED in the LVT timer entry (bit 16 = mask) and writes 0
 * to LAPIC_TIC (initial count register) to halt the countdown.  Called by
 * lapic_timer_setup() before reconfiguring the timer.
 */
void lapic_timer_stop(void) {
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED); /* mask timer LVT entry */
    lapic_write(LAPIC_TIC, 0);                       /* stop countdown */
}
