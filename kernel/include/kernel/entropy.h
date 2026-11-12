#ifndef _KERNEL_ENTROPY_H
#define _KERNEL_ENTROPY_H

#include <stdint.h>

enum entropy_source {
  ENTROPY_SOURCE_ARCH_RNG,
  ENTROPY_SOURCE_COUNTER_MIX,
  ENTROPY_SOURCE_ADDRESS_MIX
};

struct entropy_sample {
  uint64_t value;
  enum entropy_source source;
};

/* Source provenance is not an entropy estimate or a cryptographic guarantee.
 * The early counter/address fallback has no qualified minimum strength. */
struct entropy_sample entropy_sample_u64(void);

/*
 * entropy_u64 - 64 bits of best-effort entropy, unified across architectures.
 *
 * One policy drives both architectures. It first requests architected hardware
 * randomness through <hal/entropy.h>; when unavailable it mixes bounded early
 * jitter samples from the same entropy ABI. Jitter samples are explicitly not
 * runtime timekeeping values, so early SSP reseeding does not depend on the
 * clocksource having been calibrated or published.
 */
uint64_t entropy_u64(void);

#endif /* _KERNEL_ENTROPY_H */
