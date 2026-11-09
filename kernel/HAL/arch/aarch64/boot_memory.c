#include <hal/boot_info.h>
#include <hal/boot_memory.h>
#include <hal/config.h>

#include <common/boot_memory_record.h>

#include <stdint.h>

static int nxhal_boot_span_valid(uint64_t base, uint64_t length) {
  return length != 0U && base < HAL_PHYS_MAP_LIMIT &&
         length <= HAL_PHYS_MAP_LIMIT - base;
}

static int nxhal_boot_fdt_handoff_valid(void) {
  const struct nxhal_boot_info *info = &nxhal_boot_info;

  if ((info->flags & (uint32_t)NXHAL_BOOT_FLAG_FDT_SIZE_VALID) == 0U &&
      (info->flags & (uint32_t)NXHAL_BOOT_FLAG_LEGACY_TRUSTED_INPUT) == 0U &&
      (info->flags & (uint32_t)NXHAL_BOOT_FLAG_LEGACY_SEARCH) == 0U)
    return 0;
  return nxhal_boot_span_valid(info->fdt_phys, info->fdt_size);
}

static int nxhal_boot_memory_handoff_valid(void) {
  uint64_t bytes;
  const struct nxhal_boot_info *info = &nxhal_boot_info;

  if ((info->flags & (uint32_t)NXHAL_BOOT_FLAG_UEFI_MAP_VALID) == 0U ||
      info->source != NXHAL_BOOT_SOURCE_AARCH64_EFI ||
      info->firmware_map_record_count == 0U ||
      info->firmware_map_record_count > NXHAL_BOOT_MEMORY_MAX_RECORDS ||
      info->firmware_map_record_size != NXHAL_UEFI_MEMORY_RECORD_SIZE)
    return 0;
  bytes = (uint64_t)info->firmware_map_record_count *
          (uint64_t)info->firmware_map_record_size;
  return nxhal_boot_span_valid(info->firmware_map_phys, bytes);
}

static const uint8_t *nxhal_boot_memory_wire(void) {
  /* The wire ABI stores a physical address. Converting that PA into the
   * provider's direct-map VA is the required MMU boundary operation, not a
   * recoverable integer/pointer representation choice. */
  return (const uint8_t *)(uintptr_t)( /* NOLINT(performance-no-int-to-ptr) */
      HAL_DIRECT_MAP_BASE + nxhal_boot_info.firmware_map_phys);
}

uint32_t nxhal_boot_memory_count(void) {
  if (!nxhal_boot_memory_handoff_valid())
    return 0U;
  return nxhal_boot_info.firmware_map_record_count;
}

enum nxhal_boot_memory_status nxhal_boot_memory_get(
    uint32_t index, struct nxhal_boot_memory_region *out) {
  const uint8_t *record;
  enum nxhal_boot_memory_status status;

  if (out == NULL)
    return NXHAL_BOOT_MEMORY_E_INVALID;
  if (!nxhal_boot_memory_handoff_valid())
    return NXHAL_BOOT_MEMORY_E_UNAVAILABLE;
  if (index >= nxhal_boot_info.firmware_map_record_count)
    return NXHAL_BOOT_MEMORY_E_RANGE;

  record = nxhal_boot_memory_wire() +
      ((uint64_t)index * NXHAL_BOOT_MEMORY_WIRE_RECORD_SIZE);
  status = nxhal_boot_memory_decode_hcd_record(record, out);
  if (status != NXHAL_BOOT_MEMORY_OK)
    return status;
  if (!nxhal_boot_span_valid(out->base, out->length))
    return NXHAL_BOOT_MEMORY_E_RANGE;
  return NXHAL_BOOT_MEMORY_OK;
}

uint32_t nxhal_boot_live_span_count(void) {
  uint32_t count = nxhal_boot_fdt_handoff_valid() ? 1U : 0U;

  if (nxhal_boot_memory_handoff_valid())
    ++count;
  return count;
}

enum nxhal_boot_memory_status nxhal_boot_live_span_get(
    uint32_t index, struct nxhal_boot_live_span *out) {
  uint64_t bytes;

  if (out == NULL)
    return NXHAL_BOOT_MEMORY_E_INVALID;
  if (nxhal_boot_fdt_handoff_valid()) {
    if (index == 0U) {
      *out = (struct nxhal_boot_live_span){
          nxhal_boot_info.fdt_phys, nxhal_boot_info.fdt_size,
          NXHAL_BOOT_LIVE_FDT, 0U};
      return NXHAL_BOOT_MEMORY_OK;
    }
    --index;
  }
  if (!nxhal_boot_memory_handoff_valid() || index != 0U)
    return NXHAL_BOOT_MEMORY_E_RANGE;
  bytes = (uint64_t)nxhal_boot_info.firmware_map_record_count *
          (uint64_t)nxhal_boot_info.firmware_map_record_size;
  *out = (struct nxhal_boot_live_span){
      nxhal_boot_info.firmware_map_phys, bytes,
      NXHAL_BOOT_LIVE_FIRMWARE_MAP, 0U};
  return NXHAL_BOOT_MEMORY_OK;
}
