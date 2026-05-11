/*
 * kernel/arch/amd64/include/arch/arch.h
 * x86-64 architecture-specific inline primitives
 *
 * Provides the __arch_* functions consumed by kernel/include/kernel/arch.h
 */
#ifndef _ARCH_AMD64_H
#define _ARCH_AMD64_H

#include <stdint.h>
#include <kernel/types.h>

/* ─── Interrupt Control ─── */

static inline void __arch_local_irq_enable(void) {
  __asm__ __volatile__("sti" ::: "memory");
}

static inline void __arch_local_irq_disable(void) {
  __asm__ __volatile__("cli" ::: "memory");
}

static inline void __arch_local_irq_save(uint64_t *flags) {
  __asm__ __volatile__("pushfq\n\t"
                       "popq %0\n\t"
                       "cli"
                       : "=r"(*flags)
                       :
                       : "memory");
}

static inline void __arch_local_irq_restore(uint64_t flags) {
  __asm__ __volatile__("pushq %0\n\t"
                       "popfq"
                       :
                       : "r"(flags)
                       : "memory", "cc");
}

/* On x86-64, all exceptions/interrupts are controlled via RFLAGS.IF only.
 * _save_all / _restore_all / _disable_all are equivalent to the irq variants.
 */
static inline void __arch_local_irq_save_all(uint64_t *flags) {
  __arch_local_irq_save(flags);
}

static inline void __arch_local_irq_restore_all(uint64_t flags) {
  __arch_local_irq_restore(flags);
}

static inline void __arch_local_irq_disable_all(void) {
  __arch_local_irq_disable();
}

/* ─── CPU Control ─── */

static inline void __arch_nop(void) { __asm__ __volatile__("nop"); }

/* x86 equivalent of ARM WFI — halt until interrupt */
static inline void __arch_wfi(void) { __asm__ __volatile__("hlt"); }

/* x86 equivalent of ARM WFE — use PAUSE for spinloop hint */
static inline void __arch_wfe(void) { __asm__ __volatile__("pause"); }

/* No direct SEV equivalent on x86; NOP stub */
static inline void __arch_sev(void) { __asm__ __volatile__("nop"); }

static inline void __arch_yield(void) { __asm__ __volatile__("pause"); }

static inline void __arch_cpu_halt(void) {
  __arch_local_irq_disable();
  while (1) {
    __asm__ __volatile__("hlt");
  }
}

/* ─── CPU Identification ─── */

static inline uint32_t __arch_get_cpu_id(void) {
  /* Read LAPIC ID from IA32_APIC_BASE MSR or xAPIC MMIO.
   * For boot CPU, CPUID leaf 0x1 EBX[31:24] gives initial APIC ID.
   * Simplified: use CPUID for now. */
  uint32_t ebx;
  __asm__ __volatile__("cpuid" : "=b"(ebx) : "a"(1) : "ecx", "edx");
  return (ebx >> 24) & 0xFF;
}

/* ─── Memory Barriers ─── */

static inline void __arch_isb(void) {
  /* x86 has no ISB; serializing via CPUID or just MFENCE */
  __asm__ __volatile__("mfence" ::: "memory");
}

static inline void __arch_dsb(void) {
  __asm__ __volatile__("mfence" ::: "memory");
}

static inline void __arch_dmb(void) {
  __asm__ __volatile__("mfence" ::: "memory");
}

static inline void __arch_mb(void) {
  __asm__ __volatile__("mfence" ::: "memory");
}

static inline void __arch_rmb(void) {
  __asm__ __volatile__("lfence" ::: "memory");
}

static inline void __arch_wmb(void) {
  __asm__ __volatile__("sfence" ::: "memory");
}

/* ─── Page Table / TLB ─── */

static inline void __arch_set_ttbr0(uint64_t cr3_val) {
  __asm__ __volatile__("mov %0, %%cr3" ::"r"(cr3_val) : "memory");
}

static inline uint64_t __arch_get_ttbr0(void) {
  uint64_t cr3;
  __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
  return cr3;
}

static inline void __arch_tlb_flush_local(void) {
  uint64_t cr3;
  __asm__ __volatile__("mov %%cr3, %0\n\t"
                       "mov %0, %%cr3"
                       : "=r"(cr3)::"memory");
}

static inline void __arch_tlb_flush_all(void) {
  __arch_tlb_flush_local(); /* Single-core for now */
}

static inline void __arch_tlb_flush_va(uint64_t va) {
  __asm__ __volatile__("invlpg (%0)" ::"r"(va) : "memory");
}

/* ─── Cache Operations ─── */
/* x86 caches are generally coherent; these are stubs/CLFLUSHes */

static inline void __arch_clean_cache_va(void *va) {
  __asm__ __volatile__("clflush (%0)" ::"r"(va) : "memory");
}

static inline void __arch_clean_cache_va_pou(void *va) {
  __arch_clean_cache_va(va);
}

static inline void __arch_clean_cache_range_va(void *start, uint64_t size) {
  uint64_t s = (uint64_t)start;
  uint64_t e = s + size;
  s &= ~63UL;
  for (; s < e; s += 64) {
    __asm__ __volatile__("clflush (%0)" ::"r"(s) : "memory");
  }
}

/* ─── Spinlocks ─── */

static inline void __arch_spin_lock(volatile uint32_t *lock) {
  while (1) {
    uint32_t old = 1;
    __asm__ __volatile__("xchgl %0, %1"
                         : "=r"(old), "+m"(*lock)
                         : "0"(old)
                         : "memory");
    if (old == 0)
      break;
    /* Spin with PAUSE hint */
    __asm__ __volatile__("pause");
  }
}

static inline void __arch_spin_unlock(volatile uint32_t *lock) {
  __asm__ __volatile__("" ::: "memory"); /* compiler barrier */
  *lock = 0;
}

static inline int __arch_spin_trylock(volatile uint32_t *lock) {
  uint32_t old = 1;
  __asm__ __volatile__("xchgl %0, %1"
                       : "=r"(old), "+m"(*lock)
                       : "0"(old)
                       : "memory");
  return old == 0; /* 1 if acquired, 0 if contended */
}

/* ─── Timer ─── */
/* x86-64 uses APIC timer or TSC; provide TSC-based stubs */

static inline uint64_t __arch_cntfrq_el0_read(void) {
  /* TSC frequency; placeholder — calibrated at boot */
  return 1000000000ULL; /* 1 GHz default */
}

static inline uint64_t __arch_cntvct_el0_read(void) {
  uint32_t lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

static inline void __arch_cntv_cval_el0_write(uint64_t val) {
  /* APIC timer compare — implemented in apic.c */
  (void)val;
}

static inline void __arch_cntv_ctl_el0_write(uint64_t val) {
  /* APIC timer control — implemented in apic.c */
  (void)val;
}

/* ─── MSR Access ─── */

static inline uint64_t rdmsr(uint32_t msr) {
  uint32_t lo, hi;
  __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
  return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
  __asm__ __volatile__("wrmsr" ::"c"(msr), "a"((uint32_t)val),
                       "d"((uint32_t)(val >> 32)));
}

/* ─── Port I/O ─── */

static inline void outb(uint16_t port, uint8_t val) {
  __asm__ __volatile__("outb %0, %1" ::"a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
  uint8_t ret;
  __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
  __asm__ __volatile__("outw %0, %1" ::"a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
  uint16_t ret;
  __asm__ __volatile__("inw %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
  __asm__ __volatile__("outl %0, %1" ::"a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
  uint32_t ret;
  __asm__ __volatile__("inl %1, %0" : "=a"(ret) : "Nd"(port));
  return ret;
}

/* ─── SMAP Control ─── */

static inline void stac(void) {
  __asm__ __volatile__(".byte 0x0f, 0x01, 0xcb" ::: "memory"); /* stac */
}

static inline void clac(void) {
  __asm__ __volatile__(".byte 0x0f, 0x01, 0xca" ::: "memory"); /* clac */
}

/* ─── x86-64 specific system registers (mapped via kernel/arch.h) ─── */
/* These ARM-named wrappers are stubs to satisfy arch.h macros.
 * The actual x86 register manipulation happens via dedicated functions. */

static inline uint64_t __arch_get_esr(void) { return 0; } /* No ESR on x86 */
static inline uint64_t __arch_get_far(void) {
  uint64_t cr2;
  __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
  return cr2; /* CR2 = faulting address */
}

static inline uint64_t __arch_get_vbar(void) { return 0; } /* No VBAR on x86 */
static inline void __arch_set_vbar(uint64_t vbar) {
  (void)vbar;
} /* IDT managed via lidt */

static inline uint64_t __arch_get_cpacr(void) { return 0; }
static inline void __arch_set_cpacr(uint64_t v) { (void)v; }
static inline void __arch_set_mair(uint64_t v) { (void)v; }
static inline void __arch_set_tcr(uint64_t v) { (void)v; }
static inline uint64_t __arch_get_sctlr(void) {
  uint64_t cr0;
  __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
  return cr0;
}
static inline void __arch_set_sctlr(uint64_t v) {
  __asm__ __volatile__("mov %0, %%cr0" ::"r"(v) : "memory");
}

#define ARCH_MEMORY_BASE 0x0UL

#endif /* _ARCH_AMD64_H */
