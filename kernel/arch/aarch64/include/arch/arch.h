#ifndef _ARCH_AARCH64_H
#define _ARCH_AARCH64_H

#include <stdint.h>

#include <kernel/types.h>

/* AArch64 specific implementations of architecture operations */

static inline void __arch_local_irq_enable(void) {
  __asm__ __volatile__("msr daifclr, #2" ::: "memory");
}

static inline void __arch_local_irq_disable(void) {
  __asm__ __volatile__("msr daifset, #2" ::: "memory");
}

static inline void __arch_local_irq_save(uint64_t *flags) {
  __asm__ __volatile__("mrs %0, daif\n"
                       "msr daifset, #2"
                       : "=r"(*flags)
                       :
                       : "memory");
}

static inline void __arch_local_irq_restore(uint64_t flags) {
  __asm__ __volatile__("msr daif, %0" ::"r"(flags) : "memory");
}

static inline void __arch_local_irq_save_all(uint64_t *flags) {
  __asm__ __volatile__("mrs %0, daif\n"
                       "msr daifset, #0xf"
                       : "=r"(*flags)
                       :
                       : "memory");
}

static inline void __arch_local_irq_restore_all(uint64_t flags) {
  __asm__ __volatile__("msr daif, %0" ::"r"(flags) : "memory");
}

static inline void __arch_nop(void) { __asm__ __volatile__("nop"); }

static inline void __arch_wfi(void) { __asm__ __volatile__("wfi"); }

static inline void __arch_wfe(void) { __asm__ __volatile__("wfe"); }

static inline void __arch_sev(void) { __asm__ __volatile__("sev"); }

static inline void __arch_isb(void) { __asm__ __volatile__("isb"); }

static inline void __arch_dsb(void) { __asm__ __volatile__("dsb sy"); }

static inline void __arch_dmb(void) { __asm__ __volatile__("dmb sy"); }

static inline uint32_t __arch_get_cpu_id(void) {
  uint64_t mpidr;
  __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));
  return (uint32_t)(mpidr & 0xFF);
}

static inline uint64_t __arch_get_esr(void) {
  uint64_t esr;
  __asm__ __volatile__("mrs %0, esr_el1" : "=r"(esr));
  return esr;
}

static inline uint64_t __arch_get_far(void) {
  uint64_t far;
  __asm__ __volatile__("mrs %0, far_el1" : "=r"(far));
  return far;
}

static inline uint64_t __arch_get_vbar(void) {
  uint64_t vbar;
  __asm__ __volatile__("mrs %0, vbar_el1" : "=r"(vbar));
  return vbar;
}

static inline void __arch_set_vbar(uint64_t vbar) {
  __asm__ __volatile__("msr vbar_el1, %0" ::"r"(vbar));
}

static inline uint64_t __arch_get_cpacr(void) {
  uint64_t cpacr;
  __asm__ __volatile__("mrs %0, cpacr_el1" : "=r"(cpacr));
  return cpacr;
}

static inline void __arch_set_cpacr(uint64_t cpacr) {
  __asm__ __volatile__("msr cpacr_el1, %0" ::"r"(cpacr));
}

static inline void __arch_set_ttbr0(uint64_t ttbr0) {
  __asm__ __volatile__("msr ttbr0_el1, %0" ::"r"(ttbr0));
}

static inline void __arch_tlb_flush_local(void) {
  __asm__ __volatile__("tlbi vmalle1 \n dsb ish \n isb");
}

static inline void __arch_tlb_flush_all(void) {
  __asm__ __volatile__("tlbi vmalle1is \n dsb ish \n isb");
}

static inline void __arch_tlb_flush_va(uint64_t va) {
  __asm__ __volatile__("tlbi vaae1is, %0 \n dsb ish \n isb" ::"r"(va >> 12));
}

static inline void __arch_clean_cache_va(void *va) {
  __asm__ __volatile__("dc cvac, %0" ::"r"(va) : "memory");
}

static inline void __arch_clean_cache_va_pou(void *va) {
  __asm__ __volatile__("dc cvau, %0" ::"r"(va) : "memory");
}

static inline void __arch_clean_cache_range_va(void *start, uint64_t size) {
  uint64_t s = (uint64_t)start;
  uint64_t e = s + size;
  /* Align to 64-byte cache line */
  s &= ~63UL;
  for (; s < e; s += 64) {
    __asm__ __volatile__("dc cvac, %0" ::"r"(s) : "memory");
  }
}

static inline uint64_t __arch_get_ttbr0(void) {
  uint64_t ttbr0;
  __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(ttbr0));
  return ttbr0;
}

static inline void __arch_set_mair(uint64_t mair) {
  __asm__ __volatile__("msr mair_el1, %0" ::"r"(mair));
}

static inline void __arch_set_tcr(uint64_t tcr) {
  __asm__ __volatile__("msr tcr_el1, %0" ::"r"(tcr));
}

static inline uint64_t __arch_get_sctlr(void) {
  uint64_t sctlr;
  __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(sctlr));
  return sctlr;
}

static inline void __arch_set_sctlr(uint64_t sctlr) {
  __asm__ __volatile__("msr sctlr_el1, %0" ::"r"(sctlr));
}

/* Spinlocks */
static inline void __arch_spin_lock(volatile uint32_t *lock) {
  uint32_t tmp;
  __asm__ __volatile__("   sevl\n"
                       "1: wfe\n"
                       "   ldaxr   %w0, [%1]\n"
                       "   cbnz    %w0, 1b\n"
                       "   stxr    %w0, %w2, [%1]\n"
                       "   cbnz    %w0, 1b\n"
                       : "=&r"(tmp)
                       : "r"(lock), "r"(1)
                       : "memory");
}

static inline void __arch_spin_unlock(volatile uint32_t *lock) {
  __asm__ __volatile__("   stlr    %w0, [%1]\n"
                       :
                       : "r"(0), "r"(lock)
                       : "memory");
}

static inline int __arch_spin_trylock(volatile uint32_t *lock) {
  uint32_t tmp;
  __asm__ __volatile__("   ldaxr   %w0, [%1]\n"
                       "   cbnz    %w0, 1f\n"
                       "   stxr    %w0, %w2, [%1]\n"
                       "1:\n"
                       : "=&r"(tmp)
                       : "r"(lock), "r"(1)
                       : "memory");
  return tmp == 0;
}

/* Timer Registers */
static inline uint64_t __arch_cntfrq_el0_read(void) {
  uint64_t val;
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(val));
  return val;
}

static inline uint64_t __arch_cntvct_el0_read(void) {
  uint64_t val;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
  return val;
}

static inline void __arch_cntv_cval_el0_write(uint64_t val) {
  __asm__ __volatile__("msr cntv_cval_el0, %0" ::"r"(val));
}

static inline void __arch_cntv_ctl_el0_write(uint64_t val) {
  __asm__ __volatile__("msr cntv_ctl_el0, %0" ::"r"(val));
}

#endif /* _ARCH_AARCH64_H */
