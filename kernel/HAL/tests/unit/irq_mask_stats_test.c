#include <hal/irq_mask_stats.h>
#include <hal/time.h>
#include "core/irq_mask_stats_internal.h"

#include <stddef.h>
#include <stdint.h>

static int32_t mock_cpu;
static int mock_read_status;
static uint64_t mock_count;

int32_t nxhal_provider_cpu_id_fault_safe(void) { return mock_cpu; }

int nxhal_provider_clocksource_read(uint64_t *out_count) {
  if (mock_read_status != NXHAL_TIME_OK)
    return mock_read_status;
  *out_count = mock_count;
  return NXHAL_TIME_OK;
}

static int test_boundaries(void) {
  struct nxhal_irq_mask_stats_v1 snapshot;

  if (nxhal_irq_mask_stats_snapshot(0U, NULL) != NXHAL_IRQ_MASK_STATS_E_NULL)
    return 1;
  if (nxhal_irq_mask_stats_snapshot(UINT32_C(64), &snapshot) !=
      NXHAL_IRQ_MASK_STATS_E_CPU)
    return 2;
  return 0;
}

static int test_nested_interval(void) {
  struct nxhal_irq_mask_stats_v1 snapshot;

  mock_cpu = 1;
  mock_read_status = NXHAL_TIME_OK;
  mock_count = UINT64_C(100);
  nxhal_irq_mask_note_disabled(1);
  if (nxhal_irq_mask_stats_snapshot(1U, &snapshot) !=
          NXHAL_IRQ_MASK_STATS_OK ||
      (snapshot.flags & NXHAL_IRQ_MASK_STATS_ACTIVE) == 0U ||
      snapshot.active_start_count != UINT64_C(100))
    return 1;

  mock_count = UINT64_C(120);
  nxhal_irq_mask_note_disabled(0);
  nxhal_irq_mask_note_will_enable(0);
  if (nxhal_irq_mask_stats_snapshot(1U, &snapshot) !=
          NXHAL_IRQ_MASK_STATS_OK ||
      snapshot.interval_count != 0U ||
      (snapshot.flags & NXHAL_IRQ_MASK_STATS_ACTIVE) == 0U)
    return 2;

  mock_count = UINT64_C(160);
  nxhal_irq_mask_note_will_enable(1);
  if (nxhal_irq_mask_stats_snapshot(1U, &snapshot) !=
          NXHAL_IRQ_MASK_STATS_OK ||
      snapshot.interval_count != 1U || snapshot.total_counts != UINT64_C(60) ||
      snapshot.max_counts != UINT64_C(60) || snapshot.flags != 0U)
    return 3;
  return 0;
}

static int test_accumulation_and_invalid(void) {
  struct nxhal_irq_mask_stats_v1 snapshot;

  mock_cpu = 1;
  mock_count = UINT64_C(200);
  nxhal_irq_mask_note_disabled(1);
  mock_count = UINT64_C(240);
  nxhal_irq_mask_note_will_enable(1);
  if (nxhal_irq_mask_stats_snapshot(1U, &snapshot) !=
          NXHAL_IRQ_MASK_STATS_OK ||
      snapshot.interval_count != 2U || snapshot.total_counts != UINT64_C(100) ||
      snapshot.max_counts != UINT64_C(60))
    return 1;

  mock_count = UINT64_C(300);
  nxhal_irq_mask_note_disabled(1);
  mock_count = UINT64_C(290);
  nxhal_irq_mask_note_will_enable(1);
  if (nxhal_irq_mask_stats_snapshot(1U, &snapshot) !=
          NXHAL_IRQ_MASK_STATS_OK ||
      snapshot.invalid_sample_count != 1U || snapshot.interval_count != 2U ||
      snapshot.flags != 0U)
    return 2;
  return 0;
}

static int test_unavailable_counter(void) {
  struct nxhal_irq_mask_stats_v1 snapshot;

  mock_cpu = 2;
  mock_read_status = NXHAL_TIME_E_NOT_READY;
  mock_count = UINT64_C(400);
  nxhal_irq_mask_note_disabled(1);
  mock_read_status = NXHAL_TIME_OK;
  if (nxhal_irq_mask_stats_snapshot(2U, &snapshot) !=
          NXHAL_IRQ_MASK_STATS_OK ||
      snapshot.invalid_sample_count != 1U || snapshot.interval_count != 0U ||
      snapshot.flags != 0U)
    return 1;
  return 0;
}

int main(void) {
  if (test_boundaries() != 0)
    return 1;
  if (test_nested_interval() != 0)
    return 2;
  if (test_accumulation_and_invalid() != 0)
    return 3;
  if (test_unavailable_counter() != 0)
    return 4;
  return 0;
}
