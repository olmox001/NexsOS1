#include "multiboot2_memory_map.h"

#include "common/hcd_endian.h"
#include "common/hcd_wire.h"
#include "capability/multiboot2_info.h"

#include <stddef.h>
#include <stdint.h>

#define MULTIBOOT2_TAG_TYPE_MMAP UINT32_C(6)
#define MULTIBOOT2_MMAP_TAG_HEADER_SIZE UINT64_C(16)
#define MULTIBOOT2_MMAP_ENTRY_BASE_SIZE UINT64_C(24)
#define MULTIBOOT2_MMAP_ENTRY_VERSION UINT32_C(0)
#define MULTIBOOT2_MEMORY_AVAILABLE UINT32_C(1)
#define MULTIBOOT2_MEMORY_RESERVED UINT32_C(2)
#define MULTIBOOT2_MEMORY_ACPI_RECLAIMABLE UINT32_C(3)
#define MULTIBOOT2_MEMORY_NVS UINT32_C(4)
#define MULTIBOOT2_MEMORY_BADRAM UINT32_C(5)
#define HCD_MEMORY_RECORD_SIZE UINT64_C(40)

static int preboot_multiboot2_ranges_overlap(
    const void *left,
    size_t left_size,
    const void *right,
    size_t right_size,
    int *overlap)
{
    uintptr_t left_start = (uintptr_t)left;
    uintptr_t right_start = (uintptr_t)right;
    uintptr_t left_end = 0U;
    uintptr_t right_end = 0U;

    *overlap = 0;
    if ((uintmax_t)left_size >
            (uintmax_t)(UINTPTR_MAX - left_start) ||
        (uintmax_t)right_size >
            (uintmax_t)(UINTPTR_MAX - right_start)) {
        return 0;
    }
    left_end = left_start + (uintptr_t)left_size;
    right_end = right_start + (uintptr_t)right_size;
    if ((left_size != 0U) && (right_size != 0U) &&
        (left_start < right_end) && (right_start < left_end)) {
        *overlap = 1;
    }
    return 1;
}

static enum preboot_multiboot2_memory_map_error
preboot_multiboot2_require_disjoint(
    const void *left,
    size_t left_size,
    const void *right,
    size_t right_size)
{
    int overlap = 0;

    if (!preboot_multiboot2_ranges_overlap(
            left, left_size, right, right_size, &overlap)) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_BUFFER_RANGE;
    }
    if (overlap != 0) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_OUTPUT_OVERLAP;
    }
    return PREBOOT_MULTIBOOT2_MEMORY_MAP_OK;
}

enum preboot_multiboot2_memory_map_error
preboot_multiboot2_get_memory_map_tag(
    const void *information,
    size_t information_capacity,
    const void **tag,
    size_t *tag_size)
{
    struct preboot_multiboot2_info_iterator iterator;
    struct preboot_multiboot2_tag current_tag;
    const void *found_tag = (const void *)0;
    size_t found_size = 0U;
    uint32_t has_tag = 0U;
    uint32_t found = 0U;
    enum preboot_multiboot2_info_error info_error;

    if ((tag == (const void **)0) || (tag_size == (size_t *)0)) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_NULL_INPUT;
    }
    if (information == (const void *)0) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_NULL_INPUT;
    }
    {
        enum preboot_multiboot2_memory_map_error range_error =
            preboot_multiboot2_require_disjoint(
                information, information_capacity, (const void *)tag,
                sizeof(*tag));
        if (range_error != PREBOOT_MULTIBOOT2_MEMORY_MAP_OK) {
            return range_error;
        }
        range_error = preboot_multiboot2_require_disjoint(
            information, information_capacity, tag_size, sizeof(*tag_size));
        if (range_error != PREBOOT_MULTIBOOT2_MEMORY_MAP_OK) {
            return range_error;
        }
        range_error = preboot_multiboot2_require_disjoint(
            (const void *)tag, sizeof(*tag), tag_size, sizeof(*tag_size));
        if (range_error != PREBOOT_MULTIBOOT2_MEMORY_MAP_OK) {
            return range_error;
        }
    }
    *tag = (const void *)0;
    *tag_size = 0U;
    info_error = preboot_multiboot2_info_iterator_init(
        information, information_capacity, &iterator);
    if (info_error == PREBOOT_MULTIBOOT2_INFO_E_NULL_INPUT) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_NULL_INPUT;
    }
    if (info_error != PREBOOT_MULTIBOOT2_INFO_OK) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_INFO;
    }
    for (;;) {
        info_error = preboot_multiboot2_info_iterator_next(
            &iterator, &current_tag, &has_tag);
        if (info_error == PREBOOT_MULTIBOOT2_INFO_E_BAD_TAG) {
            return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_TAG;
        }
        if (info_error != PREBOOT_MULTIBOOT2_INFO_OK) {
            return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_INFO;
        }
        if (has_tag == 0U) {
            break;
        }
        if (current_tag.type == MULTIBOOT2_TAG_TYPE_MMAP) {
            if (found != 0U) {
                return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_DUPLICATE_MAP;
            }
            found_tag = current_tag.buffer;
            found_size = current_tag.size;
            found = 1U;
        }
    }
    if (found == 0U) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_MAP_NOT_FOUND;
    }
    *tag = found_tag;
    *tag_size = found_size;
    return PREBOOT_MULTIBOOT2_MEMORY_MAP_OK;
}

static int map_memory_type(uint32_t source_type, uint32_t *target_type)
{
    switch (source_type) {
    case MULTIBOOT2_MEMORY_AVAILABLE:
        *target_type = (uint32_t)HCD_MEMORY_USABLE;
        return 1;
    case MULTIBOOT2_MEMORY_RESERVED:
        *target_type = (uint32_t)HCD_MEMORY_RESERVED;
        return 1;
    case MULTIBOOT2_MEMORY_ACPI_RECLAIMABLE:
        *target_type = (uint32_t)HCD_MEMORY_ACPI_RECLAIMABLE;
        return 1;
    case MULTIBOOT2_MEMORY_NVS:
        *target_type = (uint32_t)HCD_MEMORY_ACPI_NVS;
        return 1;
    case MULTIBOOT2_MEMORY_BADRAM:
        *target_type = (uint32_t)HCD_MEMORY_UNUSABLE;
        return 1;
    default:
        return 0;
    }
}

static void write_memory_record(
    uint8_t *output,
    uint32_t index,
    uint64_t base,
    uint64_t length,
    uint32_t type)
{
    uint64_t offset = (uint64_t)index * HCD_MEMORY_RECORD_SIZE;

    hcd_store_le64(output, offset, base);
    hcd_store_le64(output, offset + 8U, length);
    hcd_store_le32(output, offset + 16U, type);
    hcd_store_le32(output, offset + 20U, 0U);
    hcd_store_le32(output, offset + 24U, HCD_ID_UNKNOWN);
    hcd_store_le32(output, offset + 28U, HCD_ID_UNKNOWN);
    hcd_store_le64(output, offset + 32U, 0U);
}

static enum preboot_multiboot2_memory_map_error validate_tag(
    const uint8_t *tag,
    size_t tag_size,
    uint64_t *entry_size,
    uint64_t *entry_count)
{
    uint32_t declared_size;
    uint32_t version;
    uint64_t payload_size;

    if ((tag_size < MULTIBOOT2_MMAP_TAG_HEADER_SIZE) ||
        (tag_size > (size_t)HCD_MAX_TOTAL_SIZE)) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_TAG;
    }
    if (hcd_load_le32(tag, 0U) != MULTIBOOT2_TAG_TYPE_MMAP) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_TAG;
    }
    declared_size = hcd_load_le32(tag, 4U);
    if (((uint64_t)declared_size != (uint64_t)tag_size) ||
        (declared_size < MULTIBOOT2_MMAP_TAG_HEADER_SIZE)) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_TAG;
    }
    *entry_size = (uint64_t)hcd_load_le32(tag, 8U);
    version = hcd_load_le32(tag, 12U);
    if (*entry_size < MULTIBOOT2_MMAP_ENTRY_BASE_SIZE) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_ENTRY_SIZE;
    }
    if (version != MULTIBOOT2_MMAP_ENTRY_VERSION) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_ENTRY_VERSION;
    }
    payload_size = (uint64_t)declared_size - MULTIBOOT2_MMAP_TAG_HEADER_SIZE;
    if ((payload_size % *entry_size) != 0U) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_TAG;
    }
    *entry_count = payload_size / *entry_size;
    if (*entry_count == 0U) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_TAG;
    }
    return PREBOOT_MULTIBOOT2_MEMORY_MAP_OK;
}

static enum preboot_multiboot2_memory_map_error convert_entries(
    const uint8_t *tag,
    uint8_t *output,
    uint64_t entry_size,
    uint64_t entry_count,
    size_t output_capacity,
    uint32_t *record_count)
{
    uint64_t index;
    uint64_t entry_offset;
    uint64_t base;
    uint64_t length;
    uint64_t end;
    uint64_t previous_end = 0U;
    uint64_t previous_offset;
    uint32_t source_type;
    uint32_t target_type;
    uint32_t output_count = 0U;

    for (index = 0U; index < entry_count; ++index) {
        entry_offset = MULTIBOOT2_MMAP_TAG_HEADER_SIZE + (index * entry_size);
        base = hcd_load_le64(tag, entry_offset);
        length = hcd_load_le64(tag, entry_offset + 8U);
        source_type = hcd_load_le32(tag, entry_offset + 16U);
        if ((hcd_load_le32(tag, entry_offset + 20U) != 0U) ||
            (length == 0U) || (base > (UINT64_MAX - length))) {
            return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_ENTRY;
        }
        if (!map_memory_type(source_type, &target_type)) {
            return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_UNSUPPORTED_TYPE;
        }
        end = base + length;
        if (index > 0U) {
            previous_offset =
                (uint64_t)(output_count - 1U) * HCD_MEMORY_RECORD_SIZE;
            if (base < hcd_load_le64(output, previous_offset)) {
                return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_UNSORTED;
            }
            if (base < previous_end) {
                return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_OVERLAP;
            }
            if ((base == previous_end) &&
                (target_type == hcd_load_le32(output, previous_offset + 16U))) {
                hcd_store_le64(
                    output, previous_offset + 8U,
                    end - hcd_load_le64(output, previous_offset));
                previous_end = end;
                continue;
            }
        }
        if ((((uint64_t)output_count + 1U) * HCD_MEMORY_RECORD_SIZE) >
            (uint64_t)output_capacity) {
            return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_CAPACITY;
        }
        if (output_count >= HCD_MAX_MEMORY_REGIONS) {
            return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_CAPACITY;
        }
        write_memory_record(output, output_count, base, length, target_type);
        ++output_count;
        previous_end = end;
    }
    *record_count = output_count;
    return PREBOOT_MULTIBOOT2_MEMORY_MAP_OK;
}

enum preboot_multiboot2_memory_map_error
preboot_multiboot2_normalize_memory_map(
    const void *tag,
    size_t tag_size,
    void *output,
    size_t output_capacity,
    uint32_t *record_count)
{
    const uint8_t *input = (const uint8_t *)tag;
    uint8_t *records = (uint8_t *)output;
    uint64_t entry_size = 0U;
    uint64_t entry_count = 0U;
    enum preboot_multiboot2_memory_map_error error;

    if (record_count == (uint32_t *)0) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_NULL_INPUT;
    }
    if (tag != (const void *)0) {
        error = preboot_multiboot2_require_disjoint(
            tag, tag_size, record_count, sizeof(*record_count));
        if (error != PREBOOT_MULTIBOOT2_MEMORY_MAP_OK) {
            return error;
        }
    }
    if (output != (void *)0) {
        error = preboot_multiboot2_require_disjoint(
            output, output_capacity, record_count, sizeof(*record_count));
        if (error != PREBOOT_MULTIBOOT2_MEMORY_MAP_OK) {
            return error;
        }
    }
    *record_count = 0U;
    if ((tag != (const void *)0) && (output != (void *)0)) {
        error = preboot_multiboot2_require_disjoint(
            tag, tag_size, output, output_capacity);
        if (error != PREBOOT_MULTIBOOT2_MEMORY_MAP_OK) {
            return error;
        }
    }
    if ((tag == (const void *)0) || (output == (void *)0)) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_NULL_INPUT;
    }
    error = validate_tag(input, tag_size, &entry_size, &entry_count);
    if (error != PREBOOT_MULTIBOOT2_MEMORY_MAP_OK) {
        return error;
    }
    return convert_entries(
        input, records, entry_size, entry_count, output_capacity, record_count);
}

enum preboot_multiboot2_memory_map_error
preboot_multiboot2_normalize_information_memory_map(
    const void *information,
    size_t information_capacity,
    void *output,
    size_t output_capacity,
    uint32_t *record_count)
{
    const void *tag = (const void *)0;
    size_t tag_size = 0U;
    size_t information_size = 0U;
    enum preboot_multiboot2_memory_map_error error;

    if (record_count == (uint32_t *)0) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_NULL_INPUT;
    }
    if (information != (const void *)0) {
        error = preboot_multiboot2_require_disjoint(
            information, information_capacity,
            record_count, sizeof(*record_count));
        if (error != PREBOOT_MULTIBOOT2_MEMORY_MAP_OK) {
            return error;
        }
    }
    if (output != (void *)0) {
        error = preboot_multiboot2_require_disjoint(
            output, output_capacity, record_count, sizeof(*record_count));
        if (error != PREBOOT_MULTIBOOT2_MEMORY_MAP_OK) {
            return error;
        }
    }
    *record_count = 0U;
    if ((information != (const void *)0) && (output != (void *)0)) {
        error = preboot_multiboot2_require_disjoint(
            information, information_capacity, output, output_capacity);
        if (error != PREBOOT_MULTIBOOT2_MEMORY_MAP_OK) {
            return error;
        }
    }
    if ((information == (const void *)0) || (output == (void *)0)) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_NULL_INPUT;
    }
    error = preboot_multiboot2_get_memory_map_tag(
        information, information_capacity, &tag, &tag_size);
    if (error != PREBOOT_MULTIBOOT2_MEMORY_MAP_OK) {
        return error;
    }
    information_size = (size_t)hcd_load_le32(information, 0U);
    error = preboot_multiboot2_require_disjoint(
        information, information_size, output, output_capacity);
    if (error != PREBOOT_MULTIBOOT2_MEMORY_MAP_OK) {
        return error;
    }
    if (information_size > information_capacity) {
        return PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_BUFFER_RANGE;
    }
    return preboot_multiboot2_normalize_memory_map(
        tag, tag_size, output, output_capacity, record_count);
}
