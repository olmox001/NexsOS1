#include "multiboot2_info.h"

#include "common/hcd_endian.h"
#include "common/hcd_wire.h"

#include <stddef.h>
#include <stdint.h>

#define MULTIBOOT2_INFO_HEADER_SIZE UINT64_C(8)
#define MULTIBOOT2_TAG_HEADER_SIZE UINT64_C(8)
#define MULTIBOOT2_TAG_END_TYPE UINT32_C(0)
#define MULTIBOOT2_TAG_ALIGNMENT UINT64_C(8)

enum preboot_multiboot2_info_error preboot_multiboot2_info_iterator_init(
    const void *buffer,
    size_t capacity,
    struct preboot_multiboot2_info_iterator *iterator)
{
    const uint8_t *bytes = (const uint8_t *)buffer;
    uint64_t total_size;

    if ((buffer == (const void *)0) ||
        (iterator == (struct preboot_multiboot2_info_iterator *)0)) {
        return PREBOOT_MULTIBOOT2_INFO_E_NULL_INPUT;
    }
    iterator->buffer = (const uint8_t *)0;
    iterator->total_size = 0U;
    iterator->offset = 0U;
    iterator->finished = 0U;
    if (capacity < (size_t)MULTIBOOT2_INFO_HEADER_SIZE) {
        return PREBOOT_MULTIBOOT2_INFO_E_BAD_INFO;
    }
    total_size = (uint64_t)hcd_load_le32(bytes, 0U);
    if ((hcd_load_le32(bytes, 4U) != 0U) ||
        (total_size < (MULTIBOOT2_INFO_HEADER_SIZE +
                       MULTIBOOT2_TAG_HEADER_SIZE)) ||
        (total_size > (uint64_t)capacity) ||
        (total_size > (uint64_t)HCD_MAX_TOTAL_SIZE) ||
        ((total_size % MULTIBOOT2_TAG_ALIGNMENT) != 0U)) {
        return PREBOOT_MULTIBOOT2_INFO_E_BAD_INFO;
    }
    iterator->buffer = bytes;
    iterator->total_size = total_size;
    iterator->offset = MULTIBOOT2_INFO_HEADER_SIZE;
    return PREBOOT_MULTIBOOT2_INFO_OK;
}

static enum preboot_multiboot2_info_error next_tag_bounds(
    const struct preboot_multiboot2_info_iterator *iterator,
    uint64_t *tag_size,
    uint64_t *aligned_size,
    uint32_t *tag_type)
{
    uint64_t remaining;
    uint64_t remainder;

    if ((iterator->buffer == (const uint8_t *)0) ||
        (iterator->offset < MULTIBOOT2_INFO_HEADER_SIZE) ||
        (iterator->offset > iterator->total_size)) {
        return PREBOOT_MULTIBOOT2_INFO_E_BAD_INFO;
    }
    remaining = iterator->total_size - iterator->offset;
    if (remaining < MULTIBOOT2_TAG_HEADER_SIZE) {
        return PREBOOT_MULTIBOOT2_INFO_E_BAD_INFO;
    }
    *tag_type = hcd_load_le32(iterator->buffer, iterator->offset);
    *tag_size = (uint64_t)hcd_load_le32(
        iterator->buffer, iterator->offset + 4U);
    if ((*tag_size < MULTIBOOT2_TAG_HEADER_SIZE) ||
        (*tag_size > remaining)) {
        return PREBOOT_MULTIBOOT2_INFO_E_BAD_TAG;
    }
    remainder = *tag_size % MULTIBOOT2_TAG_ALIGNMENT;
    *aligned_size = *tag_size;
    if (remainder != 0U) {
        *aligned_size += MULTIBOOT2_TAG_ALIGNMENT - remainder;
    }
    if (*aligned_size > remaining) {
        return PREBOOT_MULTIBOOT2_INFO_E_BAD_TAG;
    }
    return PREBOOT_MULTIBOOT2_INFO_OK;
}

enum preboot_multiboot2_info_error preboot_multiboot2_info_iterator_next(
    struct preboot_multiboot2_info_iterator *iterator,
    struct preboot_multiboot2_tag *tag,
    uint32_t *has_tag)
{
    uint64_t tag_size = 0U;
    uint64_t aligned_size = 0U;
    uint32_t tag_type = 0U;
    enum preboot_multiboot2_info_error error;

    if ((iterator == (struct preboot_multiboot2_info_iterator *)0) ||
        (tag == (struct preboot_multiboot2_tag *)0) ||
        (has_tag == (uint32_t *)0)) {
        return PREBOOT_MULTIBOOT2_INFO_E_NULL_INPUT;
    }
    tag->buffer = (const uint8_t *)0;
    tag->size = 0U;
    tag->type = 0U;
    *has_tag = 0U;
    if (iterator->finished == 1U) {
        return PREBOOT_MULTIBOOT2_INFO_OK;
    }
    if (iterator->finished != 0U) {
        return PREBOOT_MULTIBOOT2_INFO_E_BAD_INFO;
    }
    error = next_tag_bounds(iterator, &tag_size, &aligned_size, &tag_type);
    if (error != PREBOOT_MULTIBOOT2_INFO_OK) {
        return error;
    }
    if (tag_type == MULTIBOOT2_TAG_END_TYPE) {
        if ((tag_size != MULTIBOOT2_TAG_HEADER_SIZE) ||
            ((iterator->offset + tag_size) != iterator->total_size)) {
            return PREBOOT_MULTIBOOT2_INFO_E_BAD_TAG;
        }
        iterator->offset = iterator->total_size;
        iterator->finished = 1U;
        return PREBOOT_MULTIBOOT2_INFO_OK;
    }
    tag->buffer = iterator->buffer + iterator->offset;
    tag->size = (size_t)tag_size;
    tag->type = tag_type;
    iterator->offset += aligned_size;
    *has_tag = 1U;
    return PREBOOT_MULTIBOOT2_INFO_OK;
}
