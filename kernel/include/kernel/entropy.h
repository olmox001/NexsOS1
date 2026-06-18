#ifndef _KERNEL_ENTROPY_H
#define _KERNEL_ENTROPY_H

#include <kernel/types.h>

/*
 * entropy_u64 - 64 bits of best-effort entropy, unified across architectures.
 *
 * One implementation drives both arches (ASTRA: divergence is a bug): it prefers
 * the CPU hardware RNG via the arch_hw_random() HAL primitive and, when that is
 * unavailable, mixes successive monotonic cycle-counter samples
 * (arch_timer_get_count) carrying boot-time jitter through splitmix64.  The only
 * per-arch code is the ISA wrapper arch_impl_hw_random.
 */
uint64_t entropy_u64(void);

#endif /* _KERNEL_ENTROPY_H */
