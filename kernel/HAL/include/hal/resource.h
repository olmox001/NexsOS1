#ifndef NXHAL_RESOURCE_H
#define NXHAL_RESOURCE_H

#include <stddef.h>
#include <stdint.h>

#define NXHAL_RESOURCE_ABI_VERSION UINT32_C(1)
#define NXHAL_GRANT_ABI_VERSION UINT32_C(1)

#define NXHAL_OP_HARDWARE_DMA_VALIDATE UINT64_C(0x00070001)
#define NXHAL_OP_HARDWARE_GRANT_VALIDATE UINT64_C(0x00070003)
#define NXHAL_OP_HARDWARE_RANGE_VALIDATE UINT64_C(0x00070004)
#define NXHAL_OP_HARDWARE_RESOURCE_VALIDATE UINT64_C(0x00070005)

#define NXHAL_TELEMETRY_UNKNOWN UINT64_MAX

enum nxhal_resource_type {
  NXHAL_RESOURCE_PCI_FUNCTION = 1,
  NXHAL_RESOURCE_MMIO_REGION = 2,
  NXHAL_RESOURCE_INTERRUPT = 3,
  NXHAL_RESOURCE_DMA_DOMAIN = 4,
  NXHAL_RESOURCE_IOMMU_MAPPING = 5,
  NXHAL_RESOURCE_BLOCK_QUEUE = 6,
  NXHAL_RESOURCE_DEVICE_ENDPOINT = 7,
  NXHAL_RESOURCE_FIRMWARE_INTERFACE = 8,
  NXHAL_RESOURCE_RESET_CONTROL = 9,
  NXHAL_RESOURCE_POWER_CONTROL = 10
};

#define NXHAL_RIGHT_DISCOVER UINT64_C(0x00000001)
#define NXHAL_RIGHT_CONFIG_READ UINT64_C(0x00000002)
#define NXHAL_RIGHT_CONFIG_WRITE UINT64_C(0x00000004)
#define NXHAL_RIGHT_MAP UINT64_C(0x00000008)
#define NXHAL_RIGHT_READ UINT64_C(0x00000010)
#define NXHAL_RIGHT_WRITE UINT64_C(0x00000020)
#define NXHAL_RIGHT_IRQ_BIND UINT64_C(0x00000040)
#define NXHAL_RIGHT_IRQ_ACK UINT64_C(0x00000080)
#define NXHAL_RIGHT_DMA_MAP UINT64_C(0x00000100)
#define NXHAL_RIGHT_DMA_UNMAP UINT64_C(0x00000200)
#define NXHAL_RIGHT_DMA_DEVICE_READ UINT64_C(0x00000400)
#define NXHAL_RIGHT_DMA_DEVICE_WRITE UINT64_C(0x00000800)
#define NXHAL_RIGHT_QUEUE_SUBMIT UINT64_C(0x00001000)
#define NXHAL_RIGHT_QUEUE_CANCEL UINT64_C(0x00002000)
#define NXHAL_RIGHT_QUEUE_DRAIN UINT64_C(0x00004000)
#define NXHAL_RIGHT_ENDPOINT_CALL UINT64_C(0x00008000)
#define NXHAL_RIGHT_FIRMWARE_READ UINT64_C(0x00010000)
#define NXHAL_RIGHT_FIRMWARE_UPDATE UINT64_C(0x00020000)
#define NXHAL_RIGHT_RESET UINT64_C(0x00040000)
#define NXHAL_RIGHT_POWER UINT64_C(0x00080000)

#define NXHAL_RESOURCE_EXCLUSIVE UINT32_C(0x00000001)
#define NXHAL_RESOURCE_SHAREABLE UINT32_C(0x00000002)
#define NXHAL_RESOURCE_CACHE_COHERENT UINT32_C(0x00000004)
#define NXHAL_RESOURCE_WRITE_COMBINING UINT32_C(0x00000008)

enum nxhal_dma_direction {
  NXHAL_DMA_DEVICE_READS = 1,
  NXHAL_DMA_DEVICE_WRITES = 2,
  NXHAL_DMA_BIDIRECTIONAL = 3
};

enum nxhal_resource_status {
  NXHAL_RESOURCE_OK = 0,
  NXHAL_RESOURCE_E_INVALID = -1,
  NXHAL_RESOURCE_E_MALFORMED = -2,
  NXHAL_RESOURCE_E_ACCESS = -3,
  NXHAL_RESOURCE_E_OVERFLOW = -4,
  NXHAL_RESOURCE_E_NOT_SUPPORTED = -5,
  NXHAL_RESOURCE_E_LIMIT = -6,
  NXHAL_RESOURCE_E_BUDGET = -7
};

struct nxhal_object_ref {
  uint64_t id_high;
  uint64_t id_low;
  uint64_t generation;
};

struct nxhal_resource {
  uint32_t abi_version;
  uint32_t resource_type;
  uint32_t flags;
  uint32_t policy_version;
  struct nxhal_object_ref object;
  struct nxhal_object_ref parent;
  uint64_t resource_offset;
  uint64_t resource_length;
  uint64_t alignment;
  uint64_t domain_id;
  uint64_t lease_epoch;
  uint64_t rights_ceiling;
  uint64_t max_transfer_bytes;
};

struct nxhal_resource_grant {
  uint32_t abi_version;
  uint32_t policy_version;
  struct nxhal_object_ref resource;
  struct nxhal_object_ref principal;
  uint64_t rights;
  uint64_t scope_id;
  uint64_t lease_epoch;
  uint64_t expiry_tick;
  uint64_t max_transfer_bytes;
};

struct nxhal_operation_report {
  uint64_t operation_id;
  uint64_t work_bound;
  uint64_t work_consumed;
  uint64_t work_remaining;
  uint64_t bytes;
  uint64_t items;
  uint64_t retries;
  uint64_t elapsed_ns;
  uint64_t hardware_cycles;
  int32_t status;
  uint32_t complete;
};

enum nxhal_resource_status nxhal_resource_validate(
    const struct nxhal_resource *resource,
    struct nxhal_operation_report *report);

enum nxhal_resource_status nxhal_resource_grant_validate(
    const struct nxhal_resource *resource,
    const struct nxhal_resource_grant *grant,
    struct nxhal_object_ref expected_principal,
    uint64_t expected_scope_id,
    uint64_t required_rights,
    uint64_t current_tick,
    struct nxhal_operation_report *report);

enum nxhal_resource_status nxhal_resource_range_validate(
    const struct nxhal_resource *resource,
    const struct nxhal_resource_grant *grant,
    struct nxhal_object_ref expected_principal,
    uint64_t expected_scope_id,
    uint64_t offset,
    uint64_t length,
    uint64_t required_rights,
    uint64_t current_tick,
    uint64_t *end_offset,
    struct nxhal_operation_report *report);

enum nxhal_resource_status nxhal_resource_dma_validate(
    const struct nxhal_resource *resource,
    const struct nxhal_resource_grant *grant,
    struct nxhal_object_ref expected_principal,
    uint64_t expected_scope_id,
    uint64_t offset,
    uint64_t length,
    uint64_t buffer_alignment,
    uint32_t direction,
    uint64_t current_tick,
    struct nxhal_operation_report *report);

_Static_assert(sizeof(struct nxhal_object_ref) == 24U,
               "NX-HAL object reference ABI size");
_Static_assert(sizeof(struct nxhal_resource) == 120U,
               "NX-HAL resource ABI size");
_Static_assert(sizeof(struct nxhal_resource_grant) == 96U,
               "NX-HAL resource grant ABI size");

#endif /* NXHAL_RESOURCE_H */
