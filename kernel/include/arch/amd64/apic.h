#ifndef ARCH_AMD64_APIC_H
#define ARCH_AMD64_APIC_H

#include <kernel/memlayout.h>
#include <kernel/types.h>

/* Local APIC Register Offsets */
#define LAPIC_ID            0x0020
#define LAPIC_VER           0x0030
#define LAPIC_TPR           0x0080
#define LAPIC_APR           0x0090
#define LAPIC_PPR           0x00A0
#define LAPIC_EOI           0x00B0
#define LAPIC_RRD           0x00C0
#define LAPIC_LDR           0x00D0
#define LAPIC_DFR           0x00E0
#define LAPIC_SVR           0x00F0
#define LAPIC_ISR           0x0100 /* 0x0100 - 0x0170 */
#define LAPIC_TMR           0x0180 /* 0x0180 - 0x01F0 */
#define LAPIC_IRR           0x0200 /* 0x0200 - 0x0270 */
#define LAPIC_ESR           0x0280
#define LAPIC_ICR_LOW       0x0300
#define LAPIC_ICR_HIGH      0x0310
#define LAPIC_LVT_TIMER     0x0320
#define LAPIC_LVT_THERMAL   0x0330
#define LAPIC_LVT_PERF      0x0340
#define LAPIC_LVT_LINT0     0x0350
#define LAPIC_LVT_LINT1     0x0360
#define LAPIC_LVT_ERR       0x0370
#define LAPIC_TIC           0x0380
#define LAPIC_TCC           0x0390
#define LAPIC_TDCR          0x03E0

#define LAPIC_SVR_ENABLE    0x100

/* ICR Delivery Modes */
#define ICR_FIXED           0x000
#define ICR_LOWEST          0x100
#define ICR_SMI             0x200
#define ICR_NMI             0x400
#define ICR_INIT            0x500
#define ICR_STARTUP         0x600

#define ICR_PHYSICAL        0x0000
#define ICR_LOGICAL         0x0800

#define ICR_IDLE            0x0000
#define ICR_SEND_PENDING    0x1000

#define ICR_DEASSERT        0x0000
#define ICR_ASSERT          0x4000

#define ICR_EDGE            0x0000
#define ICR_LEVEL           0x8000

#define ICR_NO_SHORTHAND    0x00000
#define ICR_SELF            0x40000
#define ICR_ALL_INCL_SELF   0x80000
#define ICR_ALL_EXCL_SELF   0xC0000

#define LAPIC_LVT_MASKED    0x10000
#define LAPIC_LVT_PERIODIC  0x20000

#define LAPIC_TIMER_DIV16   0x03

#define LAPIC_DEFAULT_BASE  0xFEE00000UL

/* LAPIC_DEFAULT_BASE is a physical address; registers are accessed at its
 * direct-map kernel VA (phys_to_virt — identity while KERNEL_VIRT_BASE==0). */
static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)phys_to_virt(LAPIC_DEFAULT_BASE + reg);
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)phys_to_virt(LAPIC_DEFAULT_BASE + reg) = val;
}

void lapic_init(void);
void lapic_eoi(void);
uint32_t lapic_get_id(void);
void lapic_send_ipi(uint32_t lapic_id, uint32_t flags);
void lapic_timer_calibrate(void);
void lapic_timer_setup(uint32_t hz);
void lapic_timer_stop(void);

#endif /* ARCH_AMD64_APIC_H */
