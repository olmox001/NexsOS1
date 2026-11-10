#include <hal/irq_mask_stats.h>
#include <hal/limits.h>
#include <hal/provider.h>
#include "irq_mask_stats_internal.h"

#include <stddef.h>
#include <stdint.h>

enum {
  NXHAL_IRQ_MASK_SNAPSHOT_RETRIES = 4
};

struct nxhal_irq_mask_cpu_state {
  uint64_t sequence;
  uint64_t interval_count;
  uint64_t total_counts;
  uint64_t max_counts;
  uint64_t active_start_count;
  uint64_t invalid_sample_count;
  uint32_t active;
};

static struct nxhal_irq_mask_cpu_state irq_mask_cpu[NXHAL_CPU_LIMIT];

static struct nxhal_irq_mask_cpu_state *irq_mask_local_state(void) {
  int32_t cpu = nxhal_provider_cpu_id_fault_safe();

  if (cpu < 0 || (uint32_t)cpu >= NXHAL_CPU_LIMIT)
    return NULL;
  return &irq_mask_cpu[(uint32_t)cpu];
}

static int irq_mask_counter_read(uint64_t *count) {
  return nxhal_provider_clocksource_read(count) == NXHAL_TIME_OK ? 0 : -1;
}

static void irq_mask_write_begin(struct nxhal_irq_mask_cpu_state *state) {
  __atomic_add_fetch(&state->sequence, UINT64_C(1), __ATOMIC_ACQ_REL);
}

static void irq_mask_write_end(struct nxhal_irq_mask_cpu_state *state) {
  __atomic_add_fetch(&state->sequence, UINT64_C(1), __ATOMIC_RELEASE);
}

static void irq_mask_invalid(struct nxhal_irq_mask_cpu_state *state) {
  uint64_t invalid =
      __atomic_load_n(&state->invalid_sample_count, __ATOMIC_RELAXED);
  if (invalid != UINT64_MAX)
    __atomic_store_n(&state->invalid_sample_count, invalid + UINT64_C(1),
                     __ATOMIC_RELAXED);
}

void nxhal_irq_mask_note_disabled(int was_enabled) {
  struct nxhal_irq_mask_cpu_state *state;
  uint64_t count;

  if (was_enabled == 0)
    return;
  state = irq_mask_local_state();
  if (state == NULL)
    return;
  if (irq_mask_counter_read(&count) != 0) {
    irq_mask_invalid(state);
    return;
  }
  irq_mask_write_begin(state);
  if (__atomic_load_n(&state->active, __ATOMIC_RELAXED) != 0U) {
    irq_mask_invalid(state);
  } else {
    __atomic_store_n(&state->active_start_count, count, __ATOMIC_RELAXED);
    __atomic_store_n(&state->active, 1U, __ATOMIC_RELAXED);
  }
  irq_mask_write_end(state);
}

static void irq_mask_complete(struct nxhal_irq_mask_cpu_state *state,
                              uint64_t count) {
  uint64_t start =
      __atomic_load_n(&state->active_start_count, __ATOMIC_RELAXED);
  uint64_t interval;
  uint64_t total;
  uint64_t max;

  if (count < start) {
    irq_mask_invalid(state);
    return;
  }
  interval = count - start;
  total = __atomic_load_n(&state->total_counts, __ATOMIC_RELAXED);
  max = __atomic_load_n(&state->max_counts, __ATOMIC_RELAXED);
  if (UINT64_MAX - total < interval)
    total = UINT64_MAX;
  else
    total += interval;
  __atomic_store_n(&state->total_counts, total, __ATOMIC_RELAXED);
  if (interval > max)
    __atomic_store_n(&state->max_counts, interval, __ATOMIC_RELAXED);
  uint64_t completed =
      __atomic_load_n(&state->interval_count, __ATOMIC_RELAXED);
  if (completed != UINT64_MAX)
    __atomic_store_n(&state->interval_count, completed + UINT64_C(1),
                     __ATOMIC_RELAXED);
}

void nxhal_irq_mask_note_will_enable(int will_enable) {
  struct nxhal_irq_mask_cpu_state *state;
  uint64_t count;

  if (will_enable == 0)
    return;
  state = irq_mask_local_state();
  if (state == NULL)
    return;
  if (irq_mask_counter_read(&count) != 0) {
    irq_mask_invalid(state);
    return;
  }
  irq_mask_write_begin(state);
  if (__atomic_load_n(&state->active, __ATOMIC_RELAXED) == 0U) {
    irq_mask_invalid(state);
  } else {
    irq_mask_complete(state, count);
    __atomic_store_n(&state->active, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&state->active_start_count, 0U, __ATOMIC_RELAXED);
  }
  irq_mask_write_end(state);
}

static void irq_mask_snapshot_load(uint32_t cpu_id,
                                   struct nxhal_irq_mask_stats_v1 *out) {
  struct nxhal_irq_mask_cpu_state *state = &irq_mask_cpu[cpu_id];

  out->abi_version = NXHAL_IRQ_MASK_STATS_ABI_VERSION;
  out->struct_size = (uint32_t)sizeof(*out);
  out->cpu_id = cpu_id;
  out->flags = __atomic_load_n(&state->active, __ATOMIC_RELAXED) != 0U
                   ? NXHAL_IRQ_MASK_STATS_ACTIVE
                   : 0U;
  out->interval_count =
      __atomic_load_n(&state->interval_count, __ATOMIC_RELAXED);
  out->total_counts = __atomic_load_n(&state->total_counts, __ATOMIC_RELAXED);
  out->max_counts = __atomic_load_n(&state->max_counts, __ATOMIC_RELAXED);
  out->active_start_count =
      __atomic_load_n(&state->active_start_count, __ATOMIC_RELAXED);
  out->invalid_sample_count =
      __atomic_load_n(&state->invalid_sample_count, __ATOMIC_RELAXED);
}

int nxhal_irq_mask_stats_snapshot(uint32_t cpu_id,
                                  struct nxhal_irq_mask_stats_v1 *out) {
  struct nxhal_irq_mask_cpu_state *state;

  if (out == NULL)
    return NXHAL_IRQ_MASK_STATS_E_NULL;
  if (cpu_id >= NXHAL_CPU_LIMIT)
    return NXHAL_IRQ_MASK_STATS_E_CPU;
  state = &irq_mask_cpu[cpu_id];
  for (uint32_t attempt = 0U; attempt < NXHAL_IRQ_MASK_SNAPSHOT_RETRIES;
       ++attempt) {
    uint64_t before = __atomic_load_n(&state->sequence, __ATOMIC_ACQUIRE);
    if ((before & UINT64_C(1)) != 0U)
      continue;
    irq_mask_snapshot_load(cpu_id, out);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint64_t after = __atomic_load_n(&state->sequence, __ATOMIC_ACQUIRE);
    if (before == after) {
      out->sequence = after;
      return NXHAL_IRQ_MASK_STATS_OK;
    }
  }
  return NXHAL_IRQ_MASK_STATS_E_BUSY;
}
