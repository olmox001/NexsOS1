#ifndef NXHAL_BOOT_MEMORY_H
#define NXHAL_BOOT_MEMORY_H

#include <stdint.h>

/*
 * NX-HAL boot-memory ABI v1.
 *
 * NXHAL_BOOT_MEMORY_MAX_RECORDS is the HCD v1 wire-format bound. It is not a
 * machine RAM limit: producers must coalesce only semantically identical
 * adjacent records and reject/recover rather than truncate if the format bound
 * cannot represent a firmware map.
 */
#define NXHAL_BOOT_MEMORY_ABI_VERSION UINT32_C(1)
#define NXHAL_BOOT_MEMORY_MAX_RECORDS UINT32_C(4096)

enum nxhal_boot_memory_type {
  NXHAL_BOOT_MEMORY_USABLE = 1,
  NXHAL_BOOT_MEMORY_RESERVED = 2,
  NXHAL_BOOT_MEMORY_FIRMWARE_RUNTIME = 3,
  NXHAL_BOOT_MEMORY_ACPI_RECLAIMABLE = 4,
  NXHAL_BOOT_MEMORY_ACPI_NVS = 5,
  NXHAL_BOOT_MEMORY_MMIO = 6,
  NXHAL_BOOT_MEMORY_PERSISTENT = 7,
  NXHAL_BOOT_MEMORY_UNUSABLE = 8,
  NXHAL_BOOT_MEMORY_BOOTSTRAP = 9,
  NXHAL_BOOT_MEMORY_UNACCEPTED = 10
};

struct nxhal_boot_memory_region {
  uint64_t base;
  uint64_t length;
  uint32_t type;
  uint32_t attributes;
  uint32_t numa_node_id;
  uint32_t owner;
};

enum nxhal_boot_memory_status {
  NXHAL_BOOT_MEMORY_OK = 0,
  NXHAL_BOOT_MEMORY_E_UNAVAILABLE = -1,
  NXHAL_BOOT_MEMORY_E_INVALID = -2,
  NXHAL_BOOT_MEMORY_E_RANGE = -3
};

enum nxhal_boot_live_span_kind {
  NXHAL_BOOT_LIVE_FDT = 1,
  NXHAL_BOOT_LIVE_FIRMWARE_MAP = 2,
  NXHAL_BOOT_LIVE_ROLE_COUNT = 2
};

struct nxhal_boot_live_span {
  uint64_t base;
  uint64_t length;
  uint32_t kind;
  uint32_t reserved;
};

/* Returns a validated immutable record count, or zero when the active boot
 * provider has no HAL-owned map (for example the direct FDT compatibility
 * path during migration). */
uint32_t nxhal_boot_memory_count(void);

/* Decode one architecture-neutral record by index. The provider validates the
 * handoff and wire record again at the read boundary; no physical/wire pointer
 * is exposed to NX-CORE. */
enum nxhal_boot_memory_status nxhal_boot_memory_get(
    uint32_t index, struct nxhal_boot_memory_region *out);

/* Boot-live spans are immutable handoff buffers that remain referenced after
 * the first kernel page-table switch. They are mappings, not allocator input:
 * NX-CORE must preserve them without reclassifying firmware-reserved pages as
 * usable RAM. The current ABI has two semantic roles, not an arbitrary list. */
uint32_t nxhal_boot_live_span_count(void);
enum nxhal_boot_memory_status nxhal_boot_live_span_get(
    uint32_t index, struct nxhal_boot_live_span *out);

_Static_assert(sizeof(struct nxhal_boot_memory_region) == 32U,
               "NX-HAL boot-memory region ABI size");
_Static_assert(sizeof(struct nxhal_boot_live_span) == 24U,
               "NX-HAL boot-live span ABI size");

#endif /* NXHAL_BOOT_MEMORY_H */
