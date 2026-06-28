#ifndef _ARCH_AARCH64_H
#define _ARCH_AARCH64_H

#include <kernel/types.h>
#include <stdint.h>

/* AArch64 HAL Implementation Primitives */
#include <kernel/elf.h>
#define ARCH_TYPE EM_AARCH64

/* --- Interrupt Control --- */
static inline void arch_impl_irq_enable(void) {
  __asm__ __volatile__("msr daifclr, #2" ::: "memory");
}

static inline void arch_impl_irq_disable(void) {
  __asm__ __volatile__("msr daifset, #2" ::: "memory");
}

static inline void arch_impl_irq_save(uint64_t *flags) {
  __asm__ __volatile__("mrs %0, daif\n"
                       "msr daifset, #2"
                       : "=r"(*flags)
                       :
                       : "memory");
}

static inline void arch_impl_irq_restore(uint64_t flags) {
  __asm__ __volatile__("msr daif, %0" ::"r"(flags) : "memory");
}

static inline void arch_impl_irq_save_all(uint64_t *flags) {
  __asm__ __volatile__("mrs %0, daif\n"
                       "msr daifset, #0xf"
                       : "=r"(*flags)
                       :
                       : "memory");
}

static inline void arch_impl_irq_restore_all(uint64_t flags) {
  __asm__ __volatile__("msr daif, %0" ::"r"(flags) : "memory");
}

static inline void arch_impl_irq_disable_all(void) {
  __asm__ __volatile__("msr daifset, #0xf" ::: "memory");
}

/* --- CPU Control --- */
static inline void arch_impl_nop(void) { __asm__ __volatile__("nop"); }
static inline void arch_impl_idle(void) { __asm__ __volatile__("wfi"); }
static inline void arch_impl_yield(void) { __asm__ __volatile__("yield"); }
static inline void arch_impl_cpu_notify(void) { __asm__ __volatile__("sev"); }

/* arch_impl_reboot - PSCI SYSTEM_RESET (function 0x84000009) over the HVC
 * conduit (the same conduit the SMP bring-up uses for PSCI CPU_ON) — DIR-05 #139
 * watchdog.  Used by the HAL arch_reboot() wrapper. */
static inline void arch_impl_reboot(void) {
  register unsigned long x0 __asm__("x0") = 0x84000009UL; /* PSCI SYSTEM_RESET */
  __asm__ __volatile__("hvc #0" : "+r"(x0) : : "memory", "x1", "x2", "x3");
}

/* Barriers */
static inline void arch_impl_isb(void) {
  __asm__ __volatile__("isb" ::: "memory");
}
static inline void arch_impl_mb(void) {
  __asm__ __volatile__("dsb sy" ::: "memory");
}
static inline void arch_impl_rmb(void) {
  __asm__ __volatile__("dsb ld" ::: "memory");
}
static inline void arch_impl_wmb(void) {
  __asm__ __volatile__("dsb st" ::: "memory");
}

static inline uint32_t arch_impl_get_cpu_id(void) {
  uint64_t mpidr;
  __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));
  return (uint32_t)(mpidr & 0xFF);
}

/* --- VMM / TLB --- */
/* TTBR0 = USER half (per-process tables, VA bit 47 clear); TTBR1 = KERNEL
 * half (higher-half image + direct map, VA top bits set — see memlayout.h).
 * arch_impl_set_pgd/get_pgd keep their historical meaning of "the process
 * address-space root" (TTBR0); the kernel root has its own accessors. */
static inline void arch_impl_set_pgd(uint64_t pgd) {
  __asm__ __volatile__("msr ttbr0_el1, %0" ::"r"(pgd));
}

static inline uint64_t arch_impl_get_pgd(void) {
  uint64_t pgd;
  __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(pgd));
  return pgd;
}

static inline void arch_impl_set_kernel_pgd(uint64_t pgd) {
  __asm__ __volatile__("msr ttbr1_el1, %0" ::"r"(pgd));
}

static inline uint64_t arch_impl_get_kernel_pgd(void) {
  uint64_t pgd;
  __asm__ __volatile__("mrs %0, ttbr1_el1" : "=r"(pgd));
  return pgd;
}

static inline void arch_impl_tlb_flush_local(void) {
  __asm__ __volatile__("tlbi vmalle1 \n dsb ish \n isb");
}

static inline void arch_impl_tlb_flush_all(void) {
  __asm__ __volatile__("tlbi vmalle1is \n dsb ish \n isb");
}

static inline void arch_impl_tlb_flush_va(uintptr_t va) {
  __asm__ __volatile__("tlbi vaae1is, %0 \n dsb ish \n isb" ::"r"(va >> 12));
}

/* SMP TLB shootdown (MM-VMM-05/AMMU-08): the two flushes above use the
 * *IS (inner-shareable) TLBI variants, which the hardware DVM broadcasts to
 * every PE in the inner-shareable domain; the DSB ISH then waits for
 * completion on ALL PEs, not just the local one.  The cross-CPU shootdown
 * contract is therefore already satisfied without IPIs — these are aliases,
 * not stubs. */
static inline void arch_impl_tlb_shootdown_va(uintptr_t va) {
  arch_impl_tlb_flush_va(va);
}
static inline void arch_impl_tlb_shootdown_all(void) {
  arch_impl_tlb_flush_all();
}

/* Cache Control */
static inline void arch_impl_cache_clean_range(void *start, size_t size) {
  uint64_t s = (uint64_t)start;
  uint64_t e = s + size;
  s &= ~63UL;
  for (; s < e; s += 64) {
    __asm__ __volatile__("dc cvac, %0" ::"r"(s) : "memory");
  }
}

static inline void arch_impl_cache_sync_icache(void *start, size_t size) {
  uint64_t s = (uint64_t)start;
  uint64_t e = s + size;
  s &= ~63UL;
  for (; s < e; s += 64) {
    __asm__ __volatile__("dc cvau, %0\n"
                         "ic ivau, %0" ::"r"(s)
                         : "memory");
  }
  __asm__ __volatile__("dsb ish\n isb" ::: "memory");
}

/* --- System Registers --- */
static inline uint64_t arch_impl_get_fault_address(void) {
  uint64_t far;
  __asm__ __volatile__("mrs %0, far_el1" : "=r"(far));
  return far;
}

static inline uint64_t arch_impl_get_fault_status(void) {
  uint64_t esr;
  __asm__ __volatile__("mrs %0, esr_el1" : "=r"(esr));
  return esr;
}

static inline uint64_t arch_impl_get_cpacr(void) {
  uint64_t val;
  __asm__ __volatile__("mrs %0, cpacr_el1" : "=r"(val));
  return val;
}

static inline void arch_impl_set_cpacr(uint64_t val) {
  __asm__ __volatile__("msr cpacr_el1, %0" ::"r"(val));
}

static inline void arch_impl_set_mair(uint64_t val) {
  __asm__ __volatile__("msr mair_el1, %0" ::"r"(val));
}

static inline void arch_impl_set_tcr(uint64_t val) {
  __asm__ __volatile__("msr tcr_el1, %0" ::"r"(val));
}

static inline uint64_t arch_impl_get_sctlr(void) {
  uint64_t val;
  __asm__ __volatile__("mrs %0, sctlr_el1" : "=r"(val));
  return val;
}

static inline void arch_impl_set_sctlr(uint64_t val) {
  __asm__ __volatile__("msr sctlr_el1, %0" ::"r"(val));
}

static inline uint64_t arch_impl_get_vbar(void) {
  uint64_t val;
  __asm__ __volatile__("mrs %0, vbar_el1" : "=r"(val));
  return val;
}

static inline void arch_impl_set_vbar(uint64_t val) {
  __asm__ __volatile__("msr vbar_el1, %0" ::"r"(val));
}
/* --- Timer --- */
static inline uint64_t arch_impl_timer_get_freq(void) {
  uint64_t val;
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(val));
  return val;
}

static inline uint64_t arch_impl_timer_get_count(void) {
  uint64_t val;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
  return val;
}

static inline void arch_impl_timer_set_compare(uint64_t val) {
  __asm__ __volatile__("msr cntv_cval_el0, %0" ::"r"(val));
}

static inline void arch_impl_timer_control(uint32_t val) {
  __asm__ __volatile__("msr cntv_ctl_el0, %0" ::"r"((uint64_t)val));
}

/* --- Entropy (ISA primitive) --- */
/* arch_impl_hw_random: one attempt at the architected RNG (FEAT_RNG / RNDR).
 * Returns 1 and writes *out on success; 0 if FEAT_RNG is absent (e.g. the
 * QEMU virt cortex-a57 default, which is ARMv8.0) or the read transiently
 * failed.  Pure ISA wrapper: the retry/mix policy lives in the generic
 * entropy layer (kernel/lib/entropy.c). */
static inline int arch_impl_hw_random(uint64_t *out) {
  uint64_t isar0;
  __asm__ __volatile__("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
  if (((isar0 >> 60) & 0xf) == 0)
    return 0; /* no FEAT_RNG */
  uint64_t r, nzcv;
  __asm__ __volatile__("mrs %0, s3_3_c2_c4_0\n\t" /* RNDR */
                       "mrs %1, nzcv\n\t"
                       : "=r"(r), "=r"(nzcv));
  if (nzcv & (1ULL << 30))
    return 0; /* PSTATE.Z set => transient failure */
  *out = r;
  return 1;
}

/* --- Spinlocks --- */
static inline void arch_impl_spin_lock(volatile uint32_t *lock) {
  while (__sync_lock_test_and_set(lock, 1)) {
    while (*lock)
      __asm__ __volatile__("wfe");
  }
}

static inline void arch_impl_spin_unlock(volatile uint32_t *lock) {
  __sync_lock_release(lock);
}

static inline int arch_impl_spin_trylock(volatile uint32_t *lock) {
  return __sync_lock_test_and_set(lock, 1) == 0;
}

/* --- Constants --- */
#define HAL_RAM_START 0x40000000UL
#define HAL_RAM_SIZE 0x80000000UL /* 2GB */
#define HAL_ALIAS_OFFSET 0x40000000UL

#endif /* _ARCH_AARCH64_H */
