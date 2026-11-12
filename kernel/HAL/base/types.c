#include "nxolmo/base/types.h"
#include <stdint.h>

int nx_object_id_equal(nx_object_id left, nx_object_id right) {
    return left.high == right.high && left.low == right.low;
}

int nx_object_id_is_zero(nx_object_id value) {
    return value.high == 0u && value.low == 0u;
}

int nx_object_ref_equal(nx_object_ref left, nx_object_ref right) {
    return nx_object_id_equal(left.id, right.id) &&
           left.generation == right.generation;
}

int nx_object_ref_is_valid(nx_object_ref value) {
    return !nx_object_id_is_zero(value.id) && value.generation != 0u;
}
