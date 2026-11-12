/*
 * Unified early-boot entropy policy.
 *
 * The HAL entropy ABI deliberately separates architected hardware randomness
 * from a changing early counter sample. The latter is entropy material only;
 * it is not a runtime clocksource and carries no frequency/time semantics.
 */
#include <hal/entropy.h>
#include <kernel/entropy.h>

#include <stdint.h>

enum {
  ENTROPY_HW_ATTEMPTS = 16,
  ENTROPY_JITTER_SAMPLES = 8
};

static uint64_t splitmix64(uint64_t *state) {
  uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
  z = (z ^ (z >> 30U)) * UINT64_C(0xbf58476d1ce4e5b9);
  z = (z ^ (z >> 27U)) * UINT64_C(0x94d049bb133111eb);
  return z ^ (z >> 31U);
}

static struct entropy_sample entropy_jitter_fallback(void) {
  uint64_t state = (uint64_t)(uintptr_t)__builtin_return_address(0);
  struct entropy_sample sample = {.source = ENTROPY_SOURCE_ADDRESS_MIX};

  for (unsigned int index = 0U; index < ENTROPY_JITTER_SAMPLES; ++index) {
    uint64_t jitter = 0U;
    if (nxhal_entropy_jitter_sample(&jitter) == NXHAL_ENTROPY_OK) {
      state ^= jitter;
      sample.source = ENTROPY_SOURCE_COUNTER_MIX;
    }
    (void)splitmix64(&state);
  }
  sample.value = splitmix64(&state);
  return sample;
}

struct entropy_sample entropy_sample_u64(void) {
  for (unsigned int attempt = 0U; attempt < ENTROPY_HW_ATTEMPTS; ++attempt) {
    uint64_t value = 0U;
    if (nxhal_entropy_hardware_random(&value) == NXHAL_ENTROPY_OK)
      return (struct entropy_sample){value, ENTROPY_SOURCE_ARCH_RNG};
  }
  return entropy_jitter_fallback();
}

uint64_t entropy_u64(void) {
  return entropy_sample_u64().value;
}
