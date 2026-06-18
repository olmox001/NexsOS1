#ifndef _ARCH_AMD64_H
#define _ARCH_AMD64_H

#include <stdint.h>
#include <kernel/memlayout.h>
#include <kernel/types.h>

/* AMD64 HAL Implementation Primitives */
#include <kernel/elf.h>
#define ARCH_TYPE EM_X86_64

/* --- Interrupt Control --- */
static inline void arch_impl_irq_enable(void) {
  __asm__ __volatile__("sti" ::: "memory");
}

static inline void arch_impl_irq_disable(void) {
  __asm__ __volatile__("cli" ::: "memory");
}

static inline void arch_impl_irq_save(uint64_t *flags) {
  __asm__ __volatile__("pushfq\n\t"
                       "popq %0\n\t"
                       "cli"
                       : "=r"(*flags)
                       :
                       : "memory");
}

static inline void arch_impl_irq_restore(uint64_t flags) {
  __asm__ __volatile__("pushq %0\n\t"
                       "popfq"
                       :
                       : "r"(flags)
                       : "memory", "cc");
}

static inline void arch_impl_irq_save_all(uint64_t *flags) {
  arch_impl_irq_save(flags);
}

static inline void arch_impl_irq_restore_all(uint64_t flags) {
  arch_impl_irq_restore(flags);
}

static inline void arch_impl_irq_disable_all(void) {
  arch_impl_irq_disable();
}

/* --- CPU Control --- */
static inline void arch_impl_nop(void) { __asm__ __volatile__("nop"); }
static inline void arch_impl_idle(void) { __asm__ __volatile__("hlt"); }
static inline void arch_impl_yield(void) { __asm__ __volatile__("pause"); }
static inline void arch_impl_cpu_notify(void) { /* NOP on x86 for now */ }

/* Barriers */
static inline void arch_impl_isb(void) { __asm__ __volatile__("mfence" ::: "memory"); }
static inline void arch_impl_mb(void)  { __asm__ __volatile__("mfence" ::: "memory"); }
static inline void arch_impl_rmb(void) { __asm__ __volatile__("lfence" ::: "memory"); }
static inline void arch_impl_wmb(void) { __asm__ __volatile__("sfence" ::: "memory"); }

static inline uint32_t arch_impl_get_cpu_id(void) {
  /* Use the actual LAPIC ID register for more accuracy than CPUID leaf 1.
   * 0xFEE00020 is the LAPIC-ID PHYSICAL address; access it through the
   * direct map (KERNEL_VIRT_BASE offset — identity while it is 0).  The
   * constant is open-coded instead of using phys_to_virt() to keep this
   * header free of include cycles with memlayout.h users. */
  return (*(volatile uint32_t *)(uintptr_t)(0xFEE00020UL + KERNEL_VIRT_BASE)) >> 24;
}

/* --- VMM / TLB --- */
static inline void arch_impl_set_pgd(uint64_t pgd) {
  __asm__ __volatile__("mov %0, %%cr3" ::"r"(pgd) : "memory");
}

static inline uint64_t arch_impl_get_pgd(void) {
  uint64_t pgd;
  __asm__ __volatile__("mov %%cr3, %0" : "=r"(pgd));
  return pgd;
}

/* On amd64 the kernel half lives in the same PML4 (indices 256..511), so
 * the "kernel root" accessors are simple aliases of the CR3 ones.  They
 * exist so shared code can address the kernel address-space root without
 * caring that aarch64 keeps it in a separate register (TTBR1). */
static inline void arch_impl_set_kernel_pgd(uint64_t pgd) {
  arch_impl_set_pgd(pgd);
}

static inline uint64_t arch_impl_get_kernel_pgd(void) {
  return arch_impl_get_pgd();
}

static inline void arch_impl_tlb_flush_local(void) {
  uint64_t cr3;
  __asm__ __volatile__("mov %%cr3, %0\n\t"
                       "mov %0, %%cr3"
                       : "=r"(cr3)::"memory");
}

static inline void arch_impl_tlb_flush_all(void) {
  arch_impl_tlb_flush_local();
}

static inline void arch_impl_tlb_flush_va(uintptr_t va) {
  __asm__ __volatile__("invlpg (%0)" ::"r"(va) : "memory");
}

/* SMP TLB shootdown (MM-VMM-05/AMMU-08): invlpg and CR3 reloads are strictly
 * LOCAL on x86 — there is no hardware broadcast like AArch64's TLBI *IS.
 * Implemented in kernel/arch/amd64/mm/tlb.c with a fixed-vector LAPIC IPI
 * (vector 0xFD) and bounded acknowledgement wait. */
void amd64_tlb_shootdown_va(uintptr_t va);
void amd64_tlb_shootdown_all(void);
void amd64_tlb_ipi_init(void);
static inline void arch_impl_tlb_shootdown_va(uintptr_t va) {
  amd64_tlb_shootdown_va(va);
}
static inline void arch_impl_tlb_shootdown_all(void) {
  amd64_tlb_shootdown_all();
}

/* Cache Control */
static inline void arch_impl_cache_clean_range(void *start, size_t size) {
  uint64_t s = (uint64_t)start;
  uint64_t e = s + size;
  s &= ~63UL;
  for (; s < e; s += 64) {
    __asm__ __volatile__("clflush (%0)" ::"r"(s) : "memory");
  }
}

static inline void arch_impl_cache_sync_icache(void *start, size_t size) {
  (void)start;
  (void)size;
  __asm__ __volatile__("mfence" ::: "memory");
}

/* --- Timer --- */
/* tsc_hz: measured TSC frequency, published by tsc_calibrate() in
 * kernel/arch/amd64/cpu/apic.c (declared there and in arch/amd64/apic.h;
 * re-declared here so this header has no include dependency on apic.h).
 * Zero until calibration runs on the BSP. */
extern uint64_t tsc_hz;

/* arch_impl_timer_get_freq - the HAL real-time-reference frequency (Hz).
 *
 * Returns the calibrated TSC frequency (tsc_hz).  Before tsc_calibrate() runs
 * on the BSP, tsc_hz is 0; this returns a SAFE 1 GHz fallback so early callers
 * of the arch-neutral mono_ns() (kernel/core/timer.c) never divide by zero.
 * After calibration the real measured value is returned. */
static inline uint64_t arch_impl_timer_get_freq(void) {
  return tsc_hz ? tsc_hz : 1000000000ULL; /* fallback until BSP calibration */
}

static inline uint64_t arch_impl_timer_get_count(void) {
  uint32_t lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

static inline void arch_impl_timer_set_compare(uint64_t val) { (void)val; }
static inline void arch_impl_timer_control(uint32_t val) { (void)val; }

/* --- Entropy (ISA primitive) --- */
/* arch_impl_hw_random: one attempt at RDRAND (the on-chip DRBG).  Returns 1 and
 * writes *out on success; 0 if CPUID.01h:ECX[30] does not advertise RDRAND (the
 * default QEMU CPU model) or the read transiently failed.  Pure ISA wrapper:
 * the retry/mix policy lives in the generic entropy layer (kernel/lib/entropy.c). */
static inline int arch_impl_hw_random(uint64_t *out) {
    uint32_t a, b, c, d;
    __asm__ __volatile__("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "0"(1u), "2"(0u));
    if (!(c & (1u << 30)))
        return 0; /* no RDRAND */
    uint64_t r;
    unsigned char ok;
    __asm__ __volatile__("rdrand %0; setc %1" : "=r"(r), "=qm"(ok));
    if (!ok)
        return 0; /* transient failure */
    *out = r;
    return 1;
}

/* --- Spinlocks --- */
static inline void arch_impl_spin_lock(volatile uint32_t *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) __asm__ __volatile__("pause");
    }
}

static inline void arch_impl_spin_unlock(volatile uint32_t *lock) {
    __sync_lock_release(lock);
}

static inline int arch_impl_spin_trylock(volatile uint32_t *lock) {
    return __sync_lock_test_and_set(lock, 1) == 0;
}

/* --- System Registers --- */
static inline uint64_t arch_impl_get_fault_address(void) {
  uint64_t cr2;
  __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
  return cr2;
}

static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t low, high;
  __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
  uint32_t low = (uint32_t)val;
  uint32_t high = (uint32_t)(val >> 32);
  __asm__ __volatile__("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t val;
  __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"(port));
  return val;
}

static inline void outw(uint16_t port, uint16_t val) {
  __asm__ __volatile__("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
  uint16_t val;
  __asm__ __volatile__("inw %1, %0" : "=a"(val) : "Nd"(port));
  return val;
}

static inline void outl(uint16_t port, uint32_t val) {
  __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
  uint32_t val;
  __asm__ __volatile__("inl %1, %0" : "=a"(val) : "Nd"(port));
  return val;
}

static inline uint64_t arch_impl_get_fault_status(void) {
  return 0; /* x86 uses error code on stack */
}

/* --- Constants --- */
#define HAL_RAM_START 0x0UL
#define HAL_RAM_SIZE  0x40000000UL
#define HAL_ALIAS_OFFSET 0x0UL

#endif /* _ARCH_AMD64_H */
