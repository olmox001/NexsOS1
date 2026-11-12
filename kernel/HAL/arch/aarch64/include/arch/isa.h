#ifndef NXHAL_AARCH64_ISA_H
#define NXHAL_AARCH64_ISA_H

#include <stdint.h>

/*
 * AArch64 provider-private ISA access.
 *
 * The HCD discovery adapter and the runtime provider share these readers so
 * there is one hardware access path. NX-CORE never includes this header.
 */
static inline uint64_t arch_isa_current_el(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("mrs %0, CurrentEL" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_midr(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("mrs %0, MIDR_EL1" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_mpidr(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("mrs %0, MPIDR_EL1" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_pfr0(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("mrs %0, ID_AA64PFR0_EL1" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_isar0(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("mrs %0, ID_AA64ISAR0_EL1" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_mmfr0(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("mrs %0, ID_AA64MMFR0_EL1" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_mmfr1(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("mrs %0, ID_AA64MMFR1_EL1" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_mmfr2(void) {
  uint64_t value = 0U;
  /* Architectural encoding keeps the conservative ARMv8.0 assembler target. */
  __asm__ __volatile__("mrs %0, S3_0_C0_C7_2" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_ctr(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("mrs %0, CTR_EL0" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_cntfrq(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("mrs %0, CNTFRQ_EL0" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_daif(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("mrs %0, DAIF" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_sctlr(uint32_t el) {
  uint64_t value = 0U;
  if (el == 1U)
    __asm__ __volatile__("mrs %0, SCTLR_EL1" : "=r"(value));
  else if (el == 2U)
    __asm__ __volatile__("mrs %0, SCTLR_EL2" : "=r"(value));
  else if (el == 3U)
    __asm__ __volatile__("mrs %0, SCTLR_EL3" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_tcr(uint32_t el) {
  uint64_t value = 0U;
  if (el == 1U)
    __asm__ __volatile__("mrs %0, TCR_EL1" : "=r"(value));
  else if (el == 2U)
    __asm__ __volatile__("mrs %0, TCR_EL2" : "=r"(value));
  else if (el == 3U)
    __asm__ __volatile__("mrs %0, TCR_EL3" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_mair(uint32_t el) {
  uint64_t value = 0U;
  if (el == 1U)
    __asm__ __volatile__("mrs %0, MAIR_EL1" : "=r"(value));
  else if (el == 2U)
    __asm__ __volatile__("mrs %0, MAIR_EL2" : "=r"(value));
  else if (el == 3U)
    __asm__ __volatile__("mrs %0, MAIR_EL3" : "=r"(value));
  return value;
}

static inline uint64_t arch_isa_pc(void) {
  uint64_t value = 0U;
  __asm__ __volatile__("adr %0, 1f\n1:" : "=r"(value));
  return value;
}

#endif /* NXHAL_AARCH64_ISA_H */
