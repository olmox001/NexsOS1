#ifndef OS1_TIME_SERVICE_H
#define OS1_TIME_SERVICE_H

#include <os1.h>

#define OS1_TIME_SERVICE_VERSION UINT32_C(3)
#define OS1_TIME_SERVICE_REQUEST_SNAPSHOT UINT64_C(1)
#define OS1_TIME_SERVICE_REQUEST_TIMER_STATS UINT64_C(2)
#define OS1_TIME_SERVICE_IPC_TYPE 0x210
/* Security/liveness policy: rotate a supervised service generation after a
 * finite number of completed receive epochs.  This bounds latent per-process
 * state even if a future request handler regresses and leaks a resource. */
#define OS1_TIME_SERVICE_EPOCH_REQUEST_LIMIT UINT32_C(4096)

/* Fetch one complete, sanitized scheduler/time snapshot through OS1nx_time.
 * The privileged daemon owns the underlying OS1_NS_TIME capability; clients do
 * not acquire hardware-facing timer objects directly. */
int OS1_time_service_snapshot(struct os1_time_snapshot_v1 *snapshot);
int OS1_time_service_timer_stats(
    struct os1_timer_stats_snapshot_v3 *snapshot);

#endif /* OS1_TIME_SERVICE_H */
