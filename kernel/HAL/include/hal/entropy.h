#ifndef NXHAL_ENTROPY_H
#define NXHAL_ENTROPY_H

#include <stdint.h>

#define NXHAL_ENTROPY_ABI_VERSION UINT32_C(1)

enum nxhal_entropy_status {
  NXHAL_ENTROPY_OK = 0,
  NXHAL_ENTROPY_E_NULL = -1,
  NXHAL_ENTROPY_E_UNAVAILABLE = -2
};

/*
 * Hardware random is a best-effort sample from an architected RNG.
 * Jitter sample is an early-boot changing hardware counter value used only as
 * entropy material. It is not a clocksource and carries no time/frequency
 * semantics.
 */
int nxhal_entropy_hardware_random(uint64_t *out_value);
int nxhal_entropy_jitter_sample(uint64_t *out_value);

#endif /* NXHAL_ENTROPY_H */
