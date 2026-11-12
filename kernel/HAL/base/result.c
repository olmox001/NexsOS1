#include "nxolmo/base/result.h"
#include "nxolmo/base/status.h"
#include "nxolmo/base/types.h"

#include <stddef.h>
#include <stdint.h>

nx_status nx_result_init(nx_result *result, nx_status status,
                         nx_result_domain domain, uint64_t operation_id,
                         uint64_t completed, uint64_t detail,
                         uint64_t cycles_used, uint64_t fuel_used) {
    if (result == NULL)
        return NX_STATUS_INVALID_ARGUMENT;

    result->schema_id = NX_RESULT_SCHEMA_ID;
    result->schema_version = NX_RESULT_SCHEMA_VERSION;
    result->flags = cycles_used == NX_CYCLES_UNKNOWN
                        ? NX_RESULT_FLAG_CYCLES_UNKNOWN
                        : 0u;
    result->status = status;
    result->domain = (uint32_t)domain;
    result->operation_id = operation_id;
    result->completed = completed;
    result->detail = detail;
    result->cycles_used = cycles_used;
    result->fuel_used = fuel_used;
    return NX_STATUS_OK;
}

nx_status nx_result_validate(const nx_result *result) {
    const uint32_t known_flags = NX_RESULT_FLAG_CYCLES_UNKNOWN |
                                 NX_RESULT_FLAG_PARTIAL |
                                 NX_RESULT_FLAG_RETRYABLE;
    if (result == NULL)
        return NX_STATUS_INVALID_ARGUMENT;
    if (result->schema_id != NX_RESULT_SCHEMA_ID ||
        result->schema_version != NX_RESULT_SCHEMA_VERSION)
        return NX_STATUS_VERSION_MISMATCH;
    if (((uint32_t)result->flags & ~known_flags) != 0u)
        return NX_STATUS_MALFORMED;
    if (((result->flags & NX_RESULT_FLAG_CYCLES_UNKNOWN) != 0u) !=
        (result->cycles_used == NX_CYCLES_UNKNOWN))
        return NX_STATUS_MALFORMED;
    if (result->domain > (uint32_t)NX_RESULT_DOMAIN_IMAGE)
        return NX_STATUS_MALFORMED;
    return NX_STATUS_OK;
}
