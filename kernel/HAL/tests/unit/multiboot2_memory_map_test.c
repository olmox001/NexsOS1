#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "common/hcd_endian.h"
#include "common/hcd_wire.h"
#include "capability/multiboot2_info.h"
#include "capability/multiboot2_memory_map.h"

#define TEST_TAG_HEADER_SIZE ((size_t)16U)
#define TEST_ENTRY_SIZE ((size_t)24U)
#define TEST_EXTENDED_ENTRY_SIZE ((size_t)32U)
#define TEST_TAG_TYPE_MMAP UINT32_C(6)
#define TEST_INFO_HEADER_SIZE ((size_t)8U)
#define TEST_END_TAG_SIZE ((size_t)8U)

static void write_entry(
    uint8_t *tag,
    size_t stride,
    uint32_t index,
    uint64_t base,
    uint64_t length,
    uint32_t type)
{
    uint64_t offset = TEST_TAG_HEADER_SIZE + ((uint64_t)index * (uint64_t)stride);
    hcd_store_le64(tag, offset, base);
    hcd_store_le64(tag, offset + 8U, length);
    hcd_store_le32(tag, offset + 16U, type);
    hcd_store_le32(tag, offset + 20U, 0U);
}

static void initialize_tag(uint8_t *tag, size_t tag_size, size_t entry_size)
{
    hcd_store_le32(tag, 0U, TEST_TAG_TYPE_MMAP);
    hcd_store_le32(tag, 4U, (uint32_t)tag_size);
    hcd_store_le32(tag, 8U, (uint32_t)entry_size);
    hcd_store_le32(tag, 12U, 0U);
}

static void test_converts_and_coalesces_sorted_entries(void)
{
    uint8_t tag[TEST_TAG_HEADER_SIZE + (TEST_ENTRY_SIZE * 5U)] = {0U};
    uint8_t output[5U * 40U] = {0U};
    uint32_t count = UINT32_MAX;

    initialize_tag(tag, sizeof(tag), TEST_ENTRY_SIZE);
    write_entry(tag, TEST_ENTRY_SIZE, 0U, 0x1000U, 0x1000U, 1U);
    write_entry(tag, TEST_ENTRY_SIZE, 1U, 0x2000U, 0x1000U, 1U);
    write_entry(tag, TEST_ENTRY_SIZE, 2U, 0x4000U, 0x1000U, 3U);
    write_entry(tag, TEST_ENTRY_SIZE, 3U, 0x6000U, 0x1000U, 4U);
    write_entry(tag, TEST_ENTRY_SIZE, 4U, 0x8000U, 0x1000U, 5U);

    assert(preboot_multiboot2_normalize_memory_map(
               tag, sizeof(tag), output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_OK);
    assert(count == 4U);
    assert(hcd_load_le64(output, 0U) == UINT64_C(0x1000));
    assert(hcd_load_le64(output, 8U) == UINT64_C(0x2000));
    assert(hcd_load_le32(output, 16U) == HCD_MEMORY_USABLE);
    assert(hcd_load_le32(output, 24U) == HCD_ID_UNKNOWN);
    assert(hcd_load_le32(output, 28U) == HCD_ID_UNKNOWN);
    assert(hcd_load_le32(output, 56U) == HCD_MEMORY_ACPI_RECLAIMABLE);
    assert(hcd_load_le32(output, 96U) == HCD_MEMORY_ACPI_NVS);
    assert(hcd_load_le32(output, 136U) == HCD_MEMORY_UNUSABLE);
}

static void test_accepts_extended_entry_stride(void)
{
    uint8_t tag[TEST_TAG_HEADER_SIZE + TEST_EXTENDED_ENTRY_SIZE] = {0U};
    uint8_t output[40U] = {0U};
    uint32_t count = 0U;

    initialize_tag(tag, sizeof(tag), TEST_EXTENDED_ENTRY_SIZE);
    write_entry(tag, TEST_EXTENDED_ENTRY_SIZE, 0U, 0x1000U, 0x1000U, 2U);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, sizeof(tag), output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_OK);
    assert(count == 1U);
    assert(hcd_load_le32(output, 16U) == HCD_MEMORY_RESERVED);
}

static void test_rejects_bad_tag_and_metadata(void)
{
    uint8_t tag[TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE] = {0U};
    uint8_t output[40U] = {0U};
    uint32_t count = UINT32_MAX;

    initialize_tag(tag, sizeof(tag), TEST_ENTRY_SIZE);
    write_entry(tag, TEST_ENTRY_SIZE, 0U, 0x1000U, 0x1000U, 1U);
    hcd_store_le32(tag, 0U, 7U);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, sizeof(tag), output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_TAG);
    assert(count == 0U);

    initialize_tag(tag, sizeof(tag) - 1U, TEST_ENTRY_SIZE);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, sizeof(tag), output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_TAG);

    initialize_tag(tag, sizeof(tag), TEST_ENTRY_SIZE);
    hcd_store_le32(tag, 8U, 23U);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, sizeof(tag), output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_ENTRY_SIZE);

    hcd_store_le32(tag, 8U, TEST_ENTRY_SIZE);
    hcd_store_le32(tag, 12U, 1U);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, sizeof(tag), output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_ENTRY_VERSION);
}

static void test_rejects_invalid_entries(void)
{
    uint8_t tag[TEST_TAG_HEADER_SIZE + (TEST_ENTRY_SIZE * 2U)] = {0U};
    uint8_t output[2U * 40U] = {0U};
    uint32_t count = 0U;

    initialize_tag(tag, sizeof(tag), TEST_ENTRY_SIZE);
    write_entry(tag, TEST_ENTRY_SIZE, 0U, 0x2000U, 0x1000U, 1U);
    write_entry(tag, TEST_ENTRY_SIZE, 1U, 0x1000U, 0x1000U, 1U);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, sizeof(tag), output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_UNSORTED);
    assert(count == 0U);

    write_entry(tag, TEST_ENTRY_SIZE, 0U, 0x1000U, 0x2000U, 1U);
    write_entry(tag, TEST_ENTRY_SIZE, 1U, 0x2000U, 0x1000U, 2U);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, sizeof(tag), output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_OVERLAP);

    hcd_store_le32(tag, 4U, (uint32_t)(TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE));
    write_entry(tag, TEST_ENTRY_SIZE, 0U, 0x1000U, 0x1000U, 99U);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE,
               output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_UNSUPPORTED_TYPE);

    write_entry(tag, TEST_ENTRY_SIZE, 0U, 0x1000U, 0U, 1U);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE,
               output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_ENTRY);

    write_entry(tag, TEST_ENTRY_SIZE, 0U, UINT64_MAX - 0xFFFU, 0x1000U, 1U);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE,
               output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_ENTRY);
}

static void test_rejects_reserved_and_capacity_errors(void)
{
    uint8_t tag[TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE] = {0U};
    uint8_t output[40U] = {0U};
    uint32_t count = 0U;

    initialize_tag(tag, sizeof(tag), TEST_ENTRY_SIZE);
    write_entry(tag, TEST_ENTRY_SIZE, 0U, 0x1000U, 0x1000U, 1U);
    hcd_store_le32(tag, TEST_TAG_HEADER_SIZE + 20U, 1U);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, sizeof(tag), output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_ENTRY);

    hcd_store_le32(tag, TEST_TAG_HEADER_SIZE + 20U, 0U);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, sizeof(tag), output, sizeof(output) - 1U, &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_CAPACITY);
}

static void test_extracts_tag_from_complete_information_block(void)
{
    uint8_t info[TEST_INFO_HEADER_SIZE + TEST_TAG_HEADER_SIZE +
                 TEST_ENTRY_SIZE + TEST_END_TAG_SIZE] = {0U};
    uint8_t output[40U] = {0U};
    const void *tag = (const void *)0;
    size_t tag_size = 0U;
    uint32_t count = 0U;
    size_t end_offset = TEST_INFO_HEADER_SIZE + TEST_TAG_HEADER_SIZE +
        TEST_ENTRY_SIZE;

    hcd_store_le32(info, 0U, (uint32_t)sizeof(info));
    initialize_tag(
        info + TEST_INFO_HEADER_SIZE,
        TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE, TEST_ENTRY_SIZE);
    write_entry(
        info + TEST_INFO_HEADER_SIZE, TEST_ENTRY_SIZE, 0U,
        0x1000U, 0x2000U, 1U);
    hcd_store_le32(info, end_offset, 0U);
    hcd_store_le32(info, end_offset + 4U, (uint32_t)TEST_END_TAG_SIZE);

    assert(preboot_multiboot2_get_memory_map_tag(
               info, sizeof(info), &tag, &tag_size) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_OK);
    assert(tag == (const void *)(info + TEST_INFO_HEADER_SIZE));
    assert(tag_size == TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE);
    assert(preboot_multiboot2_normalize_information_memory_map(
               info, sizeof(info), output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_OK);
    assert(count == 1U);
    assert(hcd_load_le64(output, 0U) == UINT64_C(0x1000));
    assert(hcd_load_le64(output, 8U) == UINT64_C(0x2000));
    assert(hcd_load_le32(output, 16U) == HCD_MEMORY_USABLE);
}

static void test_rejects_missing_duplicate_and_malformed_info_maps(void)
{
    uint8_t no_map[TEST_INFO_HEADER_SIZE + TEST_END_TAG_SIZE] = {0U};
    uint8_t duplicate[TEST_INFO_HEADER_SIZE +
                      (2U * (TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE)) +
                      TEST_END_TAG_SIZE] = {0U};
    uint8_t sentinel = 0U;
    const void *tag = &sentinel;
    size_t tag_size = SIZE_MAX;
    uint32_t count = UINT32_MAX;
    size_t first_tag_size = TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE;
    size_t second_offset = TEST_INFO_HEADER_SIZE + first_tag_size;
    size_t end_offset = second_offset + first_tag_size;

    hcd_store_le32(no_map, 0U, (uint32_t)sizeof(no_map));
    hcd_store_le32(no_map, TEST_INFO_HEADER_SIZE, 0U);
    hcd_store_le32(
        no_map, TEST_INFO_HEADER_SIZE + 4U, (uint32_t)TEST_END_TAG_SIZE);
    assert(preboot_multiboot2_get_memory_map_tag(
               no_map, sizeof(no_map), &tag, &tag_size) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_MAP_NOT_FOUND);
    assert(tag == (const void *)0);
    assert(tag_size == 0U);
    assert(preboot_multiboot2_normalize_information_memory_map(
               no_map, sizeof(no_map), &sentinel, sizeof(sentinel), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_MAP_NOT_FOUND);
    assert(count == 0U);

    hcd_store_le32(duplicate, 0U, (uint32_t)sizeof(duplicate));
    initialize_tag(duplicate + TEST_INFO_HEADER_SIZE, first_tag_size,
                   TEST_ENTRY_SIZE);
    initialize_tag(duplicate + second_offset, first_tag_size,
                   TEST_ENTRY_SIZE);
    hcd_store_le32(duplicate, end_offset, 0U);
    hcd_store_le32(
        duplicate, end_offset + 4U, (uint32_t)TEST_END_TAG_SIZE);
    assert(preboot_multiboot2_get_memory_map_tag(
               duplicate, sizeof(duplicate), &tag, &tag_size) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_DUPLICATE_MAP);
    assert(tag == (const void *)0);
    assert(tag_size == 0U);
    assert(preboot_multiboot2_normalize_information_memory_map(
               duplicate, sizeof(duplicate), &sentinel, sizeof(sentinel),
               &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_DUPLICATE_MAP);
    assert(count == 0U);

    hcd_store_le32(duplicate, 4U, 1U);
    assert(preboot_multiboot2_get_memory_map_tag(
               duplicate, sizeof(duplicate), &tag, &tag_size) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_INFO);
}

static void test_information_iterator_alignment_and_termination(void)
{
    uint8_t aligned_info[32U] = {0U};
    uint8_t missing_end[16U] = {0U};
    uint8_t nonterminal_end[32U] = {0U};
    struct preboot_multiboot2_info_iterator iterator;
    struct preboot_multiboot2_tag tag;
    uint32_t has_tag = 0U;

    hcd_store_le32(aligned_info, 0U, (uint32_t)sizeof(aligned_info));
    hcd_store_le32(aligned_info, 8U, 42U);
    hcd_store_le32(aligned_info, 12U, 9U);
    hcd_store_le32(aligned_info, 24U, 0U);
    hcd_store_le32(aligned_info, 28U, (uint32_t)TEST_END_TAG_SIZE);
    assert(preboot_multiboot2_info_iterator_init(
               aligned_info, sizeof(aligned_info), &iterator) ==
           PREBOOT_MULTIBOOT2_INFO_OK);
    assert(preboot_multiboot2_info_iterator_next(
               &iterator, &tag, &has_tag) == PREBOOT_MULTIBOOT2_INFO_OK);
    assert(has_tag == 1U);
    assert(tag.type == 42U);
    assert(tag.size == 9U);
    assert(preboot_multiboot2_info_iterator_next(
               &iterator, &tag, &has_tag) == PREBOOT_MULTIBOOT2_INFO_OK);
    assert(has_tag == 0U);
    assert(preboot_multiboot2_info_iterator_next(
               &iterator, &tag, &has_tag) == PREBOOT_MULTIBOOT2_INFO_OK);
    assert(has_tag == 0U);

    hcd_store_le32(missing_end, 0U, (uint32_t)sizeof(missing_end));
    hcd_store_le32(missing_end, 8U, 42U);
    hcd_store_le32(missing_end, 12U, 8U);
    assert(preboot_multiboot2_info_iterator_init(
               missing_end, sizeof(missing_end), &iterator) ==
           PREBOOT_MULTIBOOT2_INFO_OK);
    assert(preboot_multiboot2_info_iterator_next(
               &iterator, &tag, &has_tag) == PREBOOT_MULTIBOOT2_INFO_OK);
    assert(has_tag == 1U);
    assert(preboot_multiboot2_info_iterator_next(
               &iterator, &tag, &has_tag) ==
           PREBOOT_MULTIBOOT2_INFO_E_BAD_INFO);

    hcd_store_le32(nonterminal_end, 0U, (uint32_t)sizeof(nonterminal_end));
    hcd_store_le32(nonterminal_end, 8U, 42U);
    hcd_store_le32(nonterminal_end, 12U, 8U);
    hcd_store_le32(nonterminal_end, 16U, 0U);
    hcd_store_le32(nonterminal_end, 20U, (uint32_t)TEST_END_TAG_SIZE);
    hcd_store_le32(nonterminal_end, 24U, 42U);
    hcd_store_le32(nonterminal_end, 28U, 8U);
    assert(preboot_multiboot2_info_iterator_init(
               nonterminal_end, sizeof(nonterminal_end), &iterator) ==
           PREBOOT_MULTIBOOT2_INFO_OK);
    assert(preboot_multiboot2_info_iterator_next(
               &iterator, &tag, &has_tag) == PREBOOT_MULTIBOOT2_INFO_OK);
    assert(has_tag == 1U);
    assert(preboot_multiboot2_info_iterator_next(
               &iterator, &tag, &has_tag) == PREBOOT_MULTIBOOT2_INFO_E_BAD_TAG);
}

static void test_rejects_overlapping_input_and_output(void)
{
    uint8_t tag[TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE] = {0U};
    uint8_t info[TEST_INFO_HEADER_SIZE + TEST_TAG_HEADER_SIZE +
                 TEST_ENTRY_SIZE + TEST_END_TAG_SIZE] = {0U};
    uint32_t count = UINT32_MAX;
    size_t end_offset = TEST_INFO_HEADER_SIZE + TEST_TAG_HEADER_SIZE +
        TEST_ENTRY_SIZE;

    initialize_tag(tag, sizeof(tag), TEST_ENTRY_SIZE);
    write_entry(tag, TEST_ENTRY_SIZE, 0U, 0x1000U, 0x2000U, 1U);
    assert(preboot_multiboot2_normalize_memory_map(
               tag, sizeof(tag), tag, sizeof(tag), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_OUTPUT_OVERLAP);
    assert(count == 0U);
    assert(hcd_load_le32(tag, 0U) == TEST_TAG_TYPE_MMAP);
    assert(hcd_load_le64(tag, TEST_TAG_HEADER_SIZE) == UINT64_C(0x1000));

    hcd_store_le32(info, 0U, (uint32_t)sizeof(info));
    initialize_tag(
        info + TEST_INFO_HEADER_SIZE,
        TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE, TEST_ENTRY_SIZE);
    write_entry(
        info + TEST_INFO_HEADER_SIZE, TEST_ENTRY_SIZE, 0U,
        0x3000U, 0x1000U, 1U);
    hcd_store_le32(info, end_offset, 0U);
    hcd_store_le32(info, end_offset + 4U, (uint32_t)TEST_END_TAG_SIZE);
    count = UINT32_MAX;
    assert(preboot_multiboot2_normalize_information_memory_map(
               info, sizeof(info), info + TEST_INFO_HEADER_SIZE + 8U,
               40U, &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_OUTPUT_OVERLAP);
    assert(count == 0U);
    assert(hcd_load_le32(info, 0U) == sizeof(info));
    assert(hcd_load_le64(info, TEST_INFO_HEADER_SIZE + TEST_TAG_HEADER_SIZE) ==
           UINT64_C(0x3000));
}

static void test_rejects_tag_outputs_aliasing_information(void)
{
    union {
        const void *tag_output;
        uint8_t bytes[TEST_INFO_HEADER_SIZE + TEST_TAG_HEADER_SIZE +
                      TEST_ENTRY_SIZE + TEST_END_TAG_SIZE];
    } info;
    uint8_t original[sizeof(info.bytes)];
    size_t tag_size = 0U;
    size_t end_offset = TEST_INFO_HEADER_SIZE + TEST_TAG_HEADER_SIZE +
        TEST_ENTRY_SIZE;
    size_t index = 0U;

    for (index = 0U; index < sizeof(info.bytes); ++index) {
        info.bytes[index] = 0U;
    }
    hcd_store_le32(info.bytes, 0U, (uint32_t)sizeof(info.bytes));
    initialize_tag(
        info.bytes + TEST_INFO_HEADER_SIZE,
        TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE, TEST_ENTRY_SIZE);
    write_entry(info.bytes + TEST_INFO_HEADER_SIZE, TEST_ENTRY_SIZE, 0U,
                0x1000U, 0x1000U, 1U);
    hcd_store_le32(info.bytes, end_offset, 0U);
    hcd_store_le32(info.bytes, end_offset + 4U, (uint32_t)TEST_END_TAG_SIZE);
    for (index = 0U; index < sizeof(info.bytes); ++index) {
        original[index] = info.bytes[index];
    }

    assert(preboot_multiboot2_get_memory_map_tag(
               info.bytes, sizeof(info.bytes), &info.tag_output, &tag_size) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_OUTPUT_OVERLAP);
    assert(tag_size == 0U);
    for (index = 0U; index < sizeof(info.bytes); ++index) {
        assert(info.bytes[index] == original[index]);
    }
}

static void test_rejects_tag_output_parameters_overlapping_each_other(void)
{
    union {
        const void *tag;
        size_t tag_size;
    } outputs;
    uint8_t info[TEST_INFO_HEADER_SIZE + TEST_END_TAG_SIZE] = {0U};

    hcd_store_le32(info, 0U, (uint32_t)sizeof(info));
    hcd_store_le32(info, TEST_INFO_HEADER_SIZE, 0U);
    hcd_store_le32(
        info, TEST_INFO_HEADER_SIZE + 4U, (uint32_t)TEST_END_TAG_SIZE);
    outputs.tag_size = SIZE_MAX;

    assert(preboot_multiboot2_get_memory_map_tag(
               info, sizeof(info), &outputs.tag, &outputs.tag_size) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_OUTPUT_OVERLAP);
    assert(outputs.tag_size == SIZE_MAX);
}

static void test_rejects_record_count_aliasing_tag_or_output(void)
{
    union {
        uint32_t record_count;
        uint8_t bytes[TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE];
    } tag;
    union {
        uint32_t record_count;
        uint8_t bytes[40U];
    } output;
    uint8_t original_tag[sizeof(tag.bytes)];
    uint8_t original_output[sizeof(output.bytes)];
    size_t index = 0U;

    for (index = 0U; index < sizeof(tag.bytes); ++index) {
        tag.bytes[index] = 0U;
    }
    initialize_tag(tag.bytes, sizeof(tag.bytes), TEST_ENTRY_SIZE);
    write_entry(tag.bytes, TEST_ENTRY_SIZE, 0U, 0x1000U, 0x1000U, 1U);
    for (index = 0U; index < sizeof(tag.bytes); ++index) {
        original_tag[index] = tag.bytes[index];
    }
    assert(preboot_multiboot2_normalize_memory_map(
               tag.bytes, sizeof(tag.bytes), output.bytes, sizeof(output.bytes),
               &tag.record_count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_OUTPUT_OVERLAP);
    for (index = 0U; index < sizeof(tag.bytes); ++index) {
        assert(tag.bytes[index] == original_tag[index]);
    }

    for (index = 0U; index < sizeof(output.bytes); ++index) {
        output.bytes[index] = UINT8_C(0xA5);
        original_output[index] = UINT8_C(0xA5);
    }
    assert(preboot_multiboot2_normalize_memory_map(
               original_tag, sizeof(original_tag), output.bytes,
               sizeof(output.bytes), &output.record_count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_OUTPUT_OVERLAP);
    for (index = 0U; index < sizeof(output.bytes); ++index) {
        assert(output.bytes[index] == original_output[index]);
    }
}

static void test_rejects_record_count_aliasing_information_block(void)
{
    union {
        uint32_t record_count;
        uint8_t bytes[TEST_INFO_HEADER_SIZE + TEST_TAG_HEADER_SIZE +
                      TEST_ENTRY_SIZE + TEST_END_TAG_SIZE];
    } info;
    uint8_t original[sizeof(info.bytes)];
    uint8_t output[40U] = {0U};
    size_t end_offset = TEST_INFO_HEADER_SIZE + TEST_TAG_HEADER_SIZE +
        TEST_ENTRY_SIZE;
    size_t index = 0U;

    for (index = 0U; index < sizeof(info.bytes); ++index) {
        info.bytes[index] = 0U;
    }
    hcd_store_le32(info.bytes, 0U, (uint32_t)sizeof(info.bytes));
    initialize_tag(
        info.bytes + TEST_INFO_HEADER_SIZE,
        TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE, TEST_ENTRY_SIZE);
    write_entry(info.bytes + TEST_INFO_HEADER_SIZE, TEST_ENTRY_SIZE, 0U,
                0x1000U, 0x1000U, 1U);
    hcd_store_le32(info.bytes, end_offset, 0U);
    hcd_store_le32(info.bytes, end_offset + 4U, (uint32_t)TEST_END_TAG_SIZE);
    for (index = 0U; index < sizeof(info.bytes); ++index) {
        original[index] = info.bytes[index];
    }

    assert(preboot_multiboot2_normalize_information_memory_map(
               info.bytes, sizeof(info.bytes), output, sizeof(output),
               &info.record_count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_OUTPUT_OVERLAP);
    for (index = 0U; index < sizeof(info.bytes); ++index) {
        assert(info.bytes[index] == original[index]);
    }
}

static void test_rejects_host_buffer_range_overflow(void)
{
    uint8_t tag[TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE] = {0U};
    uint8_t info[TEST_INFO_HEADER_SIZE + TEST_TAG_HEADER_SIZE +
                 TEST_ENTRY_SIZE + TEST_END_TAG_SIZE] = {0U};
    uint8_t output[40U];
    uint8_t original_output[sizeof(output)];
    uint32_t count = UINT32_C(0xA5A5A5A5);
    uint32_t original_count = count;
    const void *tag_output = &count;
    size_t tag_size_output = 123U;
    size_t end_offset = TEST_INFO_HEADER_SIZE + TEST_TAG_HEADER_SIZE +
        TEST_ENTRY_SIZE;
    size_t index = 0U;

    initialize_tag(tag, sizeof(tag), TEST_ENTRY_SIZE);
    write_entry(tag, TEST_ENTRY_SIZE, 0U, 0x1000U, 0x1000U, 1U);
    for (index = 0U; index < sizeof(output); ++index) {
        output[index] = UINT8_C(0x5A);
        original_output[index] = UINT8_C(0x5A);
    }

    assert(preboot_multiboot2_normalize_memory_map(
               tag, SIZE_MAX, output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_BUFFER_RANGE);
    assert(count == original_count);
    for (index = 0U; index < sizeof(output); ++index) {
        assert(output[index] == original_output[index]);
    }

    assert(preboot_multiboot2_normalize_memory_map(
               tag, sizeof(tag), output, SIZE_MAX, &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_BUFFER_RANGE);
    assert(count == original_count);
    for (index = 0U; index < sizeof(output); ++index) {
        assert(output[index] == original_output[index]);
    }

    hcd_store_le32(info, 0U, (uint32_t)sizeof(info));
    initialize_tag(
        info + TEST_INFO_HEADER_SIZE,
        TEST_TAG_HEADER_SIZE + TEST_ENTRY_SIZE, TEST_ENTRY_SIZE);
    write_entry(info + TEST_INFO_HEADER_SIZE, TEST_ENTRY_SIZE, 0U,
                0x2000U, 0x1000U, 1U);
    hcd_store_le32(info, end_offset, 0U);
    hcd_store_le32(info, end_offset + 4U, (uint32_t)TEST_END_TAG_SIZE);

    assert(preboot_multiboot2_get_memory_map_tag(
               info, SIZE_MAX, &tag_output, &tag_size_output) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_BUFFER_RANGE);
    assert(tag_output == &count);
    assert(tag_size_output == 123U);

    assert(preboot_multiboot2_normalize_information_memory_map(
               info, SIZE_MAX, output, sizeof(output), &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_BUFFER_RANGE);
    assert(count == original_count);
    for (index = 0U; index < sizeof(output); ++index) {
        assert(output[index] == original_output[index]);
    }

    assert(preboot_multiboot2_normalize_information_memory_map(
               info, sizeof(info), output, SIZE_MAX, &count) ==
           PREBOOT_MULTIBOOT2_MEMORY_MAP_E_BAD_BUFFER_RANGE);
    assert(count == original_count);
    for (index = 0U; index < sizeof(output); ++index) {
        assert(output[index] == original_output[index]);
    }
}

int main(void)
{
    test_converts_and_coalesces_sorted_entries();
    test_accepts_extended_entry_stride();
    test_rejects_bad_tag_and_metadata();
    test_rejects_invalid_entries();
    test_rejects_reserved_and_capacity_errors();
    test_extracts_tag_from_complete_information_block();
    test_rejects_missing_duplicate_and_malformed_info_maps();
    test_information_iterator_alignment_and_termination();
    test_rejects_overlapping_input_and_output();
    test_rejects_tag_outputs_aliasing_information();
    test_rejects_tag_output_parameters_overlapping_each_other();
    test_rejects_record_count_aliasing_tag_or_output();
    test_rejects_record_count_aliasing_information_block();
    test_rejects_host_buffer_range_overflow();
    return 0;
}
