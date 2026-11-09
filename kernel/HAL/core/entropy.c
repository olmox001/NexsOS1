#include <hal/entropy.h>
#include <hal/provider.h>

#include <stddef.h>
#include <stdint.h>

int nxhal_entropy_hardware_random(uint64_t *out_value) {
  if (out_value == NULL)
    return NXHAL_ENTROPY_E_NULL;
  return nxhal_provider_entropy_hardware_random(out_value);
}

int nxhal_entropy_jitter_sample(uint64_t *out_value) {
  if (out_value == NULL)
    return NXHAL_ENTROPY_E_NULL;
  return nxhal_provider_entropy_jitter_sample(out_value);
}
