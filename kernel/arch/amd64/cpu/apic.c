#include <arch/amd64/apic.h>
#include <kernel/printk.h>
#include <kernel/arch.h>
#include <arch/amd64_internal.h>

static uintptr_t lapic_base = 0xFEE00000;

static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(lapic_base + reg);
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(lapic_base + reg) = val;
}

void lapic_init(void) {
    /* Debug: Print 'L' */
    __asm__ __volatile__("movb $'L', %al; outb %al, $0x3f8");

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
