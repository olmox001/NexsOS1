#include "common/hcd_builder.h"
#include "common/hcd_endian.h"
#include "common/hcd_parser.h"


static enum hcd_error builder_add_cpu_sections(
    struct hcd_builder *builder,
    struct hcd_validation_error *detail)
{
    uint8_t *data = NULL;
    uint64_t size = 0U;
    enum hcd_error result = hcd_builder_reserve_section(builder,
        HCD_SECTION_CPU_SUMMARY, 1U, HCD_SF_REQUIRED,
        sizeof(struct hcd_cpu_summary), 1U, 8U, &data, &size, detail);

    if ((result != HCD_OK) || (size != sizeof(struct hcd_cpu_summary))) {
        return (result == HCD_OK) ? HCD_E_SECTION_BOUNDS : result;
    }
    hcd_store_le32(data, 0U, 1U);
    hcd_store_le32(data, 4U, 0U);
    hcd_store_le32(data, 8U, HCD_ARCH_AMD64);
    hcd_store_le32(data, 16U, HCD_FEATURE_WORDS);
    hcd_store_le64(data, 24U, 1U);
    hcd_store_le64(data, 88U, 1U);
    hcd_store_le64(data, 152U, 1U);
    result = hcd_builder_reserve_section(builder, HCD_SECTION_CPU_RECORDS,
        1U, HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_PER_CPU,
        sizeof(struct hcd_cpu_record), 1U, 8U, &data, &size, detail);
    if (result != HCD_OK) return result;
    hcd_store_le32(data, 0U, 0U);
    hcd_store_le32(data, 24U, HCD_ID_UNKNOWN);
    hcd_store_le32(data, 28U, HCD_CPU_F_PRESENT | HCD_CPU_F_BOOT |
        HCD_CPU_F_ONLINE | HCD_CPU_F_SCHEDULABLE);
    hcd_store_le32(data, 32U, HCD_ARCH_AMD64);
    hcd_store_le64(data, 80U, 1U);
    hcd_store_le64(data, 144U, 1U);
    hcd_store_le64(data, 208U, 1U);
    hcd_store_le32(data, 272U, HCD_ID_UNKNOWN);
    result = hcd_builder_reserve_section(builder,
        HCD_SECTION_CPU_FEATURE_CATALOG, 1U,
        HCD_SF_REQUIRED | HCD_SF_ARRAY,
        sizeof(struct hcd_cpu_feature_record), 1U, 8U,
        &data, &size, detail);
    if (result != HCD_OK) return result;
    hcd_store_le32(data, 0U, 0U);
    hcd_store_le32(data, 4U, HCD_ARCH_MASK_AMD64);
    return HCD_OK;
}

static enum hcd_error builder_add_memory_boot_sections(
    struct hcd_builder *builder,
    struct hcd_validation_error *detail)
{
    uint8_t *data = NULL;
    uint64_t size = 0U;
    enum hcd_error result = hcd_builder_reserve_section(builder,
        HCD_SECTION_MEMORY_MAP, 1U, HCD_SF_REQUIRED | HCD_SF_ARRAY,
        sizeof(struct hcd_memory_record), 1U, 8U, &data, &size, detail);

    if (result != HCD_OK) return result;
    hcd_store_le64(data, 0U, UINT64_C(0x100000));
    hcd_store_le64(data, 8U, UINT64_C(0x100000));
    hcd_store_le32(data, 16U, HCD_MEMORY_USABLE);
    hcd_store_le32(data, 24U, HCD_ID_UNKNOWN);
    return hcd_builder_reserve_section(builder, HCD_SECTION_BOOT_CONTEXT,
        1U, HCD_SF_REQUIRED, sizeof(struct hcd_boot_context), 1U, 8U,
        &data, &size, detail);
}

static enum hcd_error builder_add_external_sections(
    struct hcd_builder *builder,
    struct hcd_validation_error *detail)
{
    uint8_t *data = NULL;
    uint64_t size = 0U;
    enum hcd_error result = hcd_builder_reserve_section(builder,
        HCD_SECTION_PLATFORM_DEVICES, 1U,
        HCD_SF_ARRAY | HCD_SF_EXTERNAL,
        sizeof(struct hcd_platform_device_record), 0U, 8U,
        &data, &size, detail);

    if ((result == HCD_OK) && (size != 0U)) {
        return HCD_E_SECTION_BOUNDS;
    }
    return result;
}

static int test_builder_recovery_round_trip(void)
{
    uint8_t buffer[8192];
    struct hcd_builder builder = {0};
    struct hcd_validation_workspace validation_workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    uint64_t total_size = 0U;

    if (hcd_builder_init(&builder, buffer, sizeof(buffer), HCD_ARCH_AMD64,
                         HCD_F_INCOMPLETE, 1U, 1U, &detail) != HCD_OK) {
        return 1;
    }
    if (builder_add_cpu_sections(&builder, &detail) != HCD_OK) return 2;
    if (builder_add_memory_boot_sections(&builder, &detail) != HCD_OK) return 3;
    if (builder_add_external_sections(&builder, &detail) != HCD_OK) return 4;
    if (hcd_builder_finalize(&builder, &total_size, &detail) != HCD_OK) {
        return 5;
    }
    if (total_size > sizeof(buffer)) return 6;
    if ((hcd_load_le32(buffer, 32U) & HCD_F_EXTERNAL_REFERENCES) == 0U) {
        return 7;
    }
    if (hcd_validate(buffer, total_size, HCD_PROFILE_RECOVERY,
                     &validation_workspace, &detail, &view) != HCD_OK) {
        return 8;
    }
    if (builder.finalized != 1U) return 9;
    return 0;
}

static int test_builder_v2_writes_additive_mmu_sections(void)
{
    uint8_t buffer[4096];
    struct hcd_builder builder = {0};
    struct hcd_validation_error detail = {0};
    uint8_t *data = NULL;
    uint64_t size = 0U;
    uint64_t total_size = 0U;
    uint16_t flags = HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_PER_CPU;

    if (hcd_builder_init_v2(&builder, buffer, sizeof(buffer),
                            HCD_ARCH_AMD64, 0U, 1U, 1U, &detail) != HCD_OK) {
        return 1;
    }
    if (builder.version_major != HCD_VERSION_MAJOR_V2) {
        return 2;
    }
    if (hcd_builder_reserve_section(
            &builder, HCD_SECTION_MMU_TRANSLATION_CONFIGS, 1U, flags,
            sizeof(struct hcd_mmu_translation_config), 1U, 8U,
            &data, &size, &detail) != HCD_OK) {
        return 3;
    }
    if (size != sizeof(struct hcd_mmu_translation_config)) {
        return 4;
    }
    if (hcd_builder_reserve_section(
            &builder, HCD_SECTION_MMU_MAPPING_SIZES, 1U, flags,
            sizeof(struct hcd_mmu_mapping_size), 1U, 8U,
            &data, &size, &detail) != HCD_OK) {
        return 5;
    }
    if (hcd_builder_finalize(&builder, &total_size, &detail) != HCD_OK) {
        return 6;
    }
    if ((hcd_load_le16(buffer, 4U) != HCD_VERSION_MAJOR_V2) ||
        (total_size > sizeof(buffer))) {
        return 7;
    }
    return 0;
}

static int test_builder_rejects_duplicate_section(void)
{
    uint8_t buffer[4096];
    struct hcd_builder builder = {0};
    struct hcd_validation_error detail = {0};
    uint8_t *data = NULL;
    uint64_t size = 0U;

    if (hcd_builder_init(&builder, buffer, sizeof(buffer), HCD_ARCH_AMD64,
                         HCD_F_INCOMPLETE, 1U, 1U, &detail) != HCD_OK) {
        return 1;
    }
    if (hcd_builder_reserve_section(&builder, HCD_SECTION_CPU_SUMMARY, 1U,
            HCD_SF_REQUIRED, sizeof(struct hcd_cpu_summary), 1U, 8U,
            &data, &size, &detail) != HCD_OK) {
        return 2;
    }
    if (hcd_builder_reserve_section(&builder, HCD_SECTION_CPU_SUMMARY, 1U,
            HCD_SF_REQUIRED, sizeof(struct hcd_cpu_summary), 1U, 8U,
            &data, &size, &detail) != HCD_E_SECTION_DUPLICATE) {
        return 3;
    }
    return 0;
}

static int test_builder_rejects_capacity_overflow(void)
{
    uint8_t buffer[4096];
    struct hcd_builder builder = {0};
    struct hcd_validation_error detail = {0};
    uint8_t *data = NULL;
    uint64_t size = 0U;

    if (hcd_builder_init(&builder, buffer, sizeof(buffer), HCD_ARCH_AMD64,
                         HCD_F_INCOMPLETE, 1U, 1U, &detail) != HCD_OK) {
        return 1;
    }
    if (hcd_builder_reserve_section(&builder, HCD_SECTION_CPU_RECORDS, 1U,
            HCD_SF_ARRAY | HCD_SF_PER_CPU,
            sizeof(struct hcd_cpu_record), 10U, 8U,
            &data, &size, &detail) != HCD_E_BAD_TOTAL_SIZE) {
        return 2;
    }
    return 0;
}

static int test_builder_rejects_bad_alignment(void)
{
    uint8_t buffer[4096];
    struct hcd_builder builder = {0};
    struct hcd_validation_error detail = {0};
    uint8_t *data = NULL;
    uint64_t size = 0U;

    if (hcd_builder_init(&builder, buffer, sizeof(buffer), HCD_ARCH_AMD64,
                         HCD_F_INCOMPLETE, 1U, 1U, &detail) != HCD_OK) {
        return 1;
    }
    if (hcd_builder_reserve_section(&builder, HCD_SECTION_CPU_SUMMARY, 1U,
            HCD_SF_REQUIRED, sizeof(struct hcd_cpu_summary), 1U, 3U,
            &data, &size, &detail) != HCD_E_ALIGNMENT) {
        return 2;
    }
    return 0;
}


static int test_builder_rejects_memory_region_count_overflow(void)
{
    uint8_t buffer[65536];
    struct hcd_builder builder = {0};
    struct hcd_validation_error detail = {0};
    uint8_t *data = NULL;
    uint64_t size = 0U;

    if (hcd_builder_init(&builder, buffer, sizeof(buffer), HCD_ARCH_AMD64,
                         HCD_F_INCOMPLETE, 1U, 1U, &detail) != HCD_OK) {
        return 1;
    }
    if (hcd_builder_reserve_section(&builder, HCD_SECTION_MEMORY_MAP, 1U,
            HCD_SF_REQUIRED | HCD_SF_ARRAY,
            sizeof(struct hcd_memory_record), HCD_MAX_MEMORY_REGIONS + 1U,
            8U, &data, &size, &detail) != HCD_E_COUNT_LIMIT) {
        return 2;
    }
    return 0;
}

int main(void)
{
    if (test_builder_recovery_round_trip() != 0) {
        return 1;
    }
    if (test_builder_v2_writes_additive_mmu_sections() != 0) {
        return 2;
    }
    if (test_builder_rejects_duplicate_section() != 0) {
        return 3;
    }
    if (test_builder_rejects_capacity_overflow() != 0) {
        return 4;
    }
    if (test_builder_rejects_bad_alignment() != 0) {
        return 5;
    }
    if (test_builder_rejects_memory_region_count_overflow() != 0) {
        return 6;
    }
    return 0;
}
