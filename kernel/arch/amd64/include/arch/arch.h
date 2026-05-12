#ifndef _ARCH_AMD64_H
#define _ARCH_AMD64_H

#include <stdint.h>
#include <kernel/types.h>

/* AMD64 HAL Implementation Primitives */

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
  uint32_t ebx;
  __asm__ __volatile__("cpuid" : "=b"(ebx) : "a"(1) : "ecx", "edx");
  return (ebx >> 24) & 0xFF;
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
static inline uint64_t arch_impl_timer_get_freq(void) {
  return 1000000000ULL; /* Calibrated at boot usually */
}

static inline uint64_t arch_impl_timer_get_count(void) {
  uint32_t lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

static inline void arch_impl_timer_set_compare(uint64_t val) { (void)val; }
static inline void arch_impl_timer_control(uint32_t val) { (void)val; }

/* --- Spinlocks --- */
static inline void arch_impl_spin_lock(volatile uint32_t *lock) {
  while (1) {
    uint32_t old = 1;
    __asm__ __volatile__("xchgl %0, %1" : "=r"(old), "+m"(*lock) : "0"(old) : "memory");
    if (old == 0) break;
    __asm__ __volatile__("pause");
  }
}

static inline void arch_impl_spin_unlock(volatile uint32_t *lock) {
  __asm__ __volatile__("" ::: "memory");
  *lock = 0;
}

static inline int arch_impl_spin_trylock(volatile uint32_t *lock) {
  uint32_t old = 1;
  __asm__ __volatile__("xchgl %0, %1" : "=r"(old), "+m"(*lock) : "0"(old) : "memory");
  return old == 0;
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
