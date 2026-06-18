/*
 * kernel/lib/entropy.c
 * Unified best-effort entropy source.
 *
 * ASTRA (docs/ASTRA.md §1 rule 5: "the HAL is the seam, and divergence is a
 * bug"): the policy here — prefer the hardware RNG, otherwise mix cycle-counter
 * jitter — is written once and drives both architectures identically over two
 * HAL primitives:
 *   - arch_hw_random()      (ISA: RNDR on AArch64 FEAT_RNG, RDRAND on AMD64)
 *   - arch_timer_get_count()(ISA: CNTVCT_EL0 / RDTSC, already a HAL contract)
 * No arch is named below; the only per-arch code is the thin ISA wrapper
 * arch_impl_hw_random in each kernel/arch/<arch>/include/arch/arch.h.
 *
 * Primary consumer: stack_guard_init() reseeds the SSP canary (LIB-SSP-01 / #71).
 */
#include <kernel/arch.h>
#include <kernel/entropy.h>
#include <stdint.h>

/* splitmix64 finalizer/step — a fast, well-distributed bit mixer. */
static inline uint64_t splitmix64(uint64_t *state) {
  uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

uint64_t entropy_u64(void) {
  /* Prefer the hardware RNG; tolerate a bounded number of transient failures. */
  for (int i = 0; i < 16; i++) {
    uint64_t hw;
    if (arch_hw_random(&hw))
      return hw;
  }

  /* Fallback: fold several cycle-counter samples (boot-time scheduling jitter)
   * and a frame address through splitmix64. */
  uint64_t state = (uint64_t)__builtin_return_address(0);
  for (int i = 0; i < 8; i++) {
    state ^= arch_timer_get_count();
    (void)splitmix64(&state);
  }
  return splitmix64(&state);
}
