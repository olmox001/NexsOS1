#include <stdint.h>

#include "hcd_wire.h"
#include "hcd_crc64.h"
#include "hcd_assert.h"

static const uint64_t hcd_crc64_table[256] = {
#include "hcd_crc64_table.inc"
};

static uint64_t hcd_crc64_update(
    uint64_t state,
    const uint8_t *data,
    uint64_t size)
{
    uint64_t index = 0U;

    HCD_ASSERT((data != NULL) || (size == 0U));
    HCD_ASSERT(size <= HCD_MAX_TOTAL_SIZE);

    for (index = 0U; index < HCD_MAX_TOTAL_SIZE; ++index) {
        uint8_t table_index = 0U;

        if (index >= size) {
            break;
        }
        table_index = (uint8_t)((state >> 56U) ^ data[index]);
        state = hcd_crc64_table[table_index] ^ (state << 8U);
    }
    return state;
}

enum hcd_error hcd_crc64_ecma(
    const uint8_t *data,
    uint64_t size,
    uint64_t *checksum)
{
    if ((data == NULL) || (checksum == NULL)) {
        return HCD_E_NULL_INPUT;
    }
    if (size > HCD_MAX_TOTAL_SIZE) {
        return HCD_E_BAD_TOTAL_SIZE;
    }
    HCD_ASSERT(data != NULL);
    HCD_ASSERT(checksum != NULL);
    HCD_ASSERT(size <= HCD_MAX_TOTAL_SIZE);

    *checksum = hcd_crc64_update(0U, data, size);
    return HCD_OK;
}

enum hcd_error hcd_crc64_hcd(
    const uint8_t *buffer,
    uint64_t size,
    uint64_t *checksum)
{
    static const uint8_t zero_checksum[sizeof(uint64_t)] = {0U};
    uint64_t state = 0U;

    if ((buffer == NULL) || (checksum == NULL)) {
        return HCD_E_NULL_INPUT;
    }
    if ((size < HCD_HEADER_SIZE) || (size > HCD_MAX_TOTAL_SIZE)) {
        return HCD_E_BAD_TOTAL_SIZE;
    }
    HCD_ASSERT(buffer != NULL);
    HCD_ASSERT(checksum != NULL);
    HCD_ASSERT(size >= HCD_HEADER_SIZE);

    state = hcd_crc64_update(state, buffer, 24U);
    state = hcd_crc64_update(state, zero_checksum, sizeof(zero_checksum));
    state = hcd_crc64_update(state, &buffer[32U], size - 32U);
    *checksum = state;
    return HCD_OK;
}
