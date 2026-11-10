#ifndef NXOLMO_BASE_CHECKED_H
#define NXOLMO_BASE_CHECKED_H

#include "nxolmo/base/span.h"
#include "nxolmo/base/status.h"

nx_status nx_size_add(size_t left, size_t right, size_t *result);
nx_status nx_size_mul(size_t left, size_t right, size_t *result);
/* -1 for invalid/overflowing ranges, 0 for disjoint, 1 for overlap. */
int nx_pointer_ranges_overlap(const void *left, size_t left_size,
                              const void *right, size_t right_size);
nx_status nx_u64_add(uint64_t left, uint64_t right, uint64_t *result);
nx_status nx_u64_mul(uint64_t left, uint64_t right, uint64_t *result);
nx_status nx_range_validate(uint64_t offset, uint64_t length,
                            uint64_t limit, uint64_t *end);
nx_status nx_const_span_validate(nx_const_span span, size_t required_size);
nx_status nx_mut_span_validate(nx_mut_span span, size_t required_size);

#endif
