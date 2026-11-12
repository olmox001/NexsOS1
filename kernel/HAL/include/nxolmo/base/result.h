#ifndef NXOLMO_BASE_RESULT_H
#define NXOLMO_BASE_RESULT_H

#include "nxolmo/base/status.h"
#include "nxolmo/base/types.h"

#include <stdint.h>

#define NX_RESULT_SCHEMA_ID UINT32_C(0x3152584e)
#define NX_RESULT_SCHEMA_VERSION UINT16_C(1)

/* Typed bit masks: enum constants promote to signed int in C bitwise
 * expressions despite their UINT16_C initializer. Public numeric ABI is
 * unchanged; the wire field remains uint16_t. */
#define NX_RESULT_FLAG_CYCLES_UNKNOWN UINT32_C(1)
#define NX_RESULT_FLAG_PARTIAL UINT32_C(2)
#define NX_RESULT_FLAG_RETRYABLE UINT32_C(4)

typedef enum nx_result_domain {
    NX_RESULT_DOMAIN_NONE = 0,
    NX_RESULT_DOMAIN_BASE = 1,
    NX_RESULT_DOMAIN_RUNTIME = 2,
    NX_RESULT_DOMAIN_REGISTRY = 3,
    NX_RESULT_DOMAIN_STORAGE = 4,
    NX_RESULT_DOMAIN_PROTOCOL = 5,
    NX_RESULT_DOMAIN_SECURITY = 6,
    NX_RESULT_DOMAIN_IMAGE = 7
} nx_result_domain;

typedef struct nx_result {
    uint32_t schema_id;
    uint16_t schema_version;
    uint16_t flags;
    nx_status status;
    uint32_t domain;
    uint64_t operation_id;
    uint64_t completed;
    uint64_t detail;
    uint64_t cycles_used;
    uint64_t fuel_used;
} nx_result;

nx_status nx_result_init(nx_result *result, nx_status status,
                         nx_result_domain domain, uint64_t operation_id,
                         uint64_t completed, uint64_t detail,
                         uint64_t cycles_used, uint64_t fuel_used);
nx_status nx_result_validate(const nx_result *result);

#endif
