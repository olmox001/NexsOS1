#ifndef NXHAL_AMD64_ISA_H
#define NXHAL_AMD64_ISA_H

#include <stdint.h>

/*
 * AMD64 provider-private ISA access.
 *
 * All raw CPU state reads used by the provider and by HCD discovery flow
 * through these primitives.  NX-CORE never includes this header.
 */
static inline void arch_isa_cpuid(uint32_t leaf, uint32_t subleaf,
                                  uint32_t *eax, uint32_t *ebx,
                                  uint32_t *ecx, uint32_t *edx) {
  __asm__ __volatile__("cpuid"
                       : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                       : "a"(leaf), "c"(subleaf));
}

static inline uint32_t arch_isa_cpl(void) {
  uint16_t cs = 0U;
  __asm__ __volatile__("mov %%cs, %0" : "=r"(cs));
  return (uint32_t)cs & UINT32_C(3);
}

static inline uint64_t arch_isa_rflags(void) {
  uint64_t flags = 0U;
  __asm__ __volatile__("pushfq; popq %0" : "=r"(flags));
  return flags;
}

static inline uint64_t arch_isa_cr0(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("mov %%cr0, %0" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_cr4(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("mov %%cr4, %0" : "=r"(value));
  return value;
}

static inline void arch_isa_write_cr0(uint64_t value) {
  __asm__ __volatile__("mov %0, %%cr0" :: "r"(value) : "memory");
}

static inline void arch_isa_write_cr4(uint64_t value) {
  __asm__ __volatile__("mov %0, %%cr4" :: "r"(value) : "memory");
}

static inline uint64_t arch_isa_rdmsr(uint32_t msr) {
  uint32_t low = 0U;
  uint32_t high = 0U;
  __asm__ __volatile__("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
  return ((uint64_t)high << 32U) | (uint64_t)low;
}

static inline void arch_isa_wrmsr(uint32_t msr, uint64_t value) {
  uint32_t low = (uint32_t)value;
  uint32_t high = (uint32_t)(value >> 32U);
  __asm__ __volatile__("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}

#endif /* NXHAL_AMD64_ISA_H */
