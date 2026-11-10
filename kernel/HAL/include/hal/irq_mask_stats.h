#ifndef NXHAL_IRQ_MASK_STATS_H
#define NXHAL_IRQ_MASK_STATS_H

#include <stdint.h>

#define NXHAL_IRQ_MASK_STATS_ABI_VERSION UINT32_C(1)
#define NXHAL_IRQ_MASK_STATS_ACTIVE UINT32_C(1)

enum nxhal_irq_mask_stats_status {
  NXHAL_IRQ_MASK_STATS_OK = 0,
  NXHAL_IRQ_MASK_STATS_E_NULL = -1,
  NXHAL_IRQ_MASK_STATS_E_CPU = -2,
  NXHAL_IRQ_MASK_STATS_E_BUSY = -3
};

struct nxhal_irq_mask_stats_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t cpu_id;
  uint32_t flags;
  uint64_t sequence;
  uint64_t interval_count;
  uint64_t total_counts;
  uint64_t max_counts;
  uint64_t active_start_count;
  uint64_t invalid_sample_count;
};

int nxhal_irq_mask_stats_snapshot(uint32_t cpu_id,
                                  struct nxhal_irq_mask_stats_v1 *out);

#endif /* NXHAL_IRQ_MASK_STATS_H */
