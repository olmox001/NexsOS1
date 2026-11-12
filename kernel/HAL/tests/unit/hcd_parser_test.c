#include "common/hcd_parser.h"
#include "common/hcd_crc64.h"

#define TEST_HEADER_BYTES 64U
#define TEST_DIRECTORY_OFFSET 64U
#define TEST_SECTION_COUNT 5U
#define TEST_DIRECTORY_BYTES (TEST_SECTION_COUNT * HCD_SECTION_ENTRY_SIZE)
#define TEST_CPU_SUMMARY_OFFSET (TEST_DIRECTORY_OFFSET + TEST_DIRECTORY_BYTES)
#define TEST_CPU_RECORDS_OFFSET (TEST_CPU_SUMMARY_OFFSET + sizeof(struct hcd_cpu_summary))
#define TEST_FEATURE_OFFSET (TEST_CPU_RECORDS_OFFSET + sizeof(struct hcd_cpu_record))
#define TEST_MEMORY_OFFSET (TEST_FEATURE_OFFSET + sizeof(struct hcd_cpu_feature_record))
#define TEST_BOOT_OFFSET (TEST_MEMORY_OFFSET + sizeof(struct hcd_memory_record))
#define TEST_TOTAL_SIZE (TEST_BOOT_OFFSET + sizeof(struct hcd_boot_context))

static void put_u16(uint8_t *buffer, uint32_t offset, uint16_t value)
{
    buffer[offset] = (uint8_t)value;
    buffer[offset + 1U] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *buffer, uint32_t offset, uint32_t value)
{
    uint32_t index = 0U;

    for (index = 0U; index < 4U; ++index) {
        buffer[offset + index] = (uint8_t)(value >> (index * 8U));
    }
}

static void put_u64(uint8_t *buffer, uint32_t offset, uint64_t value)
{
    uint32_t index = 0U;

    for (index = 0U; index < 8U; ++index) {
        buffer[offset + index] = (uint8_t)(value >> (index * 8U));
    }
}

static void put_section(
    uint8_t *buffer,
    uint32_t index,
    uint32_t type,
    uint16_t flags,
    uint64_t offset,
    uint32_t entry_size,
    uint32_t entry_count)
{
    uint32_t entry = TEST_DIRECTORY_OFFSET +
        (index * HCD_SECTION_ENTRY_SIZE);

    put_u32(buffer, entry, type);
    put_u16(buffer, entry + 4U, 1U);
    put_u16(buffer, entry + 6U, flags);
    put_u64(buffer, entry + 8U, offset);
    put_u64(buffer, entry + 16U, (uint64_t)entry_size * entry_count);
    put_u32(buffer, entry + 24U, entry_size);
    put_u32(buffer, entry + 28U, entry_count);
    put_u32(buffer, entry + 32U, 8U);
    put_u32(buffer, entry + 36U, 0U);
}

static int seal_fixture(uint8_t *buffer)
{
    uint64_t checksum = 0U;

    put_u64(buffer, 24U, 0U);
    if (hcd_crc64_hcd(buffer, TEST_TOTAL_SIZE, &checksum) != HCD_OK) {
        return 1;
    }
    put_u64(buffer, 24U, checksum);
    return 0;
}


static void initialize_fixture_header(uint8_t *buffer)
{
    uint32_t index = 0U;

    for (index = 0U; index < TEST_TOTAL_SIZE; ++index) {
        buffer[index] = 0U;
    }
    put_u32(buffer, 0U, HCD_MAGIC);
    put_u16(buffer, 4U, HCD_VERSION_MAJOR);
    put_u16(buffer, 6U, HCD_VERSION_MINOR);
    put_u32(buffer, 8U, HCD_HEADER_SIZE);
    put_u32(buffer, 12U, HCD_ARCH_AMD64);
    put_u64(buffer, 16U, TEST_TOTAL_SIZE);
    put_u64(buffer, 24U, 0U);
    put_u32(buffer, 32U, HCD_F_INCOMPLETE);
    put_u32(buffer, 36U, TEST_SECTION_COUNT);
    put_u64(buffer, 40U, TEST_DIRECTORY_OFFSET);
    put_u32(buffer, 48U, 1U);
    put_u32(buffer, 52U, 1U);
    put_u64(buffer, 56U, 0U);
}

static void initialize_fixture_directory(uint8_t *buffer)
{
    put_section(buffer, 0U, HCD_SECTION_CPU_SUMMARY, HCD_SF_REQUIRED,
                TEST_CPU_SUMMARY_OFFSET,
                (uint32_t)sizeof(struct hcd_cpu_summary), 1U);
    put_section(buffer, 1U, HCD_SECTION_CPU_RECORDS,
                HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_PER_CPU,
                TEST_CPU_RECORDS_OFFSET,
                (uint32_t)sizeof(struct hcd_cpu_record), 1U);
    put_section(buffer, 2U, HCD_SECTION_CPU_FEATURE_CATALOG,
                HCD_SF_REQUIRED | HCD_SF_ARRAY,
                TEST_FEATURE_OFFSET,
                (uint32_t)sizeof(struct hcd_cpu_feature_record), 1U);
    put_section(buffer, 3U, HCD_SECTION_MEMORY_MAP,
                HCD_SF_REQUIRED | HCD_SF_ARRAY,
                TEST_MEMORY_OFFSET,
                (uint32_t)sizeof(struct hcd_memory_record), 1U);
    put_section(buffer, 4U, HCD_SECTION_BOOT_CONTEXT, HCD_SF_REQUIRED,
                TEST_BOOT_OFFSET,
                (uint32_t)sizeof(struct hcd_boot_context), 1U);
}

static void initialize_fixture_cpu_records(uint8_t *buffer)
{
    put_u32(buffer, TEST_CPU_SUMMARY_OFFSET, 1U);
    put_u32(buffer, TEST_CPU_SUMMARY_OFFSET + 4U, 0U);
    put_u32(buffer, TEST_CPU_SUMMARY_OFFSET + 8U, HCD_ARCH_AMD64);
    put_u32(buffer, TEST_CPU_SUMMARY_OFFSET + 12U, 0U);
    put_u32(buffer, TEST_CPU_SUMMARY_OFFSET + 16U, HCD_FEATURE_WORDS);
    put_u64(buffer, TEST_CPU_SUMMARY_OFFSET + 24U, UINT64_C(1));
    put_u64(buffer, TEST_CPU_SUMMARY_OFFSET + 88U, UINT64_C(1));
    put_u64(buffer, TEST_CPU_SUMMARY_OFFSET + 152U, UINT64_C(1));
    put_u32(buffer, TEST_CPU_RECORDS_OFFSET, 0U);
    put_u32(buffer, TEST_CPU_RECORDS_OFFSET + 28U,
            HCD_CPU_F_PRESENT | HCD_CPU_F_BOOT |
            HCD_CPU_F_ONLINE | HCD_CPU_F_SCHEDULABLE);
    put_u32(buffer, TEST_CPU_RECORDS_OFFSET + 32U, HCD_ARCH_AMD64);
    put_u64(buffer, TEST_CPU_RECORDS_OFFSET + 80U, UINT64_C(1));
    put_u64(buffer, TEST_CPU_RECORDS_OFFSET + 144U, UINT64_C(1));
    put_u64(buffer, TEST_CPU_RECORDS_OFFSET + 208U, UINT64_C(1));
    put_u32(buffer, TEST_CPU_RECORDS_OFFSET + 272U, HCD_ID_UNKNOWN);
}

static void initialize_fixture_memory_records(uint8_t *buffer)
{
    put_u32(buffer, TEST_FEATURE_OFFSET, 0U);
    put_u32(buffer, TEST_FEATURE_OFFSET + 4U, HCD_ARCH_MASK_AMD64);
    put_u64(buffer, TEST_MEMORY_OFFSET, UINT64_C(0x100000));
    put_u64(buffer, TEST_MEMORY_OFFSET + 8U, UINT64_C(0x100000));
    put_u32(buffer, TEST_MEMORY_OFFSET + 16U, HCD_MEMORY_USABLE);
    put_u32(buffer, TEST_MEMORY_OFFSET + 24U, HCD_ID_UNKNOWN);
}

static int build_fixture(uint8_t *buffer)
{
    if (buffer == NULL) {
        return 1;
    }
    initialize_fixture_header(buffer);
    initialize_fixture_directory(buffer);
    initialize_fixture_cpu_records(buffer);
    initialize_fixture_memory_records(buffer);
    return seal_fixture(buffer);
}

static int test_valid_recovery_fixture(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct hcd_section_view section = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) != HCD_OK) {
        return 2;
    }
    if ((view.buffer != buffer) || (view.size != TEST_TOTAL_SIZE)) {
        return 3;
    }
    if (hcd_find_section(&view, HCD_SECTION_MEMORY_MAP, &section) != HCD_OK) {
        return 4;
    }
    if (section.entry_count != 1U) {
        return 5;
    }
    if (hcd_find_section(&view, 99U, &section) != HCD_E_SECTION_NOT_FOUND) {
        return 6;
    }
    return 0;
}

static int test_bad_checksum_clears_view(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) != HCD_OK) {
        return 2;
    }
    buffer[TEST_CPU_SUMMARY_OFFSET] ^= UINT8_C(1);
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) != HCD_E_BAD_CHECKSUM) {
        return 3;
    }
    if (view.buffer != NULL) {
        return 4;
    }
    return 0;
}

static int test_duplicate_section(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    put_section(buffer, 4U, HCD_SECTION_MEMORY_MAP,
                HCD_SF_REQUIRED | HCD_SF_ARRAY,
                TEST_BOOT_OFFSET,
                (uint32_t)sizeof(struct hcd_memory_record), 1U);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) != HCD_E_SECTION_DUPLICATE) {
        return 3;
    }
    return 0;
}

static int test_directory_out_of_bounds(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, 40U, TEST_TOTAL_SIZE - 8U);
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) != HCD_E_BAD_SECTION_DIRECTORY) {
        return 2;
    }
    return 0;
}

static int test_profile_rejects_incomplete_core(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_E_INCOMPLETE_NOT_ALLOWED) {
        return 2;
    }
    return 0;
}


static int test_cpu_count_mismatch(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    put_u32(buffer, TEST_CPU_SUMMARY_OFFSET, 2U);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) != HCD_E_SEMANTIC_INCONSISTENCY) {
        return 3;
    }
    return 0;
}

static int test_cpu_feature_subset_violation(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, TEST_CPU_RECORDS_OFFSET + 208U, UINT64_C(2));
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) != HCD_E_CPU_FEATURE_SET_INVALID) {
        return 3;
    }
    return 0;
}

static int test_memory_map_overflow(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, TEST_MEMORY_OFFSET, UINT64_MAX - 4U);
    put_u64(buffer, TEST_MEMORY_OFFSET + 8U, 16U);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) != HCD_E_MEMORY_MAP_INVALID) {
        return 3;
    }
    return 0;
}

static int test_memory_reserved_nonzero(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, TEST_MEMORY_OFFSET + 32U, 1U);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) != HCD_E_MEMORY_MAP_INVALID) {
        return 3;
    }
    return 0;
}

static int test_section_overlap(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, TEST_DIRECTORY_OFFSET +
            (3U * HCD_SECTION_ENTRY_SIZE) + 8U, TEST_CPU_SUMMARY_OFFSET);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) != HCD_E_SECTION_OVERLAP) {
        return 3;
    }
    return 0;
}

static int test_unknown_required_section(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    put_section(buffer, 4U, 99U, HCD_SF_REQUIRED, TEST_BOOT_OFFSET,
                (uint32_t)sizeof(struct hcd_boot_context), 1U);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) !=
        HCD_E_SECTION_UNKNOWN_REQUIRED) {
        return 3;
    }
    return 0;
}

static int test_external_flag_consistency(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    put_u32(buffer, 32U, HCD_F_INCOMPLETE | HCD_F_EXTERNAL_REFERENCES);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) !=
        HCD_E_SEMANTIC_INCONSISTENCY) {
        return 3;
    }
    return 0;
}

static int test_header_reserved_nonzero(void)
{
    uint8_t buffer[TEST_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, 56U, 1U);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
                     &workspace, &detail, &view) != HCD_E_RESERVED_NONZERO) {
        return 3;
    }
    return 0;
}

int main(void)
{
    if (test_valid_recovery_fixture() != 0) {
        return 1;
    }
    if (test_bad_checksum_clears_view() != 0) {
        return 2;
    }
    if (test_duplicate_section() != 0) {
        return 3;
    }
    if (test_directory_out_of_bounds() != 0) {
        return 4;
    }
    if (test_profile_rejects_incomplete_core() != 0) {
        return 5;
    }
    if (test_cpu_count_mismatch() != 0) {
        return 6;
    }
    if (test_cpu_feature_subset_violation() != 0) {
        return 7;
    }
    if (test_memory_map_overflow() != 0) {
        return 8;
    }
    if (test_memory_reserved_nonzero() != 0) {
        return 9;
    }
    if (test_section_overlap() != 0) {
        return 10;
    }
    if (test_unknown_required_section() != 0) {
        return 11;
    }
    if (test_external_flag_consistency() != 0) {
        return 12;
    }
    if (test_header_reserved_nonzero() != 0) {
        return 13;
    }
    return 0;
}
