#include <arch/amd64/apic.h>
#include <kernel/printk.h>
#include <kernel/arch.h>
#include <arch/amd64_internal.h>

static uintptr_t lapic_base = 0xFEE00000;
static uint32_t ticks_per_ms = 0;

static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(lapic_base + reg);
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(lapic_base + reg) = val;
}

void lapic_init(void) {
    /* Debug: Print 'L' using %dx for 16-bit port */
    __asm__ __volatile__("movw $0x3f8, %%dx; movb $'L', %%al; outb %%al, %%dx" ::: "ax", "dx");

    /* Ensure APIC is enabled in MSR */
    uint64_t apic_msr = rdmsr(0x1B); /* IA32_APIC_BASE */
    if (!(apic_msr & 0x800)) {
        wrmsr(0x1B, apic_msr | 0x800);
    }

    /* Set Spurious Interrupt Vector and enable APIC */
    lapic_write(LAPIC_SVR, lapic_read(LAPIC_SVR) | 0xFF | LAPIC_SVR_ENABLE);
    
    pr_info("AMD64: LAPIC %u initialized at 0x%lx\n", lapic_get_id(), lapic_base);
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_send_ipi(uint32_t lapic_id, uint32_t flags) {
    /* Wait for previous IPI to complete */
    while (lapic_read(LAPIC_ICR_LOW) & ICR_SEND_PENDING) {
        arch_yield();
    }

    lapic_write(LAPIC_ICR_HIGH, lapic_id << 24);
    lapic_write(LAPIC_ICR_LOW, flags);
}

/* PIT Constants for calibration */
#define PIT_CH0 0x40
#define PIT_CMD 0x43

void lapic_timer_calibrate(void) {
    if (ticks_per_ms != 0) return;

    pr_info("LAPIC: Calibrating timer against PIT...\n");

    /* Set PIT to oneshot mode, maximum count (65535) */
    outb(PIT_CMD, 0x34); /* Channel 0, lobyte/hibyte, rate generator (Mode 2) */
    outb(PIT_CH0, 0xFF);
    outb(PIT_CH0, 0xFF);

    /* Set LAPIC Timer to Divide by 16 */
    lapic_write(LAPIC_TDCR, LAPIC_TIMER_DIV16);

    /* Get initial PIT count */
    uint16_t start_tick = 0xFFFF;
    
    /* Start LAPIC Timer with maximum count */
    lapic_write(LAPIC_TIC, 0xFFFFFFFF);

    /* Wait until PIT has decreased by a certain amount (11932 ticks = ~10ms) */
    uint16_t current_tick;
    do {
        outb(PIT_CMD, 0x00); /* Latch Channel 0 */
        current_tick = inb(PIT_CH0);
        current_tick |= (inb(PIT_CH0) << 8);
    } while ((start_tick - current_tick) < 11932);

    /* Read LAPIC Timer current count */
    uint32_t ticks = 0xFFFFFFFF - lapic_read(LAPIC_TCC);
    ticks_per_ms = ticks / 10;

    pr_info("LAPIC: Timer calibrated: %u ticks per ms\n", ticks_per_ms);
}

void lapic_timer_setup(uint32_t hz) {
    if (ticks_per_ms == 0) {
        lapic_timer_calibrate();
    }

    /* Stop current timer */
    lapic_timer_stop();

    /* Set up LAPIC Timer for periodic interrupts */
    /* Vector 32 (IRQ 0 equivalent), periodic mode */
    lapic_write(LAPIC_LVT_TIMER, 32 | LAPIC_LVT_PERIODIC);
    lapic_write(LAPIC_TDCR, LAPIC_TIMER_DIV16);
    
    /* Calculate ticks per interrupt */
    uint32_t interval_ms = 1000 / hz;
    lapic_write(LAPIC_TIC, ticks_per_ms * interval_ms);

    pr_info("LAPIC: CPU %u timer started at %u Hz (%u ticks/interval)\n", 
            lapic_get_id(), hz, ticks_per_ms * interval_ms);
}

void lapic_timer_stop(void) {
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIC, 0);
}
