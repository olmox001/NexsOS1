#include "hcd_builder.h"
#include "hcd_parser.h"

#include <stdint.h>

#include "hcd_assert.h"
#include "hcd_crc64.h"
#include "hcd_endian.h"
#include "hcd_wire.h"
#include "hcd_section_catalog.h"

static enum hcd_error hcd_builder_fail(
    struct hcd_validation_error *detail,
    enum hcd_error code,
    uint32_t type,
    uint64_t offset)
{
    if (detail != NULL) {
        detail->code = (uint32_t)code;
        detail->section_type = type;
        detail->record_index = HCD_ID_UNKNOWN;
        detail->reserved0 = 0U;
        detail->offset = offset;
    }
    HCD_ASSERT(code != HCD_OK);
    HCD_ASSERT((uint32_t)code <= (uint32_t)HCD_E_SECTION_NOT_FOUND);
    return code;
}


static void hcd_builder_section_reset(struct hcd_builder_section *section)
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

static void hcd_builder_reset(struct hcd_builder *builder)
{
    uint32_t index = 0U;

    builder->buffer = NULL;
    builder->capacity = 0U;
    builder->cursor = 0U;
    builder->architecture = 0U;
    builder->version_major = 0U;
    builder->reserved0 = 0U;
    builder->flags = 0U;
    builder->producer_id = 0U;
    builder->producer_version = 0U;
    builder->section_count = 0U;
    builder->seen_mask = 0U;
    builder->finalized = 0U;
    for (index = 0U; index < HCD_MAX_SECTIONS; ++index) {
        hcd_builder_section_reset(&builder->sections[index]);
    }
    HCD_ASSERT(index == HCD_MAX_SECTIONS);
}

static int hcd_builder_power_of_two(uint32_t value)
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

static void hcd_builder_zero(uint8_t *buffer, uint64_t offset,
                             uint64_t length)
{
    uint64_t index = 0U;

    HCD_ASSERT(buffer != NULL);
    for (index = 0U; index < length; ++index) {
        buffer[offset + index] = 0U;
    }
    HCD_ASSERT(index == length);
    HCD_ASSERT(length <= HCD_MAX_TOTAL_SIZE);
}

static enum hcd_error hcd_builder_init_version(
    struct hcd_builder *builder,
    uint8_t *buffer,
    uint64_t capacity,
    uint32_t architecture,
    uint32_t flags,
    uint32_t producer_id,
    uint32_t producer_version,
    uint16_t version_major,
    struct hcd_validation_error *detail)
{
    if ((builder == NULL) || (buffer == NULL)) {
        return hcd_builder_fail(detail, HCD_E_NULL_INPUT, 0U, 0U);
    }
    if ((capacity < HCD_BUILDER_PAYLOAD_OFFSET) ||
        (capacity > HCD_MAX_TOTAL_SIZE)) {
        return hcd_builder_fail(detail, HCD_E_BAD_TOTAL_SIZE, 0U, capacity);
    }
    if ((architecture != HCD_ARCH_AMD64) &&
        (architecture != HCD_ARCH_ARM64)) {
        return hcd_builder_fail(detail, HCD_E_PROFILE_MISMATCH, 0U, 12U);
    }
    if ((version_major != HCD_VERSION_MAJOR_V1) &&
        (version_major != HCD_VERSION_MAJOR_V2)) {
        return hcd_builder_fail(detail, HCD_E_UNSUPPORTED_MAJOR, 0U, 4U);
    }
    if (((flags & ~HCD_F_KNOWN_MASK) != 0U) ||
        ((flags & HCD_F_EXTERNAL_REFERENCES) != 0U) ||
        (((flags & HCD_F_FIRMWARE_BOOT_SERVICES_ACTIVE) != 0U) &&
         ((flags & HCD_F_EFI_EXIT_BOOT_SERVICES_DONE) != 0U))) {
        return hcd_builder_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                0U, 32U);
    }
    hcd_builder_reset(builder);
    builder->buffer = buffer;
    builder->capacity = capacity;
    builder->cursor = HCD_BUILDER_PAYLOAD_OFFSET;
    builder->architecture = architecture;
    builder->version_major = version_major;
    builder->flags = flags;
    builder->producer_id = producer_id;
    builder->producer_version = producer_version;
    hcd_builder_zero(buffer, 0U, HCD_BUILDER_PAYLOAD_OFFSET);
    HCD_ASSERT(builder->cursor <= capacity);
    HCD_ASSERT(builder->section_count == 0U);
    return HCD_OK;
}

enum hcd_error hcd_builder_init(
    struct hcd_builder *builder,
    uint8_t *buffer,
    uint64_t capacity,
    uint32_t architecture,
    uint32_t flags,
    uint32_t producer_id,
    uint32_t producer_version,
    struct hcd_validation_error *detail)
{
    return hcd_builder_init_version(builder, buffer, capacity, architecture,
                                    flags, producer_id, producer_version,
                                    HCD_VERSION_MAJOR_V1, detail);
}

enum hcd_error hcd_builder_init_v2(
    struct hcd_builder *builder,
    uint8_t *buffer,
    uint64_t capacity,
    uint32_t architecture,
    uint32_t flags,
    uint32_t producer_id,
    uint32_t producer_version,
    struct hcd_validation_error *detail)
{
    return hcd_builder_init_version(builder, buffer, capacity, architecture,
                                    flags, producer_id, producer_version,
                                    HCD_VERSION_MAJOR_V2, detail);
}


static enum hcd_error hcd_builder_check_classification(
    const struct hcd_builder *builder,
    uint32_t type,
    uint16_t version,
    uint16_t flags,
    uint32_t entry_size,
    struct hcd_validation_error *detail)
{
    uint32_t known_entry_size =
        hcd_section_expected_entry_size_for_version(type,
                                                    builder->version_major);
    uint32_t section_flags = (uint32_t)flags;

    if ((type == 0U) || (known_entry_size == 0U) ||
        (builder->section_count >= HCD_MAX_SECTIONS)) {
        return hcd_builder_fail(detail, HCD_E_COUNT_LIMIT, type, 0U);
    }
    if ((builder->seen_mask & (UINT32_C(1) << type)) != 0U) {
        return hcd_builder_fail(detail, HCD_E_SECTION_DUPLICATE, type, 0U);
    }
    if ((version != 1U) ||
        ((section_flags & ~((uint32_t)HCD_SF_KNOWN_MASK)) != 0U)) {
        return hcd_builder_fail(detail, HCD_E_RESERVED_NONZERO, type, 0U);
    }
    if (entry_size != known_entry_size) {
        return hcd_builder_fail(detail, HCD_E_SECTION_BOUNDS, type, 0U);
    }
    if ((((section_flags & (uint32_t)HCD_SF_ARRAY) != 0U) !=
         (hcd_section_is_array(type) != 0)) ||
        (((section_flags & (uint32_t)HCD_SF_PER_CPU) != 0U) !=
         (hcd_section_is_per_cpu(type) != 0)) ||
        (((section_flags & (uint32_t)HCD_SF_EXTERNAL) != 0U) !=
         (hcd_section_is_external(type) != 0))) {
        return hcd_builder_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                type, 0U);
    }
    HCD_ASSERT(known_entry_size > 0U);
    HCD_ASSERT(builder->section_count < HCD_MAX_SECTIONS);
    return HCD_OK;
}

static enum hcd_error hcd_builder_check_limits(
    uint32_t type,
    uint32_t entry_count,
    uint32_t entry_size,
    uint32_t alignment,
    struct hcd_validation_error *detail)
{
    if ((alignment < 8U) || !hcd_builder_power_of_two(alignment) ||
        (alignment > HCD_MAX_TOTAL_SIZE) || (entry_size == 0U)) {
        return hcd_builder_fail(detail, HCD_E_ALIGNMENT, type, 0U);
    }
    if ((type == HCD_SECTION_MEMORY_MAP) &&
        (entry_count > HCD_MAX_MEMORY_REGIONS)) {
        return hcd_builder_fail(detail, HCD_E_COUNT_LIMIT, type, 0U);
    }
    if ((type == HCD_SECTION_INTERRUPT_CONTROLLERS) &&
        (entry_count > HCD_MAX_INTERRUPT_CONTROLLERS)) {
        return hcd_builder_fail(detail, HCD_E_COUNT_LIMIT, type, 0U);
    }
    if (((type == HCD_SECTION_CPU_RECORDS) ||
         (type == HCD_SECTION_MMU_CAPABILITIES) ||
         (type == HCD_SECTION_VIRTUALIZATION)) &&
        (entry_count > HCD_MAX_CPUS)) {
        return hcd_builder_fail(detail, HCD_E_COUNT_LIMIT, type, 0U);
    }
    if ((type == HCD_SECTION_CPU_FEATURE_CATALOG) &&
        (entry_count > HCD_FEATURE_BITS)) {
        return hcd_builder_fail(detail, HCD_E_COUNT_LIMIT, type, 0U);
    }
    if ((type == HCD_SECTION_MMU_TRANSLATION_CONFIGS) &&
        (entry_count > HCD_MAX_MMU_CONFIGURATIONS)) {
        return hcd_builder_fail(detail, HCD_E_COUNT_LIMIT, type, 0U);
    }
    if ((type == HCD_SECTION_MMU_MAPPING_SIZES) &&
        (entry_count > HCD_MAX_MMU_MAPPING_SIZES)) {
        return hcd_builder_fail(detail, HCD_E_COUNT_LIMIT, type, 0U);
    }
    if ((type == HCD_SECTION_MMU_ARM64_CONTEXTS) &&
        (entry_count > HCD_MAX_MMU_CONFIGURATIONS)) {
        return hcd_builder_fail(detail, HCD_E_COUNT_LIMIT, type, 0U);
    }
    HCD_ASSERT(alignment >= 8U);
    HCD_ASSERT(entry_size > 0U);
    return HCD_OK;
}

static enum hcd_error hcd_builder_compute_size(
    uint32_t type,
    uint16_t flags,
    uint32_t entry_size,
    uint32_t entry_count,
    uint64_t *size,
    struct hcd_validation_error *detail)
{
    uint64_t expected_size = 0U;

    if (((((uint32_t)flags) & ((uint32_t)HCD_SF_ARRAY)) != 0U)) {
        if ((uint64_t)entry_count >
            (HCD_MAX_TOTAL_SIZE / (uint64_t)entry_size)) {
            return hcd_builder_fail(detail, HCD_E_COUNT_LIMIT, type, 0U);
        }
        expected_size = (uint64_t)entry_size * (uint64_t)entry_count;
    } else {
        if (entry_count != 1U) {
            return hcd_builder_fail(detail, HCD_E_SECTION_BOUNDS, type, 0U);
        }
        expected_size = entry_size;
    }
    *size = expected_size;
    HCD_ASSERT(expected_size <= HCD_MAX_TOTAL_SIZE);
    HCD_ASSERT(entry_size > 0U);
    return HCD_OK;
}

static enum hcd_error hcd_builder_check_section(
    const struct hcd_builder *builder,
    uint32_t type,
    uint16_t version,
    uint16_t flags,
    uint32_t entry_size,
    uint32_t entry_count,
    uint32_t alignment,
    uint64_t *size,
    struct hcd_validation_error *detail)
{
    enum hcd_error result = hcd_builder_check_classification(
        builder, type, version, flags, entry_size, detail);

    if (result != HCD_OK) {
        return result;
    }
    result = hcd_builder_check_limits(type, entry_count, entry_size,
                                      alignment, detail);
    if (result != HCD_OK) {
        return result;
    }
    return hcd_builder_compute_size(type, flags, entry_size,
                                    entry_count, size, detail);
}

static enum hcd_error hcd_builder_reserve_offset(
    const struct hcd_builder *builder,
    uint32_t type,
    uint32_t alignment,
    uint64_t size,
    uint64_t *offset,
    struct hcd_validation_error *detail)
{
    uint64_t aligned_cursor = 0U;

    if (builder->cursor > (UINT64_MAX - (uint64_t)(alignment - 1U))) {
        return hcd_builder_fail(detail, HCD_E_INTEGER_OVERFLOW, type,
                                builder->cursor);
    }
    aligned_cursor = (builder->cursor + (uint64_t)(alignment - 1U)) &
                    ~((uint64_t)alignment - 1U);
    if ((aligned_cursor > builder->capacity) ||
        (size > (builder->capacity - aligned_cursor)) ||
        (aligned_cursor > HCD_MAX_TOTAL_SIZE) ||
        (size > (HCD_MAX_TOTAL_SIZE - aligned_cursor))) {
        return hcd_builder_fail(detail, HCD_E_BAD_TOTAL_SIZE, type,
                                aligned_cursor);
    }
    *offset = aligned_cursor;
    HCD_ASSERT(*offset <= builder->capacity);
    HCD_ASSERT(size <= HCD_MAX_TOTAL_SIZE);
    return HCD_OK;
}

static void hcd_builder_commit_section(
    struct hcd_builder *builder,
    struct hcd_builder_section *section,
    uint32_t type,
    uint16_t version,
    uint16_t flags,
    uint32_t entry_size,
    uint32_t entry_count,
    uint32_t alignment,
    uint64_t offset,
    uint64_t size)
{
    hcd_builder_section_reset(section);
    section->type = type;
    section->version = version;
    section->flags = flags;
    section->offset = offset;
    section->size = size;
    section->entry_size = entry_size;
    section->entry_count = entry_count;
    section->alignment = alignment;
    hcd_builder_zero(builder->buffer, offset, size);
    builder->cursor = offset + size;
    builder->seen_mask |= UINT32_C(1) << type;
    builder->section_count++;
    HCD_ASSERT(builder->cursor <= builder->capacity);
    HCD_ASSERT(builder->section_count <= HCD_MAX_SECTIONS);
}

enum hcd_error hcd_builder_reserve_section(
    struct hcd_builder *builder,
    uint32_t type,
    uint16_t version,
    uint16_t flags,
    uint32_t entry_size,
    uint32_t entry_count,
    uint32_t alignment,
    uint8_t **section_data,
    uint64_t *section_size,
    struct hcd_validation_error *detail)
{
    uint64_t size = 0U;
    uint64_t offset = 0U;
    struct hcd_builder_section *section = NULL;
    enum hcd_error result = HCD_OK;

    if ((builder == NULL) || (section_data == NULL) ||
        (section_size == NULL) || (builder->buffer == NULL)) {
        return hcd_builder_fail(detail, HCD_E_NULL_INPUT, type, 0U);
    }
    if (builder->finalized != 0U) {
        return hcd_builder_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                type, builder->cursor);
    }
    result = hcd_builder_check_section(builder, type, version, flags,
                                       entry_size, entry_count, alignment,
                                       &size, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_builder_reserve_offset(builder, type, alignment, size,
                                        &offset, detail);
    if (result != HCD_OK) {
        return result;
    }
    section = &builder->sections[builder->section_count];
    hcd_builder_commit_section(builder, section, type, version, flags,
                               entry_size, entry_count, alignment,
                               offset, size);
    *section_data = &builder->buffer[offset];
    *section_size = size;
    HCD_ASSERT(builder->section_count > 0U);
    HCD_ASSERT(offset == section->offset);
    return HCD_OK;
}

static void hcd_builder_write_header(struct hcd_builder *builder)
{
    uint32_t flags = builder->flags;
    uint32_t index = 0U;

    for (index = 0U; index < builder->section_count; ++index) {
        if (((uint32_t)builder->sections[index].flags &
             (uint32_t)HCD_SF_EXTERNAL) != 0U) {
            flags |= HCD_F_EXTERNAL_REFERENCES;
        }
    }
    hcd_store_le32(builder->buffer, 0U, HCD_MAGIC);
    hcd_store_le16(builder->buffer, 4U, builder->version_major);
    hcd_store_le16(builder->buffer, 6U, HCD_VERSION_MINOR);
    hcd_store_le32(builder->buffer, 8U, HCD_HEADER_SIZE);
    hcd_store_le32(builder->buffer, 12U, builder->architecture);
    hcd_store_le64(builder->buffer, 16U, builder->cursor);
    hcd_store_le64(builder->buffer, 24U, 0U);
    hcd_store_le32(builder->buffer, 32U, flags);
    hcd_store_le32(builder->buffer, 36U, builder->section_count);
    hcd_store_le64(builder->buffer, 40U, HCD_HEADER_SIZE);
    hcd_store_le32(builder->buffer, 48U, builder->producer_id);
    hcd_store_le32(builder->buffer, 52U, builder->producer_version);
    hcd_store_le64(builder->buffer, 56U, 0U);
    HCD_ASSERT(index == builder->section_count);
    HCD_ASSERT(builder->cursor <= builder->capacity);
}

static void hcd_builder_write_directory(struct hcd_builder *builder)
{
    uint32_t index = 0U;

    for (index = 0U; index < builder->section_count; ++index) {
        const struct hcd_builder_section *section = &builder->sections[index];
        uint64_t offset = HCD_HEADER_SIZE +
            ((uint64_t)index * (uint64_t)HCD_SECTION_ENTRY_SIZE);

        hcd_store_le32(builder->buffer, offset, section->type);
        hcd_store_le16(builder->buffer, offset + 4U, section->version);
        hcd_store_le16(builder->buffer, offset + 6U, section->flags);
        hcd_store_le64(builder->buffer, offset + 8U, section->offset);
        hcd_store_le64(builder->buffer, offset + 16U, section->size);
        hcd_store_le32(builder->buffer, offset + 24U, section->entry_size);
        hcd_store_le32(builder->buffer, offset + 28U, section->entry_count);
        hcd_store_le32(builder->buffer, offset + 32U, section->alignment);
        hcd_store_le32(builder->buffer, offset + 36U, 0U);
    }
    HCD_ASSERT(index == builder->section_count);
    HCD_ASSERT(HCD_HEADER_SIZE + (uint64_t)index *
               HCD_SECTION_ENTRY_SIZE <= HCD_BUILDER_PAYLOAD_OFFSET);
}

enum hcd_error hcd_builder_finalize(
    struct hcd_builder *builder,
    uint64_t *total_size,
    struct hcd_validation_error *detail)
{
    uint64_t checksum = 0U;
    enum hcd_error result = HCD_OK;

    if ((builder == NULL) || (total_size == NULL) ||
        (builder->buffer == NULL)) {
        return hcd_builder_fail(detail, HCD_E_NULL_INPUT, 0U, 0U);
    }
    if ((builder->finalized != 0U) || (builder->section_count == 0U)) {
        return hcd_builder_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                0U, builder->cursor);
    }
    hcd_builder_write_header(builder);
    hcd_builder_write_directory(builder);
    result = hcd_crc64_hcd(builder->buffer, builder->cursor, &checksum);
    if (result != HCD_OK) {
        return hcd_builder_fail(detail, result, 0U, 24U);
    }
    hcd_store_le64(builder->buffer, 24U, checksum);
    builder->finalized = 1U;
    *total_size = builder->cursor;
    HCD_ASSERT(*total_size <= builder->capacity);
    HCD_ASSERT(builder->finalized == 1U);
    return HCD_OK;
}
