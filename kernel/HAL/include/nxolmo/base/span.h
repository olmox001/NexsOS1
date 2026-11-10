#ifndef NXOLMO_BASE_SPAN_H
#define NXOLMO_BASE_SPAN_H

#include <stddef.h>
#include <stdint.h>

typedef struct nx_const_span {
    const uint8_t *data;
    size_t size;
} nx_const_span;

typedef struct nx_mut_span {
    uint8_t *data;
    size_t size;
} nx_mut_span;

#endif
