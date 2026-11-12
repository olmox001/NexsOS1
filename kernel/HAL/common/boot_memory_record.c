#include <common/boot_memory_record.h>
#include <common/hcd_endian.h>
#include <hal/boot_memory.h>

#include <stdint.h>

static int nxhal_memory_type_valid(uint32_t type) {
  return type >= (uint32_t)NXHAL_BOOT_MEMORY_USABLE &&
         type <= (uint32_t)NXHAL_BOOT_MEMORY_UNACCEPTED;
}

enum nxhal_boot_memory_status nxhal_boot_memory_decode_hcd_record(
    const uint8_t *record, struct nxhal_boot_memory_region *out) {
  if (record == NULL || out == NULL)
    return NXHAL_BOOT_MEMORY_E_INVALID;

  out->base = hcd_load_le64(record, 0U);
  out->length = hcd_load_le64(record, 8U);
  out->type = hcd_load_le32(record, 16U);
  out->attributes = hcd_load_le32(record, 20U);
  out->numa_node_id = hcd_load_le32(record, 24U);
  out->owner = hcd_load_le32(record, 28U);
  if (hcd_load_le64(record, 32U) != 0U || out->length == 0U ||
      !nxhal_memory_type_valid(out->type) ||
      out->base > UINT64_MAX - out->length)
    return NXHAL_BOOT_MEMORY_E_INVALID;
  return NXHAL_BOOT_MEMORY_OK;
}
