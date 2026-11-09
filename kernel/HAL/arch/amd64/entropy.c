#include <arch/arch.h>
#include <hal/provider.h>

#include <stddef.h>
#include <stdint.h>

int nxhal_provider_entropy_hardware_random(uint64_t *out_value) {
  if (out_value == NULL)
    return NXHAL_ENTROPY_E_NULL;
  return arch_impl_hw_random(out_value) != 0
             ? NXHAL_ENTROPY_OK : NXHAL_ENTROPY_E_UNAVAILABLE;
}

int nxhal_provider_entropy_jitter_sample(uint64_t *out_value) {
  if (out_value == NULL)
    return NXHAL_ENTROPY_E_NULL;
  *out_value = arch_impl_timer_get_count();
  return NXHAL_ENTROPY_OK;
}
