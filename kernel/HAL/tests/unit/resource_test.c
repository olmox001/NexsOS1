#include <hal/resource.h>

#include <stdint.h>
#include <string.h>

static struct nxhal_object_ref ref(uint64_t high, uint64_t low,
                                   uint64_t generation) {
  struct nxhal_object_ref value = {high, low, generation};
  return value;
}

static struct nxhal_resource valid_mmio(void) {
  struct nxhal_resource resource;
  memset(&resource, 0, sizeof(resource));
  resource.abi_version = NXHAL_RESOURCE_ABI_VERSION;
  resource.resource_type = NXHAL_RESOURCE_MMIO_REGION;
  resource.flags = NXHAL_RESOURCE_EXCLUSIVE |
                   NXHAL_RESOURCE_WRITE_COMBINING;
  resource.policy_version = 3U;
  resource.object = ref(1U, 2U, 4U);
  resource.parent = ref(1U, 1U, 2U);
  resource.resource_offset = UINT64_C(0x1000);
  resource.resource_length = UINT64_C(0x4000);
  resource.alignment = UINT64_C(0x1000);
  resource.lease_epoch = 9U;
  resource.rights_ceiling =
      NXHAL_RIGHT_MAP | NXHAL_RIGHT_READ | NXHAL_RIGHT_WRITE;
  return resource;
}

static struct nxhal_resource valid_dma(void) {
  struct nxhal_resource resource = valid_mmio();
  resource.resource_type = NXHAL_RESOURCE_DMA_DOMAIN;
  resource.flags = NXHAL_RESOURCE_EXCLUSIVE |
                   NXHAL_RESOURCE_CACHE_COHERENT;
  resource.object = ref(1U, 3U, 5U);
  resource.resource_offset = 0U;
  resource.resource_length = UINT64_C(0x8000);
  resource.alignment = 64U;
  resource.domain_id = 7U;
  resource.rights_ceiling =
      NXHAL_RIGHT_DMA_MAP | NXHAL_RIGHT_DMA_UNMAP |
      NXHAL_RIGHT_DMA_DEVICE_READ | NXHAL_RIGHT_DMA_DEVICE_WRITE;
  resource.max_transfer_bytes = UINT64_C(0x1000);
  return resource;
}

static struct nxhal_resource_grant grant_for(
    const struct nxhal_resource *resource) {
  struct nxhal_resource_grant grant;
  memset(&grant, 0, sizeof(grant));
  grant.abi_version = NXHAL_GRANT_ABI_VERSION;
  grant.policy_version = resource->policy_version;
  grant.resource = resource->object;
  grant.principal = ref(9U, 8U, 7U);
  grant.rights = resource->rights_ceiling;
  grant.scope_id = 55U;
  grant.lease_epoch = resource->lease_epoch;
  grant.expiry_tick = 1000U;
  grant.max_transfer_bytes = resource->max_transfer_bytes;
  return grant;
}

static int report_ok(const struct nxhal_operation_report *report,
                     uint64_t operation_id) {
  if (report->operation_id != operation_id || report->complete != 1U)
    return 0;
  if (report->work_consumed > report->work_bound)
    return 0;
  if (report->work_remaining !=
      report->work_bound - report->work_consumed)
    return 0;
  return report->hardware_cycles == NXHAL_TELEMETRY_UNKNOWN &&
         report->elapsed_ns == NXHAL_TELEMETRY_UNKNOWN;
}

static int test_resource_valid_paths(void) {
  struct nxhal_operation_report report;
  struct nxhal_resource mmio = valid_mmio();
  struct nxhal_resource dma = valid_dma();

  if (nxhal_resource_validate(&mmio, &report) != NXHAL_RESOURCE_OK)
    return 1;
  if (!report_ok(&report, NXHAL_OP_HARDWARE_RESOURCE_VALIDATE) ||
      report.work_bound != 3U || report.work_consumed != 3U)
    return 2;
  if (nxhal_resource_validate(&dma, &report) != NXHAL_RESOURCE_OK)
    return 3;
  if (report.status != NXHAL_RESOURCE_OK || report.work_consumed != 3U)
    return 4;
  return 0;
}

static int test_resource_rejects_malformed(void) {
  struct nxhal_operation_report report;
  struct nxhal_resource resource = valid_mmio();

  resource.flags |= NXHAL_RESOURCE_SHAREABLE;
  if (nxhal_resource_validate(&resource, &report) !=
      NXHAL_RESOURCE_E_MALFORMED)
    return 1;
  resource = valid_mmio();
  resource.resource_offset = UINT64_MAX - UINT64_C(0xFFF);
  if (nxhal_resource_validate(&resource, &report) !=
      NXHAL_RESOURCE_E_OVERFLOW)
    return 2;
  resource = valid_dma();
  resource.max_transfer_bytes = resource.resource_length + 1U;
  if (nxhal_resource_validate(&resource, &report) !=
      NXHAL_RESOURCE_E_MALFORMED)
    return 3;
  resource = valid_mmio();
  resource.rights_ceiling |= NXHAL_RIGHT_IRQ_BIND;
  if (nxhal_resource_validate(&resource, &report) !=
      NXHAL_RESOURCE_E_ACCESS)
    return 4;
  return 0;
}

static int test_grant_valid_paths(void) {
  struct nxhal_operation_report report;
  struct nxhal_resource mmio = valid_mmio();
  struct nxhal_resource dma = valid_dma();
  struct nxhal_resource_grant grant = grant_for(&mmio);

  if (nxhal_resource_grant_validate(
          &mmio, &grant, grant.principal, grant.scope_id,
          NXHAL_RIGHT_READ, 10U, &report) != NXHAL_RESOURCE_OK)
    return 1;
  if (!report_ok(&report, NXHAL_OP_HARDWARE_GRANT_VALIDATE) ||
      report.work_bound != 6U || report.work_consumed != 6U)
    return 2;
  grant = grant_for(&dma);
  if (nxhal_resource_grant_validate(
          &dma, &grant, grant.principal, grant.scope_id,
          NXHAL_RIGHT_DMA_MAP | NXHAL_RIGHT_DMA_DEVICE_READ,
          10U, &report) != NXHAL_RESOURCE_OK)
    return 3;
  return report.work_consumed == 6U ? 0 : 4;
}

static int test_grant_rejects_authority_mismatch(void) {
  struct nxhal_operation_report report;
  struct nxhal_resource resource = valid_mmio();
  struct nxhal_resource_grant grant = grant_for(&resource);

  if (nxhal_resource_grant_validate(
          &resource, &grant, ref(9U, 8U, 8U), grant.scope_id,
          NXHAL_RIGHT_READ, 10U, &report) != NXHAL_RESOURCE_E_ACCESS)
    return 1;
  grant = grant_for(&resource);
  grant.expiry_tick = 10U;
  if (nxhal_resource_grant_validate(
          &resource, &grant, grant.principal, grant.scope_id,
          NXHAL_RIGHT_READ, 10U, &report) != NXHAL_RESOURCE_E_ACCESS)
    return 2;
  grant = grant_for(&resource);
  grant.rights = NXHAL_RIGHT_READ;
  if (nxhal_resource_grant_validate(
          &resource, &grant, grant.principal, grant.scope_id,
          NXHAL_RIGHT_WRITE, 1U, &report) != NXHAL_RESOURCE_E_ACCESS)
    return 3;
  return 0;
}

static int test_range_paths(void) {
  struct nxhal_operation_report report;
  struct nxhal_resource resource = valid_mmio();
  struct nxhal_resource_grant grant = grant_for(&resource);
  uint64_t end = UINT64_C(0xfeedface);

  if (nxhal_resource_range_validate(
          &resource, &grant, grant.principal, grant.scope_id,
          0U, UINT64_C(0x1000), NXHAL_RIGHT_READ, 10U,
          &end, &report) != NXHAL_RESOURCE_OK)
    return 1;
  if (end != UINT64_C(0x1000) ||
      !report_ok(&report, NXHAL_OP_HARDWARE_RANGE_VALIDATE))
    return 2;
  if (nxhal_resource_range_validate(
          &resource, &grant, grant.principal, grant.scope_id,
          UINT64_C(0x3000), UINT64_C(0x1000), NXHAL_RIGHT_READ,
          10U, &end, &report) != NXHAL_RESOURCE_OK)
    return 3;
  return end == UINT64_C(0x4000) ? 0 : 4;
}

static int test_range_rejects_and_preserves_output(void) {
  struct nxhal_operation_report report;
  struct nxhal_resource resource = valid_mmio();
  struct nxhal_resource_grant grant = grant_for(&resource);
  uint64_t end = UINT64_C(0xfeedface);

  if (nxhal_resource_range_validate(
          &resource, &grant, grant.principal, grant.scope_id,
          UINT64_C(0x3fff), 2U, NXHAL_RIGHT_READ, 10U,
          &end, &report) != NXHAL_RESOURCE_E_LIMIT)
    return 1;
  if (end != UINT64_C(0xfeedface))
    return 2;
  if (nxhal_resource_range_validate(
          &resource, NULL, grant.principal, grant.scope_id,
          0U, 1U, NXHAL_RIGHT_READ, 10U, &end,
          &report) != NXHAL_RESOURCE_E_INVALID)
    return 3;
  return end == UINT64_C(0xfeedface) ? 0 : 4;
}

static int test_dma_paths(void) {
  struct nxhal_operation_report report;
  struct nxhal_resource resource = valid_dma();
  struct nxhal_resource_grant grant = grant_for(&resource);

  if (nxhal_resource_dma_validate(
          &resource, &grant, grant.principal, grant.scope_id,
          64U, 512U, 64U, NXHAL_DMA_DEVICE_READS, 10U,
          &report) != NXHAL_RESOURCE_OK)
    return 1;
  if (!report_ok(&report, NXHAL_OP_HARDWARE_DMA_VALIDATE) ||
      report.work_bound != 11U || report.work_consumed != 11U)
    return 2;
  if (nxhal_resource_dma_validate(
          &resource, &grant, grant.principal, grant.scope_id,
          128U, 1024U, 128U, NXHAL_DMA_BIDIRECTIONAL, 10U,
          &report) != NXHAL_RESOURCE_OK)
    return 3;
  return 0;
}

static int test_dma_rejects_limits(void) {
  struct nxhal_operation_report report;
  struct nxhal_resource resource = valid_dma();
  struct nxhal_resource_grant grant = grant_for(&resource);

  if (nxhal_resource_dma_validate(
          &resource, &grant, grant.principal, grant.scope_id,
          64U, UINT64_C(0x1001), 64U, NXHAL_DMA_DEVICE_READS,
          10U, &report) != NXHAL_RESOURCE_E_LIMIT)
    return 1;
  if (nxhal_resource_dma_validate(
          &resource, &grant, grant.principal, grant.scope_id,
          64U, 512U, 32U, NXHAL_DMA_DEVICE_READS, 10U,
          &report) != NXHAL_RESOURCE_E_INVALID)
    return 2;
  if (nxhal_resource_dma_validate(
          &resource, &grant, grant.principal, grant.scope_id,
          64U, 512U, 64U, 0U, 10U,
          &report) != NXHAL_RESOURCE_E_INVALID)
    return 3;
  return 0;
}

static int test_invalid_output_aliases(void) {
  struct nxhal_resource resource = valid_mmio();
  struct nxhal_resource_grant grant = grant_for(&resource);
  struct nxhal_operation_report report;

  if (nxhal_resource_validate(
          &resource, (struct nxhal_operation_report *)&resource) !=
      NXHAL_RESOURCE_E_INVALID)
    return 1;
  if (nxhal_resource_grant_validate(
          &resource, NULL, grant.principal, grant.scope_id,
          NXHAL_RIGHT_READ, 1U, &report) != NXHAL_RESOURCE_E_INVALID)
    return 2;
  return 0;
}

int main(void) {
  if (test_resource_valid_paths() != 0)
    return 1;
  if (test_resource_rejects_malformed() != 0)
    return 2;
  if (test_grant_valid_paths() != 0)
    return 3;
  if (test_grant_rejects_authority_mismatch() != 0)
    return 4;
  if (test_range_paths() != 0)
    return 5;
  if (test_range_rejects_and_preserves_output() != 0)
    return 6;
  if (test_dma_paths() != 0)
    return 7;
  if (test_dma_rejects_limits() != 0)
    return 8;
  if (test_invalid_output_aliases() != 0)
    return 9;
  return 0;
}
