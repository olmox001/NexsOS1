#include <hal/resource.h>
#include <nxolmo/base/checked.h>
#include <nxolmo/base/status.h>

#include <stddef.h>
#include <stdint.h>

#define NXHAL_RESOURCE_ALL_RIGHTS \
  (NXHAL_RIGHT_DISCOVER | NXHAL_RIGHT_CONFIG_READ | \
   NXHAL_RIGHT_CONFIG_WRITE | NXHAL_RIGHT_MAP | NXHAL_RIGHT_READ | \
   NXHAL_RIGHT_WRITE | NXHAL_RIGHT_IRQ_BIND | NXHAL_RIGHT_IRQ_ACK | \
   NXHAL_RIGHT_DMA_MAP | NXHAL_RIGHT_DMA_UNMAP | \
   NXHAL_RIGHT_DMA_DEVICE_READ | NXHAL_RIGHT_DMA_DEVICE_WRITE | \
   NXHAL_RIGHT_QUEUE_SUBMIT | NXHAL_RIGHT_QUEUE_CANCEL | \
   NXHAL_RIGHT_QUEUE_DRAIN | NXHAL_RIGHT_ENDPOINT_CALL | \
   NXHAL_RIGHT_FIRMWARE_READ | NXHAL_RIGHT_FIRMWARE_UPDATE | \
   NXHAL_RIGHT_RESET | NXHAL_RIGHT_POWER)

#define NXHAL_RESOURCE_ALL_FLAGS \
  (NXHAL_RESOURCE_EXCLUSIVE | NXHAL_RESOURCE_SHAREABLE | \
   NXHAL_RESOURCE_CACHE_COHERENT | NXHAL_RESOURCE_WRITE_COMBINING)

#define NXHAL_RESOURCE_WORK UINT64_C(3)
#define NXHAL_GRANT_WORK UINT64_C(6)
#define NXHAL_RANGE_WORK UINT64_C(8)
#define NXHAL_DMA_WORK UINT64_C(11)

static int nxhal_ref_valid(struct nxhal_object_ref ref) {
  return (ref.id_high != 0U || ref.id_low != 0U) && ref.generation != 0U;
}

static int nxhal_ref_zero(struct nxhal_object_ref ref) {
  return ref.id_high == 0U && ref.id_low == 0U && ref.generation == 0U;
}

static int nxhal_ref_equal(struct nxhal_object_ref left,
                           struct nxhal_object_ref right) {
  return left.id_high == right.id_high && left.id_low == right.id_low &&
         left.generation == right.generation;
}

static int nxhal_power_of_two(uint64_t value) {
  return value != 0U && (value & (value - UINT64_C(1))) == 0U;
}

static int nxhal_resource_has_range(uint32_t type) {
  return type == NXHAL_RESOURCE_MMIO_REGION ||
         type == NXHAL_RESOURCE_INTERRUPT ||
         type == NXHAL_RESOURCE_DMA_DOMAIN ||
         type == NXHAL_RESOURCE_IOMMU_MAPPING ||
         type == NXHAL_RESOURCE_BLOCK_QUEUE;
}

static int nxhal_resource_parent_required(uint32_t type) {
  return type != NXHAL_RESOURCE_PCI_FUNCTION;
}

static uint64_t nxhal_resource_type_rights(uint32_t type) {
  switch (type) {
  case NXHAL_RESOURCE_PCI_FUNCTION:
    return NXHAL_RIGHT_DISCOVER | NXHAL_RIGHT_CONFIG_READ |
           NXHAL_RIGHT_CONFIG_WRITE | NXHAL_RIGHT_RESET | NXHAL_RIGHT_POWER;
  case NXHAL_RESOURCE_MMIO_REGION:
    return NXHAL_RIGHT_MAP | NXHAL_RIGHT_READ | NXHAL_RIGHT_WRITE;
  case NXHAL_RESOURCE_INTERRUPT:
    return NXHAL_RIGHT_IRQ_BIND | NXHAL_RIGHT_IRQ_ACK;
  case NXHAL_RESOURCE_DMA_DOMAIN:
  case NXHAL_RESOURCE_IOMMU_MAPPING:
    return NXHAL_RIGHT_DMA_MAP | NXHAL_RIGHT_DMA_UNMAP |
           NXHAL_RIGHT_DMA_DEVICE_READ | NXHAL_RIGHT_DMA_DEVICE_WRITE;
  case NXHAL_RESOURCE_BLOCK_QUEUE:
    return NXHAL_RIGHT_QUEUE_SUBMIT | NXHAL_RIGHT_QUEUE_CANCEL |
           NXHAL_RIGHT_QUEUE_DRAIN;
  case NXHAL_RESOURCE_DEVICE_ENDPOINT:
    return NXHAL_RIGHT_ENDPOINT_CALL;
  case NXHAL_RESOURCE_FIRMWARE_INTERFACE:
    return NXHAL_RIGHT_FIRMWARE_READ | NXHAL_RIGHT_FIRMWARE_UPDATE;
  case NXHAL_RESOURCE_RESET_CONTROL:
    return NXHAL_RIGHT_RESET;
  case NXHAL_RESOURCE_POWER_CONTROL:
    return NXHAL_RIGHT_POWER;
  default:
    return 0U;
  }
}

static int nxhal_aligned(const void *ptr, size_t alignment) {
  return ptr != NULL && ((uintptr_t)ptr % (uintptr_t)alignment) == 0U;
}

static void nxhal_report_begin(struct nxhal_operation_report *report,
                               uint64_t operation_id, uint64_t work_bound,
                               uint64_t bytes, uint64_t items) {
  report->operation_id = operation_id;
  report->work_bound = work_bound;
  report->work_consumed = 0U;
  report->work_remaining = work_bound;
  report->bytes = bytes;
  report->items = items;
  report->retries = 0U;
  report->elapsed_ns = NXHAL_TELEMETRY_UNKNOWN;
  report->hardware_cycles = NXHAL_TELEMETRY_UNKNOWN;
  report->status = NXHAL_RESOURCE_E_BUDGET;
  report->complete = 0U;
}

static enum nxhal_resource_status nxhal_work_step(
    struct nxhal_operation_report *report) {
  if (report->work_consumed >= report->work_bound)
    return NXHAL_RESOURCE_E_BUDGET;
  ++report->work_consumed;
  report->work_remaining = report->work_bound - report->work_consumed;
  return NXHAL_RESOURCE_OK;
}

static enum nxhal_resource_status nxhal_report_finish(
    struct nxhal_operation_report *report, enum nxhal_resource_status status) {
  report->status = (int32_t)status;
  report->complete = 1U;
  return status;
}

static enum nxhal_resource_status nxhal_resource_identity(
    const struct nxhal_resource *resource) {
  uint64_t allowed_rights;
  int exclusive;
  int shareable;

  if (resource->abi_version != NXHAL_RESOURCE_ABI_VERSION ||
      resource->policy_version == 0U ||
      resource->resource_type < NXHAL_RESOURCE_PCI_FUNCTION ||
      resource->resource_type > NXHAL_RESOURCE_POWER_CONTROL ||
      (resource->flags & ~NXHAL_RESOURCE_ALL_FLAGS) != 0U ||
      !nxhal_ref_valid(resource->object) || resource->lease_epoch == 0U ||
      resource->rights_ceiling == 0U ||
      (resource->rights_ceiling & ~NXHAL_RESOURCE_ALL_RIGHTS) != 0U)
    return NXHAL_RESOURCE_E_MALFORMED;

  exclusive = (resource->flags & NXHAL_RESOURCE_EXCLUSIVE) != 0U;
  shareable = (resource->flags & NXHAL_RESOURCE_SHAREABLE) != 0U;
  if (exclusive == shareable)
    return NXHAL_RESOURCE_E_MALFORMED;
  allowed_rights = nxhal_resource_type_rights(resource->resource_type);
  if ((resource->rights_ceiling & ~allowed_rights) != 0U)
    return NXHAL_RESOURCE_E_ACCESS;
  return NXHAL_RESOURCE_OK;
}

static enum nxhal_resource_status nxhal_resource_parent(
    const struct nxhal_resource *resource) {
  if (nxhal_resource_parent_required(resource->resource_type)) {
    if (!nxhal_ref_valid(resource->parent) ||
        nxhal_ref_equal(resource->parent, resource->object))
      return NXHAL_RESOURCE_E_MALFORMED;
  } else if (!nxhal_ref_zero(resource->parent)) {
    return NXHAL_RESOURCE_E_MALFORMED;
  }
  return NXHAL_RESOURCE_OK;
}

static enum nxhal_resource_status nxhal_resource_range(
    const struct nxhal_resource *resource) {
  uint64_t ignored_end;

  if (!nxhal_resource_has_range(resource->resource_type)) {
    if (resource->resource_offset != 0U || resource->resource_length != 0U ||
        resource->alignment != 0U)
      return NXHAL_RESOURCE_E_MALFORMED;
    return NXHAL_RESOURCE_OK;
  }
  if (resource->resource_length == 0U ||
      !nxhal_power_of_two(resource->alignment) ||
      (resource->resource_offset % resource->alignment) != 0U)
    return NXHAL_RESOURCE_E_MALFORMED;
  if (nx_u64_add(resource->resource_offset, resource->resource_length,
                 &ignored_end) != NX_STATUS_OK)
    return NXHAL_RESOURCE_E_OVERFLOW;
  return NXHAL_RESOURCE_OK;
}

static enum nxhal_resource_status nxhal_resource_attributes(
    const struct nxhal_resource *resource) {
  uint32_t type = resource->resource_type;
  int dma_type = type == NXHAL_RESOURCE_DMA_DOMAIN ||
                 type == NXHAL_RESOURCE_IOMMU_MAPPING;

  if ((resource->flags & NXHAL_RESOURCE_WRITE_COMBINING) != 0U &&
      type != NXHAL_RESOURCE_MMIO_REGION)
    return NXHAL_RESOURCE_E_MALFORMED;
  if ((resource->flags & NXHAL_RESOURCE_CACHE_COHERENT) != 0U && !dma_type)
    return NXHAL_RESOURCE_E_MALFORMED;
  if (dma_type &&
      (resource->domain_id == 0U || resource->max_transfer_bytes == 0U ||
       resource->max_transfer_bytes > resource->resource_length))
    return NXHAL_RESOURCE_E_MALFORMED;
  if (!dma_type &&
      (resource->domain_id != 0U || resource->max_transfer_bytes != 0U))
    return NXHAL_RESOURCE_E_MALFORMED;
  return nxhal_resource_parent(resource);
}

static enum nxhal_resource_status nxhal_resource_validate_internal(
    const struct nxhal_resource *resource,
    struct nxhal_operation_report *report) {
  enum nxhal_resource_status status;

  status = nxhal_work_step(report);
  if (status != NXHAL_RESOURCE_OK)
    return status;
  status = nxhal_resource_identity(resource);
  if (status != NXHAL_RESOURCE_OK)
    return status;
  status = nxhal_work_step(report);
  if (status != NXHAL_RESOURCE_OK)
    return status;
  status = nxhal_resource_range(resource);
  if (status != NXHAL_RESOURCE_OK)
    return status;
  status = nxhal_work_step(report);
  if (status != NXHAL_RESOURCE_OK)
    return status;
  return nxhal_resource_attributes(resource);
}

static enum nxhal_resource_status nxhal_grant_shape(
    const struct nxhal_resource_grant *grant) {
  if (grant->abi_version != NXHAL_GRANT_ABI_VERSION ||
      grant->policy_version == 0U || !nxhal_ref_valid(grant->resource) ||
      !nxhal_ref_valid(grant->principal) || grant->rights == 0U ||
      grant->scope_id == 0U || grant->lease_epoch == 0U ||
      grant->expiry_tick == 0U ||
      (grant->rights & ~NXHAL_RESOURCE_ALL_RIGHTS) != 0U)
    return NXHAL_RESOURCE_E_MALFORMED;
  return NXHAL_RESOURCE_OK;
}

static enum nxhal_resource_status nxhal_grant_relation(
    const struct nxhal_resource *resource,
    const struct nxhal_resource_grant *grant,
    struct nxhal_object_ref expected_principal, uint64_t expected_scope_id,
    uint64_t required_rights, uint64_t current_tick) {
  if (!nxhal_ref_equal(grant->resource, resource->object) ||
      !nxhal_ref_equal(grant->principal, expected_principal) ||
      grant->scope_id != expected_scope_id ||
      grant->policy_version != resource->policy_version ||
      grant->lease_epoch != resource->lease_epoch ||
      (grant->rights & ~resource->rights_ceiling) != 0U ||
      (required_rights & ~grant->rights) != 0U ||
      grant->expiry_tick <= current_tick)
    return NXHAL_RESOURCE_E_ACCESS;
  return NXHAL_RESOURCE_OK;
}

static enum nxhal_resource_status nxhal_grant_transfer(
    const struct nxhal_resource *resource,
    const struct nxhal_resource_grant *grant) {
  int dma_type = resource->resource_type == NXHAL_RESOURCE_DMA_DOMAIN ||
                 resource->resource_type == NXHAL_RESOURCE_IOMMU_MAPPING;

  if (grant->max_transfer_bytes > resource->max_transfer_bytes)
    return NXHAL_RESOURCE_E_ACCESS;
  if (dma_type && grant->max_transfer_bytes == 0U)
    return NXHAL_RESOURCE_E_ACCESS;
  if (!dma_type && grant->max_transfer_bytes != 0U)
    return NXHAL_RESOURCE_E_ACCESS;
  return NXHAL_RESOURCE_OK;
}

static enum nxhal_resource_status nxhal_grant_validate_internal(
    const struct nxhal_resource *resource,
    const struct nxhal_resource_grant *grant,
    struct nxhal_object_ref expected_principal, uint64_t expected_scope_id,
    uint64_t required_rights, uint64_t current_tick,
    struct nxhal_operation_report *report) {
  enum nxhal_resource_status status = nxhal_resource_validate_internal(
      resource, report);

  if (status != NXHAL_RESOURCE_OK)
    return status;
  if (nxhal_work_step(report) != NXHAL_RESOURCE_OK)
    return NXHAL_RESOURCE_E_BUDGET;
  status = nxhal_grant_shape(grant);
  if (status != NXHAL_RESOURCE_OK)
    return status;
  if (nxhal_work_step(report) != NXHAL_RESOURCE_OK)
    return NXHAL_RESOURCE_E_BUDGET;
  status = nxhal_grant_relation(resource, grant, expected_principal,
                                expected_scope_id, required_rights,
                                current_tick);
  if (status != NXHAL_RESOURCE_OK)
    return status;
  if (nxhal_work_step(report) != NXHAL_RESOURCE_OK)
    return NXHAL_RESOURCE_E_BUDGET;
  return nxhal_grant_transfer(resource, grant);
}

static int nxhal_common_args_valid(
    const struct nxhal_resource *resource,
    const struct nxhal_resource_grant *grant,
    const struct nxhal_operation_report *report) {
  if (!nxhal_aligned(resource, _Alignof(struct nxhal_resource)) ||
      !nxhal_aligned(report, _Alignof(struct nxhal_operation_report)))
    return 0;
  if (grant != NULL &&
      !nxhal_aligned(grant, _Alignof(struct nxhal_resource_grant)))
    return 0;
  if (nx_pointer_ranges_overlap(report, sizeof(*report), resource,
                          sizeof(*resource)))
    return 0;
  if (grant != NULL &&
      nx_pointer_ranges_overlap(report, sizeof(*report), grant, sizeof(*grant)))
    return 0;
  return 1;
}

enum nxhal_resource_status nxhal_resource_validate(
    const struct nxhal_resource *resource,
    struct nxhal_operation_report *report) {
  enum nxhal_resource_status status;

  if (!nxhal_common_args_valid(resource, NULL, report))
    return NXHAL_RESOURCE_E_INVALID;
  nxhal_report_begin(report, NXHAL_OP_HARDWARE_RESOURCE_VALIDATE,
                     NXHAL_RESOURCE_WORK, sizeof(*resource), 1U);
  status = nxhal_resource_validate_internal(resource, report);
  return nxhal_report_finish(report, status);
}

enum nxhal_resource_status nxhal_resource_grant_validate(
    const struct nxhal_resource *resource,
    const struct nxhal_resource_grant *grant,
    struct nxhal_object_ref expected_principal, uint64_t expected_scope_id,
    uint64_t required_rights, uint64_t current_tick,
    struct nxhal_operation_report *report) {
  enum nxhal_resource_status status;

  if (grant == NULL ||
      !nxhal_common_args_valid(resource, grant, report) ||
      !nxhal_ref_valid(expected_principal) || expected_scope_id == 0U ||
      required_rights == 0U ||
      (required_rights & ~NXHAL_RESOURCE_ALL_RIGHTS) != 0U)
    return NXHAL_RESOURCE_E_INVALID;
  if (nx_pointer_ranges_overlap(resource, sizeof(*resource), grant, sizeof(*grant)))
    return NXHAL_RESOURCE_E_INVALID;
  nxhal_report_begin(report, NXHAL_OP_HARDWARE_GRANT_VALIDATE,
                     NXHAL_GRANT_WORK, sizeof(*resource) + sizeof(*grant), 2U);
  status = nxhal_grant_validate_internal(
      resource, grant, expected_principal, expected_scope_id, required_rights,
      current_tick, report);
  return nxhal_report_finish(report, status);
}

static enum nxhal_resource_status nxhal_range_internal(
    const struct nxhal_resource *resource,
    const struct nxhal_resource_grant *grant,
    struct nxhal_object_ref principal, uint64_t scope, uint64_t offset,
    uint64_t length, uint64_t rights, uint64_t tick, uint64_t *end,
    struct nxhal_operation_report *report) {
  enum nxhal_resource_status status = nxhal_grant_validate_internal(
      resource, grant, principal, scope, rights, tick, report);

  if (status != NXHAL_RESOURCE_OK)
    return status;
  if (nxhal_work_step(report) != NXHAL_RESOURCE_OK)
    return NXHAL_RESOURCE_E_BUDGET;
  if (!nxhal_resource_has_range(resource->resource_type))
    return NXHAL_RESOURCE_E_NOT_SUPPORTED;
  if (nxhal_work_step(report) != NXHAL_RESOURCE_OK)
    return NXHAL_RESOURCE_E_BUDGET;
  if (nx_range_validate(offset, length, resource->resource_length,
                        end) != NX_STATUS_OK)
    return NXHAL_RESOURCE_E_LIMIT;
  return NXHAL_RESOURCE_OK;
}

static int nxhal_range_outputs_valid(
    const struct nxhal_resource *resource,
    const struct nxhal_resource_grant *grant, uint64_t *end_offset,
    const struct nxhal_operation_report *report) {
  if (!nxhal_aligned(end_offset, _Alignof(uint64_t)))
    return 0;
  if (nx_pointer_ranges_overlap(end_offset, sizeof(*end_offset), resource,
                          sizeof(*resource)) ||
      nx_pointer_ranges_overlap(end_offset, sizeof(*end_offset), grant,
                          sizeof(*grant)) ||
      nx_pointer_ranges_overlap(end_offset, sizeof(*end_offset), report,
                          sizeof(*report)))
    return 0;
  return 1;
}

enum nxhal_resource_status nxhal_resource_range_validate(
    const struct nxhal_resource *resource,
    const struct nxhal_resource_grant *grant,
    struct nxhal_object_ref expected_principal, uint64_t expected_scope_id,
    uint64_t offset, uint64_t length, uint64_t required_rights,
    uint64_t current_tick, uint64_t *end_offset,
    struct nxhal_operation_report *report) {
  enum nxhal_resource_status status;
  uint64_t end;

  if (grant == NULL ||
      !nxhal_common_args_valid(resource, grant, report) ||
      !nxhal_range_outputs_valid(resource, grant, end_offset, report) ||
      !nxhal_ref_valid(expected_principal) || expected_scope_id == 0U ||
      required_rights == 0U ||
      (required_rights & ~NXHAL_RESOURCE_ALL_RIGHTS) != 0U)
    return NXHAL_RESOURCE_E_INVALID;
  nxhal_report_begin(report, NXHAL_OP_HARDWARE_RANGE_VALIDATE,
                     NXHAL_RANGE_WORK, length, 1U);
  status = nxhal_range_internal(resource, grant, expected_principal,
                                expected_scope_id, offset, length,
                                required_rights, current_tick, &end, report);
  if (status == NXHAL_RESOURCE_OK)
    *end_offset = end;
  return nxhal_report_finish(report, status);
}

static enum nxhal_resource_status nxhal_dma_args(
    const struct nxhal_resource *resource, uint64_t offset, uint64_t length,
    uint64_t buffer_alignment, uint32_t direction, uint64_t *required_rights) {
  if ((resource->resource_type != NXHAL_RESOURCE_DMA_DOMAIN &&
       resource->resource_type != NXHAL_RESOURCE_IOMMU_MAPPING) ||
      length == 0U || !nxhal_power_of_two(buffer_alignment) ||
      (offset % buffer_alignment) != 0U ||
      (direction != NXHAL_DMA_DEVICE_READS &&
       direction != NXHAL_DMA_DEVICE_WRITES &&
       direction != NXHAL_DMA_BIDIRECTIONAL))
    return NXHAL_RESOURCE_E_INVALID;
  *required_rights = NXHAL_RIGHT_DMA_MAP;
  if (direction == NXHAL_DMA_DEVICE_READS ||
      direction == NXHAL_DMA_BIDIRECTIONAL)
    *required_rights |= NXHAL_RIGHT_DMA_DEVICE_READ;
  if (direction == NXHAL_DMA_DEVICE_WRITES ||
      direction == NXHAL_DMA_BIDIRECTIONAL)
    *required_rights |= NXHAL_RIGHT_DMA_DEVICE_WRITE;
  return NXHAL_RESOURCE_OK;
}

enum nxhal_resource_status nxhal_resource_dma_validate(
    const struct nxhal_resource *resource,
    const struct nxhal_resource_grant *grant,
    struct nxhal_object_ref expected_principal, uint64_t expected_scope_id,
    uint64_t offset, uint64_t length, uint64_t buffer_alignment,
    uint32_t direction, uint64_t current_tick,
    struct nxhal_operation_report *report) {
  enum nxhal_resource_status status;
  uint64_t required_rights;
  uint64_t end;

  if (grant == NULL ||
      !nxhal_common_args_valid(resource, grant, report) ||
      !nxhal_ref_valid(expected_principal) || expected_scope_id == 0U)
    return NXHAL_RESOURCE_E_INVALID;
  nxhal_report_begin(report, NXHAL_OP_HARDWARE_DMA_VALIDATE, NXHAL_DMA_WORK,
                     length, 1U);
  if (nxhal_work_step(report) != NXHAL_RESOURCE_OK)
    return nxhal_report_finish(report, NXHAL_RESOURCE_E_BUDGET);
  status = nxhal_dma_args(resource, offset, length, buffer_alignment,
                          direction, &required_rights);
  if (status != NXHAL_RESOURCE_OK)
    return nxhal_report_finish(report, status);
  status = nxhal_range_internal(resource, grant, expected_principal,
                                expected_scope_id, offset, length,
                                required_rights, current_tick, &end, report);
  if (status != NXHAL_RESOURCE_OK)
    return nxhal_report_finish(report, status);
  if (nxhal_work_step(report) != NXHAL_RESOURCE_OK)
    return nxhal_report_finish(report, NXHAL_RESOURCE_E_BUDGET);
  if (length > grant->max_transfer_bytes)
    return nxhal_report_finish(report, NXHAL_RESOURCE_E_LIMIT);
  if (nxhal_work_step(report) != NXHAL_RESOURCE_OK)
    return nxhal_report_finish(report, NXHAL_RESOURCE_E_BUDGET);
  if (buffer_alignment < resource->alignment)
    return nxhal_report_finish(report, NXHAL_RESOURCE_E_INVALID);
  (void)end;
  return nxhal_report_finish(report, NXHAL_RESOURCE_OK);
}
