#include <stdint.h>

#include "hcd_wire.h"
#include "hcd_parser.h"

#include "hcd_assert.h"
#include "hcd_crc64.h"
#include "hcd_endian.h"
#include "hcd_semantics.h"
#include "hcd_section_catalog.h"

static enum hcd_error hcd_fail(
    struct hcd_validation_error *detail,
    enum hcd_error code,
    uint32_t section_type,
    uint32_t record_index,
    uint64_t offset)
{
    if (detail != NULL) {
        detail->code = (uint32_t)code;
        detail->section_type = section_type;
        detail->record_index = record_index;
        detail->reserved0 = 0U;
        detail->offset = offset;
    }
    HCD_ASSERT(code != HCD_OK);
    HCD_ASSERT((uint32_t)code <= (uint32_t)HCD_E_PROFILE_MISMATCH);
    return code;
}
static enum hcd_error hcd_read_header(
    const uint8_t *buffer,
    struct hcd_header *header)
{
    if ((buffer == NULL) || (header == NULL)) {
        return HCD_E_NULL_INPUT;
    }
    HCD_ASSERT(buffer != NULL);
    HCD_ASSERT(header != NULL);

    header->magic = hcd_load_le32(buffer, 0U);
    header->version_major = hcd_load_le16(buffer, 4U);
    header->version_minor = hcd_load_le16(buffer, 6U);
    header->header_size = hcd_load_le32(buffer, 8U);
    header->architecture = hcd_load_le32(buffer, 12U);
    header->total_size = hcd_load_le64(buffer, 16U);
    header->checksum = hcd_load_le64(buffer, 24U);
    header->flags = hcd_load_le32(buffer, 32U);
    header->section_count = hcd_load_le32(buffer, 36U);
    header->section_table_offset = hcd_load_le64(buffer, 40U);
    header->producer_id = hcd_load_le32(buffer, 48U);
    header->producer_version = hcd_load_le32(buffer, 52U);
    header->reserved0 = hcd_load_le64(buffer, 56U);
    return HCD_OK;
}


static enum hcd_error hcd_check_directory_header(
    const struct hcd_header *header,
    struct hcd_validation_error *detail)
{
    uint64_t directory_size = 0U;

    if ((header->section_count == 0U) ||
        (header->section_count > HCD_MAX_SECTIONS)) {
        return hcd_fail(detail, HCD_E_COUNT_LIMIT, 0U, HCD_ID_UNKNOWN, 36U);
    }
    if ((header->section_table_offset < HCD_HEADER_SIZE) ||
        ((header->section_table_offset & UINT64_C(7)) != 0U)) {
        return hcd_fail(detail, HCD_E_ALIGNMENT, 0U, HCD_ID_UNKNOWN, 40U);
    }
    directory_size = (uint64_t)header->section_count *
                     (uint64_t)HCD_SECTION_ENTRY_SIZE;
    if ((header->section_table_offset > header->total_size) ||
        (directory_size >
         (header->total_size - header->section_table_offset))) {
        return hcd_fail(detail, HCD_E_BAD_SECTION_DIRECTORY, 0U,
                        HCD_ID_UNKNOWN, header->section_table_offset);
    }
    HCD_ASSERT(header->section_count <= HCD_MAX_SECTIONS);
    HCD_ASSERT(directory_size <= HCD_MAX_TOTAL_SIZE);
    return HCD_OK;
}

static enum hcd_error hcd_check_header_checksum(
    const uint8_t *buffer,
    uint64_t available_size,
    const struct hcd_header *header,
    struct hcd_validation_error *detail)
{
    uint64_t checksum = 0U;
    enum hcd_error result = hcd_crc64_hcd(buffer, available_size, &checksum);

    if (result != HCD_OK) {
        return hcd_fail(detail, result, 0U, HCD_ID_UNKNOWN, 24U);
    }
    if (checksum != header->checksum) {
        return hcd_fail(detail, HCD_E_BAD_CHECKSUM, 0U, HCD_ID_UNKNOWN, 24U);
    }
    HCD_ASSERT(header->total_size == available_size);
    HCD_ASSERT(available_size <= HCD_MAX_TOTAL_SIZE);
    return HCD_OK;
}

static enum hcd_error hcd_check_header(
    const uint8_t *buffer,
    uint64_t available_size,
    struct hcd_header *header,
    struct hcd_validation_error *detail)
{
    enum hcd_error result = HCD_OK;
    if ((buffer == NULL) || (header == NULL)) {
        return hcd_fail(detail, HCD_E_NULL_INPUT, 0U, HCD_ID_UNKNOWN, 0U);
    }
    if ((available_size < HCD_HEADER_SIZE) ||
        (available_size > HCD_MAX_TOTAL_SIZE)) {
        return hcd_fail(detail, HCD_E_BAD_TOTAL_SIZE, 0U, HCD_ID_UNKNOWN, 0U);
    }
    result = hcd_read_header(buffer, header);
    if (result != HCD_OK) {
        return hcd_fail(detail, result, 0U, HCD_ID_UNKNOWN, 0U);
    }
    if (header->magic != HCD_MAGIC) {
        return hcd_fail(detail, HCD_E_BAD_MAGIC, 0U, HCD_ID_UNKNOWN, 0U);
    }
    if ((header->version_major != HCD_VERSION_MAJOR_V1) &&
        (header->version_major != HCD_VERSION_MAJOR_V2)) {
        return hcd_fail(detail, HCD_E_UNSUPPORTED_MAJOR, 0U, HCD_ID_UNKNOWN, 4U);
    }
    if (header->version_minor > HCD_VERSION_MINOR) {
        return hcd_fail(detail, HCD_E_UNSUPPORTED_MINOR_REQUIRED, 0U,
                        HCD_ID_UNKNOWN, 6U);
    }
    if (header->header_size != HCD_HEADER_SIZE) {
        return hcd_fail(detail, HCD_E_BAD_HEADER_SIZE, 0U, HCD_ID_UNKNOWN, 8U);
    }
    if ((header->architecture != HCD_ARCH_AMD64) &&
        (header->architecture != HCD_ARCH_ARM64)) {
        return hcd_fail(detail, HCD_E_PROFILE_MISMATCH, 0U, HCD_ID_UNKNOWN, 12U);
    }
    if (header->total_size != available_size) {
        return hcd_fail(detail, HCD_E_BAD_TOTAL_SIZE, 0U, HCD_ID_UNKNOWN, 16U);
    }
    if (((header->flags & ~HCD_F_KNOWN_MASK) != 0U) ||
        (header->reserved0 != 0U)) {
        return hcd_fail(detail, HCD_E_RESERVED_NONZERO, 0U, HCD_ID_UNKNOWN, 32U);
    }
    if ((header->flags & HCD_F_FIRMWARE_BOOT_SERVICES_ACTIVE) != 0U &&
        (header->flags & HCD_F_EFI_EXIT_BOOT_SERVICES_DONE) != 0U) {
        return hcd_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY, 0U,
                        HCD_ID_UNKNOWN, 32U);
    }
    result = hcd_check_directory_header(header, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_check_header_checksum(buffer, available_size, header, detail);
    if (result != HCD_OK) {
        return result;
    }
    HCD_ASSERT(header->total_size <= HCD_MAX_TOTAL_SIZE);
    HCD_ASSERT(header->section_count <= HCD_MAX_SECTIONS);
    return HCD_OK;
}

static int hcd_is_power_of_two(uint32_t value)
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

static void hcd_decode_section(
    const uint8_t *buffer,
    uint64_t entry_offset,
    struct hcd_section_view *section)
{
    HCD_ASSERT(buffer != NULL);
    HCD_ASSERT(section != NULL);
    section->type = hcd_load_le32(buffer, entry_offset);
    section->version = hcd_load_le16(buffer, entry_offset + 4U);
    section->flags = hcd_load_le16(buffer, entry_offset + 6U);
    section->offset = hcd_load_le64(buffer, entry_offset + 8U);
    section->size = hcd_load_le64(buffer, entry_offset + 16U);
    section->entry_size = hcd_load_le32(buffer, entry_offset + 24U);
    section->entry_count = hcd_load_le32(buffer, entry_offset + 28U);
    section->alignment = hcd_load_le32(buffer, entry_offset + 32U);
    section->reserved0 = hcd_load_le32(buffer, entry_offset + 36U);
}

static enum hcd_error hcd_check_section_range(
    const struct hcd_header *header,
    const struct hcd_section_view *section,
    uint64_t directory_end,
    uint32_t index,
    uint64_t entry_offset,
    struct hcd_validation_error *detail)
{
    if (((((uint32_t)section->flags) & ~((uint32_t)HCD_SF_KNOWN_MASK)) != 0U) ||
        (section->reserved0 != 0U)) {
        return hcd_fail(detail, HCD_E_RESERVED_NONZERO, section->type,
                        index, entry_offset + 6U);
    }
    if ((section->alignment < 8U) ||
        !hcd_is_power_of_two(section->alignment) ||
        ((section->offset & (uint64_t)(section->alignment - 1U)) != 0U)) {
        return hcd_fail(detail, HCD_E_ALIGNMENT, section->type,
                        index, entry_offset + 32U);
    }
    if ((section->offset < header->header_size) ||
        (section->offset > header->total_size) ||
        (section->size > (header->total_size - section->offset))) {
        return hcd_fail(detail, HCD_E_SECTION_BOUNDS, section->type,
                        index, section->offset);
    }
    if ((section->offset < directory_end) && (section->size != 0U) &&
        (section->offset + section->size > header->section_table_offset)) {
        return hcd_fail(detail, HCD_E_SECTION_OVERLAP, section->type,
                        index, section->offset);
    }
    HCD_ASSERT(section->offset >= header->header_size);
    HCD_ASSERT(section->size <= header->total_size);
    return HCD_OK;
}

static enum hcd_error hcd_check_unknown_section(
    const struct hcd_section_view *section,
    uint32_t index,
    uint64_t entry_offset,
    struct hcd_validation_error *detail)
{
    uint64_t expected_bytes = 0U;

    if ((((uint32_t)section->flags) & ((uint32_t)HCD_SF_REQUIRED)) != 0U) {
        return hcd_fail(detail, HCD_E_SECTION_UNKNOWN_REQUIRED,
                        section->type, index, entry_offset);
    }
    if ((((uint32_t)section->flags) & ((uint32_t)HCD_SF_ARRAY)) != 0U) {
        if ((section->entry_size == 0U) ||
            ((uint64_t)section->entry_count >
             (HCD_MAX_TOTAL_SIZE / (uint64_t)section->entry_size))) {
            return hcd_fail(detail, HCD_E_COUNT_LIMIT, section->type,
                            index, entry_offset + 24U);
        }
        expected_bytes = (uint64_t)section->entry_count *
                         (uint64_t)section->entry_size;
        if (expected_bytes != section->size) {
            return hcd_fail(detail, HCD_E_SECTION_BOUNDS, section->type,
                            index, entry_offset + 16U);
        }
    } else if ((section->entry_count != 1U) ||
               (section->entry_size == 0U) ||
               (section->entry_size != section->size)) {
        return hcd_fail(detail, HCD_E_SECTION_BOUNDS, section->type,
                        index, entry_offset + 16U);
    }
    HCD_ASSERT((((uint32_t)section->flags) & ((uint32_t)HCD_SF_REQUIRED)) == 0U);
    HCD_ASSERT(section->alignment >= 8U);
    return HCD_OK;
}


static enum hcd_error hcd_check_array_layout(
    const struct hcd_section_view *section,
    uint32_t index,
    uint64_t entry_offset,
    struct hcd_validation_error *detail)
{
    uint64_t expected_bytes = 0U;

    if ((uint64_t)section->entry_count >
        (HCD_MAX_TOTAL_SIZE / (uint64_t)section->entry_size)) {
        return hcd_fail(detail, HCD_E_COUNT_LIMIT, section->type,
                        index, entry_offset + 28U);
    }
    if (((section->type == HCD_SECTION_CPU_RECORDS) ||
         (section->type == HCD_SECTION_MMU_CAPABILITIES) ||
         (section->type == HCD_SECTION_VIRTUALIZATION)) &&
        (section->entry_count > HCD_MAX_CPUS)) {
        return hcd_fail(detail, HCD_E_COUNT_LIMIT, section->type,
                        index, entry_offset + 28U);
    }
    if ((section->type == HCD_SECTION_CPU_FEATURE_CATALOG) &&
        (section->entry_count > HCD_FEATURE_BITS)) {
        return hcd_fail(detail, HCD_E_COUNT_LIMIT, section->type,
                        index, entry_offset + 28U);
    }
    if ((section->type == HCD_SECTION_INTERRUPT_CONTROLLERS) &&
        (section->entry_count > HCD_MAX_INTERRUPT_CONTROLLERS)) {
        return hcd_fail(detail, HCD_E_COUNT_LIMIT, section->type,
                        index, entry_offset + 28U);
    }
    if ((section->type == HCD_SECTION_MMU_TRANSLATION_CONFIGS) &&
        (section->entry_count > HCD_MAX_MMU_CONFIGURATIONS)) {
        return hcd_fail(detail, HCD_E_COUNT_LIMIT, section->type,
                        index, entry_offset + 28U);
    }
    if ((section->type == HCD_SECTION_MMU_MAPPING_SIZES) &&
        (section->entry_count > HCD_MAX_MMU_MAPPING_SIZES)) {
        return hcd_fail(detail, HCD_E_COUNT_LIMIT, section->type,
                        index, entry_offset + 28U);
    }
    if ((section->type == HCD_SECTION_MMU_ARM64_CONTEXTS) &&
        (section->entry_count > HCD_MAX_MMU_CONFIGURATIONS)) {
        return hcd_fail(detail, HCD_E_COUNT_LIMIT, section->type,
                        index, entry_offset + 28U);
    }
    expected_bytes = (uint64_t)section->entry_count *
                     (uint64_t)section->entry_size;
    if (expected_bytes != section->size) {
        return hcd_fail(detail, HCD_E_SECTION_BOUNDS, section->type,
                        index, entry_offset + 16U);
    }
    HCD_ASSERT(section->entry_size > 0U);
    HCD_ASSERT(expected_bytes <= HCD_MAX_TOTAL_SIZE);
    return HCD_OK;
}

static enum hcd_error hcd_check_known_section(
    const struct hcd_section_view *section,
    uint32_t expected_size,
    int array_section,
    uint32_t index,
    uint64_t entry_offset,
    struct hcd_validation_error *detail)
{
    if (section->version != 1U) {
        return hcd_fail(detail, HCD_E_UNSUPPORTED_MINOR_REQUIRED,
                        section->type, index, entry_offset + 4U);
    }
    if (((((uint32_t)section->flags) & ((uint32_t)HCD_SF_PER_CPU)) != 0U) !=
        (hcd_section_is_per_cpu(section->type) != 0)) {
        return hcd_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                        section->type, index, entry_offset + 6U);
    }
    if (((((uint32_t)section->flags) & ((uint32_t)HCD_SF_EXTERNAL)) != 0U) !=
        (hcd_section_is_external(section->type) != 0)) {
        return hcd_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                        section->type, index, entry_offset + 6U);
    }
    if (((((uint32_t)section->flags) & ((uint32_t)HCD_SF_ARRAY)) != 0U) != (array_section != 0)) {
        return hcd_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                        section->type, index, entry_offset + 6U);
    }
    if (section->entry_size != expected_size) {
        return hcd_fail(detail, HCD_E_SECTION_BOUNDS, section->type,
                        index, entry_offset + 24U);
    }
    if ((section->type == HCD_SECTION_MEMORY_MAP) &&
        (section->entry_count > HCD_MAX_MEMORY_REGIONS)) {
        return hcd_fail(detail, HCD_E_COUNT_LIMIT, section->type,
                        index, entry_offset + 28U);
    }
    if (array_section != 0) {
        return hcd_check_array_layout(section, index, entry_offset, detail);
    }
    if ((section->entry_count != 1U) ||
        (section->size != section->entry_size)) {
        return hcd_fail(detail, HCD_E_SECTION_BOUNDS, section->type,
                        index, entry_offset + 16U);
    }
    HCD_ASSERT(section->entry_size == expected_size);
    HCD_ASSERT(section->version == 1U);
    return HCD_OK;
}

static enum hcd_error hcd_read_section(
    const uint8_t *buffer,
    const struct hcd_header *header,
    uint32_t index,
    struct hcd_section_view *section,
    struct hcd_validation_error *detail)
{
    uint64_t entry_offset = header->section_table_offset +
        ((uint64_t)index * (uint64_t)HCD_SECTION_ENTRY_SIZE);
    uint64_t directory_end = header->section_table_offset +
        ((uint64_t)header->section_count * (uint64_t)HCD_SECTION_ENTRY_SIZE);
    uint32_t expected_size = 0U;
    int array_section = 0;
    enum hcd_error result = HCD_OK;

    hcd_decode_section(buffer, entry_offset, section);
    result = hcd_check_section_range(header, section, directory_end,
                                     index, entry_offset, detail);
    if (result != HCD_OK) {
        return result;
    }
    expected_size = hcd_section_expected_entry_size_for_version(
        section->type, header->version_major);
    array_section = hcd_section_is_array(section->type);
    if (expected_size == 0U) {
        return hcd_check_unknown_section(section, index, entry_offset, detail);
    }
    return hcd_check_known_section(section, expected_size, array_section,
                                   index, entry_offset, detail);
}

static enum hcd_error hcd_check_section_overlaps(
    const struct hcd_header *header,
    const struct hcd_validation_workspace *workspace,
    struct hcd_validation_error *detail)
{
    uint32_t left = 0U;
    uint32_t right = 0U;
    uint64_t directory_end = header->section_table_offset +
        ((uint64_t)header->section_count * (uint64_t)HCD_SECTION_ENTRY_SIZE);

    for (left = 0U; left < workspace->section_count; ++left) {
        const struct hcd_section_view *a = &workspace->sections[left];

        uint64_t a_end = a->offset + a->size;

        if (a->size == 0U) {
            continue;
        }
        for (right = left + 1U; right < workspace->section_count; ++right) {
            const struct hcd_section_view *b = &workspace->sections[right];
            uint64_t b_end = b->offset + b->size;

            if ((b->size != 0U) && (a->offset < b_end) &&
                (b->offset < a_end)) {
                return hcd_fail(detail, HCD_E_SECTION_OVERLAP,
                                b->type, right, b->offset);
            }
        }
        HCD_ASSERT(a->offset >= header->header_size);
        HCD_ASSERT(a_end <= header->total_size);
        HCD_ASSERT(a_end <= directory_end || a->offset >= directory_end);
    }
    return HCD_OK;
}

static enum hcd_error hcd_check_profile(
    const struct hcd_header *header,
    const struct hcd_validation_workspace *workspace,
    enum hcd_validation_profile profile,
    struct hcd_validation_error *detail)
{
    uint32_t required_mask = 0U;
    uint32_t index = 0U;

    if (profile == HCD_PROFILE_CORE) {
        required_mask = UINT32_C(0x33DE);
        if ((header->flags & HCD_F_INCOMPLETE) != 0U) {
            return hcd_fail(detail, HCD_E_INCOMPLETE_NOT_ALLOWED,
                            0U, HCD_ID_UNKNOWN, 32U);
        }
    } else if (profile == HCD_PROFILE_RECOVERY) {
        required_mask = (UINT32_C(1) << (uint32_t)HCD_SECTION_CPU_SUMMARY) |
                        (UINT32_C(1) << (uint32_t)HCD_SECTION_CPU_RECORDS) |
                        (UINT32_C(1) << (uint32_t)HCD_SECTION_CPU_FEATURE_CATALOG) |
                        (UINT32_C(1) << (uint32_t)HCD_SECTION_MEMORY_MAP) |
                        (UINT32_C(1) << (uint32_t)HCD_SECTION_BOOT_CONTEXT);
        if ((header->flags & HCD_F_INCOMPLETE) == 0U) {
            return hcd_fail(detail, HCD_E_PROFILE_MISMATCH,
                            0U, HCD_ID_UNKNOWN, 32U);
        }
    } else {
        return hcd_fail(detail, HCD_E_PROFILE_MISMATCH,
                        0U, HCD_ID_UNKNOWN, 0U);
    }

    if (header->version_major == HCD_VERSION_MAJOR_V2) {
        required_mask |=
            (UINT32_C(1) << (uint32_t)HCD_SECTION_MMU_CAPABILITIES) |
            (UINT32_C(1) << (uint32_t)HCD_SECTION_MMU_TRANSLATION_CONFIGS) |
            (UINT32_C(1) << (uint32_t)HCD_SECTION_MMU_MAPPING_SIZES);
        if (header->architecture == HCD_ARCH_ARM64) {
            required_mask |=
                UINT32_C(1) << (uint32_t)HCD_SECTION_MMU_ARM64_CONTEXTS;
        }
    }
    if ((workspace->seen_mask & required_mask) != required_mask) {
        return hcd_fail(detail, HCD_E_BAD_SECTION_DIRECTORY,
                        0U, HCD_ID_UNKNOWN, header->section_table_offset);
    }
    for (index = 0U; index < workspace->section_count; ++index) {
        const struct hcd_section_view *section = &workspace->sections[index];
        uint32_t bit = 0U;

        if ((section->type == 0U) ||
            (section->type > ((header->version_major == HCD_VERSION_MAJOR_V2)
                                  ? HCD_SECTION_MMU_ARM64_CONTEXTS
                                  : HCD_SECTION_CPU_RAW_REGISTERS))) {
            continue;
        }
        bit = UINT32_C(1) << section->type;
        if (((required_mask & bit) != 0U) &&
            ((((uint32_t)section->flags) & ((uint32_t)HCD_SF_REQUIRED)) == 0U)) {
            return hcd_fail(detail, HCD_E_PROFILE_MISMATCH,
                            section->type, index, section->offset);
        }
        if (((required_mask & bit) != 0U) &&
            (section->entry_count == 0U)) {
            return hcd_fail(detail, HCD_E_COUNT_LIMIT,
                            section->type, index, section->offset);
        }
    }
    HCD_ASSERT(workspace->section_count <= HCD_MAX_SECTIONS);
    HCD_ASSERT((workspace->seen_mask & required_mask) == required_mask);
    return HCD_OK;
}



static void hcd_clear_header(struct hcd_header *header)
{
    header->magic = 0U;
    header->version_major = 0U;
    header->version_minor = 0U;
    header->header_size = 0U;
    header->architecture = 0U;
    header->total_size = 0U;
    header->checksum = 0U;
    header->flags = 0U;
    header->section_count = 0U;
    header->section_table_offset = 0U;
    header->producer_id = 0U;
    header->producer_version = 0U;
    header->reserved0 = 0U;
}

static void hcd_copy_header(
    struct hcd_header *destination,
    const struct hcd_header *source)
{
    destination->magic = source->magic;
    destination->version_major = source->version_major;
    destination->version_minor = source->version_minor;
    destination->header_size = source->header_size;
    destination->architecture = source->architecture;
    destination->total_size = source->total_size;
    destination->checksum = source->checksum;
    destination->flags = source->flags;
    destination->section_count = source->section_count;
    destination->section_table_offset = source->section_table_offset;
    destination->producer_id = source->producer_id;
    destination->producer_version = source->producer_version;
    destination->reserved0 = source->reserved0;
}

static void hcd_clear_section_view(struct hcd_section_view *section)
{
    section->type = 0U;
    section->version = 0U;
    section->flags = 0U;
    section->offset = 0U;
    section->size = 0U;
    section->entry_size = 0U;
    section->entry_count = 0U;
    section->alignment = 0U;
    section->reserved0 = 0U;
}

static void hcd_reset_validation_state(
    struct hcd_validation_workspace *workspace,
    struct hcd_validation_error *detail,
    struct hcd_validated_view *view)
{
    uint32_t index = 0U;

    view->buffer = NULL;
    view->size = 0U;
    hcd_clear_header(&view->header);
    view->workspace = NULL;
    if (detail != NULL) {
        detail->code = (uint32_t)HCD_OK;
        detail->section_type = 0U;
        detail->record_index = HCD_ID_UNKNOWN;
        detail->reserved0 = 0U;
        detail->offset = 0U;
    }
    workspace->section_count = 0U;
    workspace->seen_mask = 0U;
    for (index = 0U; index < HCD_MAX_SECTIONS; ++index) {
        hcd_clear_section_view(&workspace->sections[index]);
    }
    for (index = 0U; index < (HCD_MAX_CPUS / 64U); ++index) {
        workspace->cpu_seen[index] = 0U;
    }
}

static enum hcd_error hcd_parse_directory(
    const uint8_t *buffer,
    const struct hcd_header *header,
    struct hcd_validation_workspace *workspace,
    struct hcd_validation_error *detail)
{
    uint32_t index = 0U;
    enum hcd_error result = HCD_OK;

    workspace->section_count = header->section_count;
    for (index = 0U; index < header->section_count; ++index) {
        struct hcd_section_view *section = &workspace->sections[index];

        result = hcd_read_section(buffer, header, index, section, detail);
        if (result != HCD_OK) {
            return result;
        }
        if ((section->type > 0U) &&
            (section->type <= ((header->version_major == HCD_VERSION_MAJOR_V2)
                                   ? HCD_SECTION_MMU_ARM64_CONTEXTS
                                   : HCD_SECTION_CPU_RAW_REGISTERS))) {
            uint32_t bit = UINT32_C(1) << section->type;

            if ((workspace->seen_mask & bit) != 0U) {
                return hcd_fail(detail, HCD_E_SECTION_DUPLICATE,
                                section->type, index, section->offset);
            }
            workspace->seen_mask |= bit;
        }
    }
    HCD_ASSERT(workspace->section_count == header->section_count);
    HCD_ASSERT(workspace->section_count <= HCD_MAX_SECTIONS);
    return HCD_OK;
}

static enum hcd_error hcd_check_external_flag(
    const struct hcd_header *header,
    const struct hcd_validation_workspace *workspace,
    struct hcd_validation_error *detail)
{
    uint32_t index = 0U;
    uint32_t external_seen = 0U;

    for (index = 0U; index < workspace->section_count; ++index) {
        if ((((uint32_t)workspace->sections[index].flags) & ((uint32_t)HCD_SF_EXTERNAL)) != 0U) {
            external_seen = 1U;
        }
    }
    if ((external_seen != 0U) !=
        ((header->flags & HCD_F_EXTERNAL_REFERENCES) != 0U)) {
        return hcd_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                        0U, HCD_ID_UNKNOWN, 32U);
    }
    HCD_ASSERT(external_seen <= 1U);
    HCD_ASSERT(workspace->section_count <= HCD_MAX_SECTIONS);
    return HCD_OK;
}

enum hcd_error hcd_validate(
    const uint8_t *buffer,
    uint64_t available_size,
    enum hcd_validation_profile profile,
    struct hcd_validation_workspace *workspace,
    struct hcd_validation_error *detail,
    struct hcd_validated_view *view)
{
    struct hcd_header header;
    enum hcd_error result = HCD_OK;

    if ((buffer == NULL) || (workspace == NULL) || (view == NULL)) {
        return hcd_fail(detail, HCD_E_NULL_INPUT, 0U, HCD_ID_UNKNOWN, 0U);
    }
    HCD_ASSERT(buffer != NULL);
    HCD_ASSERT(workspace != NULL);
    hcd_reset_validation_state(workspace, detail, view);
    result = hcd_check_header(buffer, available_size, &header, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_parse_directory(buffer, &header, workspace, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_check_section_overlaps(&header, workspace, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_check_external_flag(&header, workspace, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_check_profile(&header, workspace, profile, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_validate_semantics(buffer, &header, workspace, profile, detail);
    if (result != HCD_OK) {
        return result;
    }
    view->buffer = buffer;
    view->size = available_size;
    hcd_copy_header(&view->header, &header);
    view->workspace = workspace;
    HCD_ASSERT(view->buffer == buffer);
    HCD_ASSERT(view->size == header.total_size);
    return HCD_OK;
}

enum hcd_error hcd_find_section(
    const struct hcd_validated_view *view,
    uint32_t section_type,
    struct hcd_section_view *section)
{
    uint32_t index = 0U;

    if ((view == NULL) || (section == NULL) ||
        (view->buffer == NULL) || (view->workspace == NULL)) {
        return HCD_E_NULL_INPUT;
    }
    HCD_ASSERT(view->workspace->section_count <= HCD_MAX_SECTIONS);
    HCD_ASSERT(view->size <= HCD_MAX_TOTAL_SIZE);
    for (index = 0U; index < view->workspace->section_count; ++index) {
        if (view->workspace->sections[index].type == section_type) {
            const struct hcd_section_view *source =
                &view->workspace->sections[index];
            section->type = source->type;
            section->version = source->version;
            section->flags = source->flags;
            section->offset = source->offset;
            section->size = source->size;
            section->entry_size = source->entry_size;
            section->entry_count = source->entry_count;
            section->alignment = source->alignment;
            section->reserved0 = source->reserved0;
            return HCD_OK;
        }
    }
    return HCD_E_SECTION_NOT_FOUND;
}
