#ifndef NXEC_HCD_ENDIAN_H
#define NXEC_HCD_ENDIAN_H

#include "hcd_assert.h"

#include <stddef.h>
#include <stdint.h>

static inline uint16_t hcd_load_le16(const uint8_t *buffer, uint64_t offset)
{
    HCD_ASSERT(buffer != NULL);
    HCD_ASSERT(sizeof(uint16_t) == 2U);
    {
        uint32_t value = (uint32_t)buffer[offset] |
            ((uint32_t)buffer[offset + 1U] << 8U);
        return (uint16_t)value;
    }
}

static inline uint32_t hcd_load_le32(const uint8_t *buffer, uint64_t offset)
{
    uint32_t value = 0U;
    uint32_t index = 0U;

    HCD_ASSERT(buffer != NULL);
    HCD_ASSERT(sizeof(uint32_t) == 4U);
    for (index = 0U; index < 4U; ++index) {
        value |= (uint32_t)buffer[offset + index] << (index * 8U);
    }
    return value;
}

static inline uint64_t hcd_load_le64(const uint8_t *buffer, uint64_t offset)
{
    uint64_t value = 0U;
    uint32_t index = 0U;

    HCD_ASSERT(buffer != NULL);
    HCD_ASSERT(sizeof(uint64_t) == 8U);
    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)buffer[offset + index] << (index * 8U);
    }
    return value;
}


static inline void hcd_store_le16(uint8_t *buffer, uint64_t offset,
                                  uint16_t value)
{
    HCD_ASSERT(buffer != NULL);
    buffer[offset] = (uint8_t)value;
    buffer[offset + 1U] = (uint8_t)(value >> 8U);
}

static inline void hcd_store_le32(uint8_t *buffer, uint64_t offset,
                                  uint32_t value)
{
    uint32_t index = 0U;

    HCD_ASSERT(buffer != NULL);
    for (index = 0U; index < 4U; ++index) {
        buffer[offset + index] = (uint8_t)(value >> (index * 8U));
    }
    HCD_ASSERT(index == 4U);
}

static inline void hcd_store_le64(uint8_t *buffer, uint64_t offset,
                                  uint64_t value)
{
    uint32_t index = 0U;

    HCD_ASSERT(buffer != NULL);
    for (index = 0U; index < 8U; ++index) {
        buffer[offset + index] = (uint8_t)(value >> (index * 8U));
    }
    HCD_ASSERT(index == 8U);
}

#endif
