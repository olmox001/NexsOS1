#ifndef NXEC_HCD_CRC64_H
#define NXEC_HCD_CRC64_H

#include <stdint.h>

#include "hcd_wire.h"

enum hcd_error hcd_crc64_ecma(
    const uint8_t *data,
    uint64_t size,
    uint64_t *checksum);

enum hcd_error hcd_crc64_hcd(
    const uint8_t *buffer,
    uint64_t size,
    uint64_t *checksum);

#endif
