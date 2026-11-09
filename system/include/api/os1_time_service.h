#ifndef OS1_TIME_SERVICE_H
#define OS1_TIME_SERVICE_H

#include <os1.h>

#define OS1_TIME_SERVICE_VERSION UINT32_C(1)
#define OS1_TIME_SERVICE_REQUEST_SNAPSHOT UINT64_C(1)
#define OS1_TIME_SERVICE_IPC_TYPE 0x210

struct os1_time_service_request_v1 {
  uint32_t version;
  uint32_t reserved;
};

/* Fetch one complete, sanitized scheduler/time snapshot through OS1nx_time.
 * The privileged daemon owns the underlying OS1_NS_TIME capability; clients do
 * not acquire hardware-facing timer objects directly. */
int OS1_time_service_snapshot(struct os1_time_snapshot_v1 *snapshot);

#endif /* OS1_TIME_SERVICE_H */
