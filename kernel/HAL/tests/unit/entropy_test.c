#include <hal/entropy.h>

#include <stddef.h>
#include <stdint.h>

static int hardware_status;
static int jitter_status;
static uint64_t hardware_value;
static uint64_t jitter_value;

int nxhal_provider_entropy_hardware_random(uint64_t *out_value) {
  if (hardware_status != NXHAL_ENTROPY_OK)
    return hardware_status;
  *out_value = hardware_value;
  return NXHAL_ENTROPY_OK;
}

int nxhal_provider_entropy_jitter_sample(uint64_t *out_value) {
  if (jitter_status != NXHAL_ENTROPY_OK)
    return jitter_status;
  *out_value = jitter_value;
  return NXHAL_ENTROPY_OK;
}

static void reset_mocks(void) {
  hardware_status = NXHAL_ENTROPY_OK;
  jitter_status = NXHAL_ENTROPY_OK;
  hardware_value = UINT64_C(0x1122334455667788);
  jitter_value = UINT64_C(0x8877665544332211);
}

static int test_hardware_random(void) {
  uint64_t value = 0U;

  reset_mocks();
  if (nxhal_entropy_hardware_random(&value) != NXHAL_ENTROPY_OK ||
      value != hardware_value)
    return 1;
  hardware_status = NXHAL_ENTROPY_E_UNAVAILABLE;
  if (nxhal_entropy_hardware_random(&value) != NXHAL_ENTROPY_E_UNAVAILABLE)
    return 2;
  return nxhal_entropy_hardware_random(NULL) == NXHAL_ENTROPY_E_NULL ? 0 : 3;
}

static int test_jitter_sample(void) {
  uint64_t value = 0U;

  reset_mocks();
  if (nxhal_entropy_jitter_sample(&value) != NXHAL_ENTROPY_OK ||
      value != jitter_value)
    return 1;
  jitter_status = NXHAL_ENTROPY_E_UNAVAILABLE;
  if (nxhal_entropy_jitter_sample(&value) != NXHAL_ENTROPY_E_UNAVAILABLE)
    return 2;
  return nxhal_entropy_jitter_sample(NULL) == NXHAL_ENTROPY_E_NULL ? 0 : 3;
}

int main(void) {
  if (test_hardware_random() != 0)
    return 1;
  if (test_jitter_sample() != 0)
    return 2;
  return 0;
}
