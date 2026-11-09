#include <hal/boot_memory.h>

#include <stdint.h>

uint32_t nxhal_boot_memory_count(void) {
  return 0U;
}

enum nxhal_boot_memory_status nxhal_boot_memory_get(
    uint32_t index, struct nxhal_boot_memory_region *out) {
  (void)index;
  (void)out;
  return NXHAL_BOOT_MEMORY_E_UNAVAILABLE;
}

uint32_t nxhal_boot_live_span_count(void) {
  return 0U;
}

enum nxhal_boot_memory_status nxhal_boot_live_span_get(
    uint32_t index, struct nxhal_boot_live_span *out) {
  (void)index;
  (void)out;
  return NXHAL_BOOT_MEMORY_E_UNAVAILABLE;
}
