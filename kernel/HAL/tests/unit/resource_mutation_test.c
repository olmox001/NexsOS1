#include <hal/resource.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RESOURCE_MUTATION_ITERATIONS UINT64_C(100000)

static uint64_t next_random(uint64_t *state) {
  *state ^= *state >> 12U;
  *state ^= *state << 25U;
  *state ^= *state >> 27U;
  return *state * UINT64_C(2685821657736338717);
}

static struct nxhal_object_ref ref(uint64_t high, uint64_t low,
                                   uint64_t generation) {
  struct nxhal_object_ref value = {high, low, generation};
  return value;
}

static void init_resource(struct nxhal_resource *resource, int dma) {
  memset(resource, 0, sizeof(*resource));
  resource->abi_version = NXHAL_RESOURCE_ABI_VERSION;
  resource->resource_type =
      dma != 0 ? NXHAL_RESOURCE_DMA_DOMAIN : NXHAL_RESOURCE_MMIO_REGION;
  resource->flags = NXHAL_RESOURCE_EXCLUSIVE |
      (dma != 0 ? NXHAL_RESOURCE_CACHE_COHERENT :
                  NXHAL_RESOURCE_WRITE_COMBINING);
  resource->policy_version = 1U;
  resource->object = ref(1U, dma != 0 ? 3U : 2U, 1U);
  resource->parent = ref(1U, 1U, 1U);
  resource->resource_offset = dma != 0 ? 0U : UINT64_C(0x1000);
  resource->resource_length = UINT64_C(8192);
  resource->alignment = dma != 0 ? 64U : UINT64_C(4096);
  resource->domain_id = dma != 0 ? 2U : 0U;
  resource->lease_epoch = 7U;
  resource->rights_ceiling =
      dma != 0
          ? NXHAL_RIGHT_DMA_MAP | NXHAL_RIGHT_DMA_UNMAP |
                NXHAL_RIGHT_DMA_DEVICE_READ | NXHAL_RIGHT_DMA_DEVICE_WRITE
          : NXHAL_RIGHT_MAP | NXHAL_RIGHT_READ | NXHAL_RIGHT_WRITE;
  resource->max_transfer_bytes = dma != 0 ? UINT64_C(1024) : 0U;
}

static void init_grant(struct nxhal_resource_grant *grant,
                       const struct nxhal_resource *resource) {
  memset(grant, 0, sizeof(*grant));
  grant->abi_version = NXHAL_GRANT_ABI_VERSION;
  grant->policy_version = resource->policy_version;
  grant->resource = resource->object;
  grant->principal = ref(9U, 8U, 7U);
  grant->rights = resource->rights_ceiling;
  grant->scope_id = 55U;
  grant->lease_epoch = resource->lease_epoch;
  grant->expiry_tick = 100U;
  grant->max_transfer_bytes = resource->max_transfer_bytes;
}

static void mutate_bytes(void *object, size_t size, uint64_t *state) {
  uint8_t *bytes = (uint8_t *)object;
  uint64_t mutations = (next_random(state) % UINT64_C(4)) + 1U;
  uint64_t mutation;

  for (mutation = 0U; mutation < mutations; ++mutation) {
    size_t index = (size_t)(next_random(state) % (uint64_t)size);
    uint8_t delta = (uint8_t)next_random(state);
    if (delta == 0U)
      delta = UINT8_C(1);
    bytes[index] ^= delta;
  }
}

static int report_sane(const struct nxhal_operation_report *report) {
  if (report->complete != 1U || report->work_consumed > report->work_bound)
    return 0;
  return report->work_remaining ==
         report->work_bound - report->work_consumed;
}

int main(void) {
  struct nxhal_resource resource;
  struct nxhal_resource_grant grant;
  struct nxhal_operation_report report;
  uint64_t state = UINT64_C(0x6e7868616c687731);
  uint64_t accepted_resource = 0U;
  uint64_t accepted_grant = 0U;
  uint64_t accepted_range = 0U;
  uint64_t accepted_dma = 0U;
  uint64_t iteration;

  for (iteration = 0U; iteration < RESOURCE_MUTATION_ITERATIONS; ++iteration) {
    int dma = ((iteration / UINT64_C(4)) & UINT64_C(1)) != 0U;
    uint64_t rights = dma != 0
        ? NXHAL_RIGHT_DMA_MAP | NXHAL_RIGHT_DMA_DEVICE_READ
        : NXHAL_RIGHT_READ;
    uint64_t offset = dma != 0 ? 64U : 0U;
    uint64_t length = dma != 0 ? 512U : 128U;
    uint64_t end = UINT64_C(0xfeedface);
    uint64_t original_end = end;

    init_resource(&resource, dma);
    init_grant(&grant, &resource);
    if ((iteration % UINT64_C(4)) == 1U)
      mutate_bytes(&resource, sizeof(resource), &state);
    else if ((iteration % UINT64_C(4)) == 2U) {
      mutate_bytes(&resource, sizeof(resource), &state);
      mutate_bytes(&grant, sizeof(grant), &state);
    } else if ((iteration % UINT64_C(4)) == 3U)
      mutate_bytes(&grant, sizeof(grant), &state);

    if (nxhal_resource_validate(&resource, &report) == NXHAL_RESOURCE_OK)
      ++accepted_resource;
    if (!report_sane(&report))
      return 1;

    if (nxhal_resource_grant_validate(
            &resource, &grant, ref(9U, 8U, 7U), 55U, rights,
            10U, &report) == NXHAL_RESOURCE_OK)
      ++accepted_grant;
    if (report.complete != 1U && report.operation_id != 0U)
      return 2;

    if (nxhal_resource_range_validate(
            &resource, &grant, ref(9U, 8U, 7U), 55U, offset, length,
            rights, 10U, &end, &report) == NXHAL_RESOURCE_OK)
      ++accepted_range;
    else if (end != original_end)
      return 3;

    if (nxhal_resource_dma_validate(
            &resource, &grant, ref(9U, 8U, 7U), 55U, offset, length,
            dma != 0 ? 64U : 1U,
            dma != 0 ? NXHAL_DMA_DEVICE_READS : 0U,
            10U, &report) == NXHAL_RESOURCE_OK)
      ++accepted_dma;
  }

  if (accepted_resource == 0U || accepted_grant == 0U ||
      accepted_range == 0U || accepted_dma == 0U)
    return 4;
  (void)printf("NX-HAL resource mutation: %" PRIu64
               " PASS resource=%" PRIu64 " grant=%" PRIu64
               " range=%" PRIu64 " dma=%" PRIu64 "\n",
               RESOURCE_MUTATION_ITERATIONS, accepted_resource,
               accepted_grant, accepted_range, accepted_dma);
  return 0;
}
