#ifndef NXOLMO_BASE_TYPES_H
#define NXOLMO_BASE_TYPES_H

#include <stdint.h>

#define NX_CYCLES_UNKNOWN UINT64_MAX

typedef struct nx_object_id {
    uint64_t high;
    uint64_t low;
} nx_object_id;

typedef struct nx_object_ref {
    nx_object_id id;
    uint64_t generation;
} nx_object_ref;

typedef struct nx_digest256 {
    uint8_t bytes[32];
} nx_digest256;

int nx_object_id_equal(nx_object_id left, nx_object_id right);
int nx_object_id_is_zero(nx_object_id value);
int nx_object_ref_equal(nx_object_ref left, nx_object_ref right);
int nx_object_ref_is_valid(nx_object_ref value);

#endif
