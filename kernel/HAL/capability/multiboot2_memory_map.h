#ifndef NXEC_PREBOOT_MULTIBOOT2_MEMORY_MAP_H
#define NXEC_PREBOOT_MULTIBOOT2_MEMORY_MAP_H

#include <stddef.h>
#include <stdint.h>

enum preboot_multiboot2_memory_map_error {
    PREBOOT_MULTIBOOT2_MEMORY_MAP_OK = 0,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_NULL_INPUT = 1,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_TAG = 2,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_ENTRY_SIZE = 3,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_ENTRY_VERSION = 4,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_ENTRY = 5,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_UNSUPPORTED_TYPE = 6,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_OVERLAP = 7,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_UNSORTED = 8,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_CAPACITY = 9,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_INFO = 10,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_MAP_NOT_FOUND = 11,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_DUPLICATE_MAP = 12,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_BUFFER_RANGE = 13,
    PREBOOT_MULTIBOOT2_MEMORY_MAP_E_OUTPUT_OVERLAP = 14
};

/*
 * Finds the unique BIOS memory-map tag (type 6) in a complete information
 * block. The returned tag refers to the caller-owned input buffer. The output
 * parameters must be disjoint from the information buffer and from each
 * other; an overlap is rejected without modifying any buffer.
 */
enum preboot_multiboot2_memory_map_error
preboot_multiboot2_get_memory_map_tag(
    const void *information,
    size_t information_capacity,
    const void **tag,
    size_t *tag_size);

/*
 * Converts one complete Multiboot2 memory-map tag (type 6) to sorted,
 * coalesced hcd_memory_record wire records in caller-owned output storage.
 * The tag must remain readable for the call and its byte range must not
 * overlap the output-capacity range. record_count must be disjoint from both
 * buffers. On ordinary errors, record_count is zero and output must not be
 * used as a normalized map; on a range/overlap error, no aliased buffer is
 * modified.
 */
enum preboot_multiboot2_memory_map_error
preboot_multiboot2_normalize_memory_map(
    const void *tag,
    size_t tag_size,
    void *output,
    size_t output_capacity,
    uint32_t *record_count);

/*
 * Extracts and normalizes the unique type-6 tag from a complete BIOS
 * Multiboot2 information block. The input remains caller-owned and readable
 * for the call, and its capacity range must not overlap output storage.
 * record_count must be disjoint from both ranges. On ordinary errors,
 * record_count is zero and output is not valid; on a range/overlap error, no
 * aliased buffer is modified.
 */
enum preboot_multiboot2_memory_map_error
preboot_multiboot2_normalize_information_memory_map(
    const void *information,
    size_t information_capacity,
    void *output,
    size_t output_capacity,
    uint32_t *record_count);

#endif
