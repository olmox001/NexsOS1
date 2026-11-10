/* NXOLMO base checked operations: canonical HAL-owned implementation.
 * Uses verified reference semantics, now exercised by HAL resource paths. */
#include "nxolmo/base/checked.h"
#include "nxolmo/base/span.h"
#include "nxolmo/base/status.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

nx_status nx_size_add(size_t left, size_t right, size_t *result) {
    if (result == NULL)
        return NX_STATUS_INVALID_ARGUMENT;
    *result = 0u;
    if (right > SIZE_MAX - left)
        return NX_STATUS_OVERFLOW;
    *result = left + right;
    return NX_STATUS_OK;
}

nx_status nx_size_mul(size_t left, size_t right, size_t *result) {
    if (result == NULL)
        return NX_STATUS_INVALID_ARGUMENT;
    *result = 0u;
    if (left != 0u && right > SIZE_MAX / left)
        return NX_STATUS_OVERFLOW;
    *result = left * right;
    return NX_STATUS_OK;
}

int nx_pointer_ranges_overlap(const void *left, size_t left_size,
                              const void *right, size_t right_size) {
    uintptr_t left_start;
    uintptr_t right_start;
    uintptr_t left_end;
    uintptr_t right_end;

    if (left_size == 0u || right_size == 0u)
        return 0;
    if (left == NULL || right == NULL)
        return -1;
    left_start = (uintptr_t)left;
    right_start = (uintptr_t)right;
    if ((uintmax_t)left_size > (uintmax_t)(UINTPTR_MAX - left_start) ||
        (uintmax_t)right_size > (uintmax_t)(UINTPTR_MAX - right_start))
        return -1;
    left_end = left_start + (uintptr_t)left_size;
    right_end = right_start + (uintptr_t)right_size;
    return left_start < right_end && right_start < left_end ? 1 : 0;
}

nx_status nx_u64_add(uint64_t left, uint64_t right, uint64_t *result) {
    if (result == NULL)
        return NX_STATUS_INVALID_ARGUMENT;
    *result = 0u;
    if (right > UINT64_MAX - left)
        return NX_STATUS_OVERFLOW;
    *result = left + right;
    return NX_STATUS_OK;
}

nx_status nx_u64_mul(uint64_t left, uint64_t right, uint64_t *result) {
    if (result == NULL)
        return NX_STATUS_INVALID_ARGUMENT;
    *result = 0u;
    if (left != 0u && right > UINT64_MAX / left)
        return NX_STATUS_OVERFLOW;
    *result = left * right;
    return NX_STATUS_OK;
}

nx_status nx_range_validate(uint64_t offset, uint64_t length,
                            uint64_t limit, uint64_t *end) {
    uint64_t computed_end = 0u;
    nx_status status;

    if (end == NULL)
        return NX_STATUS_INVALID_ARGUMENT;
    *end = 0u;
    status = nx_u64_add(offset, length, &computed_end);
    if (status != NX_STATUS_OK)
        return status;
    if (computed_end > limit)
        return NX_STATUS_LIMIT_EXCEEDED;
    *end = computed_end;
    return NX_STATUS_OK;
}

nx_status nx_const_span_validate(nx_const_span span, size_t required_size) {
    if (span.data == NULL && span.size != 0u)
        return NX_STATUS_INVALID_ARGUMENT;
    if (required_size > span.size)
        return NX_STATUS_TRUNCATED;
    return NX_STATUS_OK;
}

nx_status nx_mut_span_validate(nx_mut_span span, size_t required_size) {
    if (span.data == NULL && span.size != 0u)
        return NX_STATUS_INVALID_ARGUMENT;
    if (required_size > span.size)
        return NX_STATUS_NO_SPACE;
    return NX_STATUS_OK;
}
