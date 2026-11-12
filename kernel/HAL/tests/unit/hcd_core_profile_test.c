#include "common/hcd_parser.h"
#include "common/hcd_crc64.h"

#if __STDC_HOSTED__
#include <stdio.h>
#endif

#define CORE_SECTIONS 15U
#define CORE_DIRECTORY_OFFSET 64U
#define CORE_SUMMARY_OFFSET (CORE_DIRECTORY_OFFSET + CORE_SECTIONS * HCD_SECTION_ENTRY_SIZE)
#define CORE_CPU_OFFSET (CORE_SUMMARY_OFFSET + sizeof(struct hcd_cpu_summary))
#define CORE_FEATURE_OFFSET (CORE_CPU_OFFSET + sizeof(struct hcd_cpu_record))
#define CORE_TOPOLOGY_OFFSET (CORE_FEATURE_OFFSET + sizeof(struct hcd_cpu_feature_record))
#define CORE_MEMORY_OFFSET (CORE_TOPOLOGY_OFFSET + 2U * sizeof(struct hcd_topology_record))
#define CORE_INTERRUPT_OFFSET (CORE_MEMORY_OFFSET + 3U * sizeof(struct hcd_memory_record))
#define CORE_VECTOR_OFFSET (CORE_INTERRUPT_OFFSET + sizeof(struct hcd_interrupt_controller_record))
#define CORE_MMU_OFFSET (CORE_VECTOR_OFFSET + sizeof(struct hcd_vector_record))
#define CORE_BOOT_OFFSET (CORE_MMU_OFFSET + sizeof(struct hcd_mmu_capabilities))
#define CORE_TIMER_OFFSET (CORE_BOOT_OFFSET + sizeof(struct hcd_boot_context))
#define CORE_RAW_OFFSET (CORE_TIMER_OFFSET + sizeof(struct hcd_timer_record))
#define CORE_CACHE_OFFSET (CORE_RAW_OFFSET + sizeof(struct hcd_cpu_raw_register_record))
#define CORE_VIRT_OFFSET (CORE_CACHE_OFFSET + sizeof(struct hcd_cache_record))
#define CORE_FW_OFFSET (CORE_VIRT_OFFSET + sizeof(struct hcd_virtualization_record))
#define CORE_DEVICE_OFFSET (CORE_FW_OFFSET + sizeof(struct hcd_firmware_reference))
#define CORE_TOTAL_SIZE (CORE_DEVICE_OFFSET + sizeof(struct hcd_platform_device_record))
#define V2_DIRECTORY_SHIFT (3U * HCD_SECTION_ENTRY_SIZE)
#define V2_SHIFTED_OFFSET(offset) ((offset) + V2_DIRECTORY_SHIFT)
#define V2_CONFIG_OFFSET (CORE_TOTAL_SIZE + V2_DIRECTORY_SHIFT)
#define V2_MAPPING_OFFSET (V2_CONFIG_OFFSET + 2U * sizeof(struct hcd_mmu_translation_config))
#define V2_CONTEXT_OFFSET (V2_MAPPING_OFFSET + 6U * sizeof(struct hcd_mmu_mapping_size))
#define V2_TOTAL_SIZE (V2_CONTEXT_OFFSET + 2U * sizeof(struct hcd_mmu_arm64_context))
#define V2_DUPLICATE_CURRENT_SIZE (V2_TOTAL_SIZE + 2U * sizeof(struct hcd_mmu_mapping_size))

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
    uint32_t entry = CORE_DIRECTORY_OFFSET +
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

static int seal_fixture_size(uint8_t *buffer, uint64_t size)
{
    uint64_t checksum = 0U;

    put_u64(buffer, 24U, 0U);
    if (hcd_crc64_hcd(buffer, size, &checksum) != HCD_OK) {
        return 1;
    }
    put_u64(buffer, 24U, checksum);
    return 0;
}

static int seal_fixture(uint8_t *buffer)
{
    return seal_fixture_size(buffer, CORE_TOTAL_SIZE);
}


static void initialize_core_header(uint8_t *buffer)
{
    uint32_t index = 0U;

    for (index = 0U; index < CORE_TOTAL_SIZE; ++index) {
        buffer[index] = 0U;
    }
    put_u32(buffer, 0U, HCD_MAGIC);
    put_u16(buffer, 4U, HCD_VERSION_MAJOR);
    put_u16(buffer, 6U, HCD_VERSION_MINOR);
    put_u32(buffer, 8U, HCD_HEADER_SIZE);
    put_u32(buffer, 12U, HCD_ARCH_AMD64);
    put_u64(buffer, 16U, CORE_TOTAL_SIZE);
    put_u32(buffer, 32U, HCD_F_EXTERNAL_REFERENCES);
    put_u32(buffer, 36U, CORE_SECTIONS);
    put_u64(buffer, 40U, CORE_DIRECTORY_OFFSET);
    put_u32(buffer, 48U, 1U);
    put_u32(buffer, 52U, 1U);
}

static void initialize_core_directory(uint8_t *buffer)
{
    put_section(buffer, 0U, HCD_SECTION_CPU_SUMMARY, HCD_SF_REQUIRED,
                CORE_SUMMARY_OFFSET, sizeof(struct hcd_cpu_summary), 1U);
    put_section(buffer, 1U, HCD_SECTION_CPU_RECORDS,
                HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_PER_CPU,
                CORE_CPU_OFFSET, sizeof(struct hcd_cpu_record), 1U);
    put_section(buffer, 2U, HCD_SECTION_CPU_FEATURE_CATALOG,
                HCD_SF_REQUIRED | HCD_SF_ARRAY,
                CORE_FEATURE_OFFSET, sizeof(struct hcd_cpu_feature_record), 1U);
    put_section(buffer, 3U, HCD_SECTION_TOPOLOGY,
                HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_PER_CPU,
                CORE_TOPOLOGY_OFFSET, sizeof(struct hcd_topology_record), 2U);
    put_section(buffer, 4U, HCD_SECTION_MEMORY_MAP,
                HCD_SF_REQUIRED | HCD_SF_ARRAY,
                CORE_MEMORY_OFFSET, sizeof(struct hcd_memory_record), 3U);
    put_section(buffer, 5U, HCD_SECTION_INTERRUPT_CONTROLLERS,
                HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_EXTERNAL,
                CORE_INTERRUPT_OFFSET,
                sizeof(struct hcd_interrupt_controller_record), 1U);
    put_section(buffer, 6U, HCD_SECTION_VECTOR_LAYOUT,
                HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_PER_CPU,
                CORE_VECTOR_OFFSET, sizeof(struct hcd_vector_record), 1U);
    put_section(buffer, 7U, HCD_SECTION_MMU_CAPABILITIES,
                HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_PER_CPU,
                CORE_MMU_OFFSET, sizeof(struct hcd_mmu_capabilities), 1U);
    put_section(buffer, 8U, HCD_SECTION_BOOT_CONTEXT, HCD_SF_REQUIRED,
                CORE_BOOT_OFFSET, sizeof(struct hcd_boot_context), 1U);
    put_section(buffer, 9U, HCD_SECTION_TIMER_CAPABILITIES,
                HCD_SF_REQUIRED | HCD_SF_ARRAY,
                CORE_TIMER_OFFSET, sizeof(struct hcd_timer_record), 1U);
    put_section(buffer, 10U, HCD_SECTION_CPU_RAW_REGISTERS,
                HCD_SF_ARRAY | HCD_SF_PER_CPU,
                CORE_RAW_OFFSET, sizeof(struct hcd_cpu_raw_register_record), 1U);
    put_section(buffer, 11U, HCD_SECTION_CACHE,
                HCD_SF_ARRAY | HCD_SF_PER_CPU,
                CORE_CACHE_OFFSET, sizeof(struct hcd_cache_record), 1U);
    put_section(buffer, 12U, HCD_SECTION_VIRTUALIZATION,
                HCD_SF_ARRAY | HCD_SF_PER_CPU,
                CORE_VIRT_OFFSET, sizeof(struct hcd_virtualization_record), 1U);
    put_section(buffer, 13U, HCD_SECTION_FIRMWARE_REFERENCES,
                HCD_SF_ARRAY | HCD_SF_EXTERNAL,
                CORE_FW_OFFSET, sizeof(struct hcd_firmware_reference), 1U);
    put_section(buffer, 14U, HCD_SECTION_PLATFORM_DEVICES,
                HCD_SF_ARRAY | HCD_SF_EXTERNAL,
                CORE_DEVICE_OFFSET, sizeof(struct hcd_platform_device_record), 1U);
}

static void initialize_core_cpu_records(uint8_t *buffer)
{
    put_u32(buffer, CORE_SUMMARY_OFFSET, 1U);
    put_u32(buffer, CORE_SUMMARY_OFFSET + 4U, 0U);
    put_u32(buffer, CORE_SUMMARY_OFFSET + 8U, HCD_ARCH_AMD64);
    put_u32(buffer, CORE_SUMMARY_OFFSET + 16U, HCD_FEATURE_WORDS);
    put_u64(buffer, CORE_SUMMARY_OFFSET + 24U, 1U);
    put_u64(buffer, CORE_SUMMARY_OFFSET + 88U, 1U);
    put_u64(buffer, CORE_SUMMARY_OFFSET + 152U, 1U);
    put_u32(buffer, CORE_CPU_OFFSET, 0U);
    put_u32(buffer, CORE_CPU_OFFSET + 24U, HCD_ID_UNKNOWN);
    put_u32(buffer, CORE_CPU_OFFSET + 28U,
            HCD_CPU_F_PRESENT | HCD_CPU_F_BOOT |
            HCD_CPU_F_ONLINE | HCD_CPU_F_SCHEDULABLE);
    put_u32(buffer, CORE_CPU_OFFSET + 32U, HCD_ARCH_AMD64);
    put_u64(buffer, CORE_CPU_OFFSET + 80U, 1U);
    put_u64(buffer, CORE_CPU_OFFSET + 144U, 1U);
    put_u64(buffer, CORE_CPU_OFFSET + 208U, 1U);
    put_u32(buffer, CORE_CPU_OFFSET + 272U, 0U);
    put_u32(buffer, CORE_FEATURE_OFFSET, 0U);
    put_u32(buffer, CORE_FEATURE_OFFSET + 4U, HCD_ARCH_MASK_AMD64);
    put_u32(buffer, CORE_TOPOLOGY_OFFSET, 0U);
    put_u32(buffer, CORE_TOPOLOGY_OFFSET + 4U, HCD_ID_UNKNOWN);
    put_u32(buffer, CORE_TOPOLOGY_OFFSET + 8U, HCD_ID_UNKNOWN);
    put_u32(buffer, CORE_TOPOLOGY_OFFSET + 12U, HCD_TOPOLOGY_PACKAGE);
    put_u32(buffer, CORE_TOPOLOGY_OFFSET + 32U, 1U);
    put_u32(buffer, CORE_TOPOLOGY_OFFSET + 36U, 0U);
    put_u32(buffer, CORE_TOPOLOGY_OFFSET + 40U, 0U);
    put_u32(buffer, CORE_TOPOLOGY_OFFSET + 44U, HCD_TOPOLOGY_THREAD);
}

static void initialize_core_memory_and_boot(uint8_t *buffer)
{
    put_u64(buffer, CORE_MEMORY_OFFSET, UINT64_C(0x100000));
    put_u64(buffer, CORE_MEMORY_OFFSET + 8U, UINT64_C(0x10000));
    put_u32(buffer, CORE_MEMORY_OFFSET + 16U, HCD_MEMORY_BOOTSTRAP);
    put_u32(buffer, CORE_MEMORY_OFFSET + 24U, HCD_ID_UNKNOWN);
    put_u64(buffer, CORE_MEMORY_OFFSET + 40U, UINT64_C(0x200000));
    put_u64(buffer, CORE_MEMORY_OFFSET + 48U, UINT64_C(0x10000));
    put_u32(buffer, CORE_MEMORY_OFFSET + 56U, HCD_MEMORY_BOOTSTRAP);
    put_u32(buffer, CORE_MEMORY_OFFSET + 64U, HCD_ID_UNKNOWN);
    put_u64(buffer, CORE_MEMORY_OFFSET + 80U, UINT64_C(0xFEE00000));
    put_u64(buffer, CORE_MEMORY_OFFSET + 88U, UINT64_C(0x1000));
    put_u32(buffer, CORE_MEMORY_OFFSET + 96U, HCD_MEMORY_MMIO);
    put_u32(buffer, CORE_MEMORY_OFFSET + 104U, HCD_ID_UNKNOWN);
    put_u32(buffer, CORE_BOOT_OFFSET, HCD_BOOT_MULTIBOOT2);
    put_u32(buffer, CORE_BOOT_OFFSET + 4U, HCD_BOOT_MODE_BIOS);
    put_u32(buffer, CORE_BOOT_OFFSET + 8U, HCD_ENTRY_AMD64_PROTECTED32);
    put_u64(buffer, CORE_BOOT_OFFSET + 16U, UINT64_C(0x100000));
    put_u64(buffer, CORE_BOOT_OFFSET + 24U, UINT64_C(0x10000));
    put_u64(buffer, CORE_BOOT_OFFSET + 32U, UINT64_C(0x200000));
    put_u64(buffer, CORE_BOOT_OFFSET + 40U, UINT64_C(0x200000));
    put_u64(buffer, CORE_BOOT_OFFSET + 48U, UINT64_C(0x10000));
}

static void initialize_core_interrupt_mmu_timer(uint8_t *buffer)
{
    put_u32(buffer, CORE_INTERRUPT_OFFSET, 0U);
    put_u32(buffer, CORE_INTERRUPT_OFFSET + 4U, HCD_INTERRUPT_XAPIC);
    put_u32(buffer, CORE_INTERRUPT_OFFSET + 12U, 1U);
    put_u64(buffer, CORE_INTERRUPT_OFFSET + 16U, UINT64_C(0xFEE00000));
    put_u64(buffer, CORE_INTERRUPT_OFFSET + 24U, UINT64_C(0x1000));
    put_u32(buffer, CORE_INTERRUPT_OFFSET + 32U, 0U);
    put_u32(buffer, CORE_INTERRUPT_OFFSET + 36U, 256U);
    put_u32(buffer, CORE_INTERRUPT_OFFSET + 40U, HCD_ID_UNKNOWN);
    put_u32(buffer, CORE_INTERRUPT_OFFSET + 44U, HCD_ID_UNKNOWN);
    put_u32(buffer, CORE_INTERRUPT_OFFSET + 48U, 1U);
    put_u32(buffer, CORE_VECTOR_OFFSET, 0U);
    put_u32(buffer, CORE_VECTOR_OFFSET + 4U, HCD_VECTOR_EXCEPTION);
    put_u32(buffer, CORE_VECTOR_OFFSET + 12U, 0U);
    put_u32(buffer, CORE_MMU_OFFSET, HCD_ARCH_AMD64);
    put_u32(buffer, CORE_MMU_OFFSET + 8U, 48U);
    put_u32(buffer, CORE_MMU_OFFSET + 12U, 48U);
    put_u32(buffer, CORE_MMU_OFFSET + 16U, 1U);
    put_u32(buffer, CORE_MMU_OFFSET + 24U, 7U);
    put_u32(buffer, CORE_MMU_OFFSET + 28U, 7U);
    put_u32(buffer, CORE_MMU_OFFSET + 44U, 0U);
    put_u32(buffer, CORE_TIMER_OFFSET, 0U);
    put_u32(buffer, CORE_TIMER_OFFSET + 4U, HCD_TIMER_TSC);
    put_u32(buffer, CORE_TIMER_OFFSET + 12U, 64U);
    put_u64(buffer, CORE_TIMER_OFFSET + 16U, UINT64_C(2400000000));
    put_u32(buffer, CORE_TIMER_OFFSET + 32U, HCD_ID_UNKNOWN);
    put_u32(buffer, CORE_TIMER_OFFSET + 36U, 0U);
}

static void initialize_core_optional_sections(uint8_t *buffer)
{
    put_u32(buffer, CORE_RAW_OFFSET, 0U);
    put_u32(buffer, CORE_RAW_OFFSET + 4U, HCD_RAW_SPACE_CPUID);
    put_u32(buffer, CORE_RAW_OFFSET + 8U, 1U);
    put_u64(buffer, CORE_RAW_OFFSET + 16U, 1U);
    put_u32(buffer, CORE_RAW_OFFSET + 24U, HCD_RAW_ACCESS_READ);
    put_u32(buffer, CORE_CACHE_OFFSET, 0U);
    put_u32(buffer, CORE_CACHE_OFFSET + 4U, 0U);
    put_u16(buffer, CORE_CACHE_OFFSET + 8U, 1U);
    put_u16(buffer, CORE_CACHE_OFFSET + 10U, 1U);
    put_u64(buffer, CORE_CACHE_OFFSET + 16U, UINT64_C(32768));
    put_u32(buffer, CORE_CACHE_OFFSET + 24U, 64U);
    put_u32(buffer, CORE_CACHE_OFFSET + 28U, 8U);
    put_u32(buffer, CORE_CACHE_OFFSET + 32U, 64U);
    put_u32(buffer, CORE_CACHE_OFFSET + 36U, 1U);
    put_u32(buffer, CORE_VIRT_OFFSET, HCD_ARCH_AMD64);
    put_u32(buffer, CORE_VIRT_OFFSET + 4U, 1U);
    put_u32(buffer, CORE_VIRT_OFFSET + 8U, 1U);
    put_u32(buffer, CORE_VIRT_OFFSET + 20U, 1U);
    put_u32(buffer, CORE_VIRT_OFFSET + 24U, 1U);
    put_u32(buffer, CORE_VIRT_OFFSET + 28U, 0U);
    put_u32(buffer, CORE_FW_OFFSET, 1U);
    put_u32(buffer, CORE_FW_OFFSET + 12U, HCD_MEMORY_BOOTSTRAP);
    put_u64(buffer, CORE_FW_OFFSET + 16U, UINT64_C(0x100000));
    put_u64(buffer, CORE_FW_OFFSET + 24U, UINT64_C(0x1000));
    put_u32(buffer, CORE_DEVICE_OFFSET, 0U);
    put_u32(buffer, CORE_DEVICE_OFFSET + 4U, HCD_DEVICE_PCI);
    put_u64(buffer, CORE_DEVICE_OFFSET + 16U, UINT64_C(0xFEE00000));
    put_u64(buffer, CORE_DEVICE_OFFSET + 24U, UINT64_C(0x1000));
    put_u32(buffer, CORE_DEVICE_OFFSET + 32U, HCD_ID_UNKNOWN);
    put_u32(buffer, CORE_DEVICE_OFFSET + 36U, HCD_ID_UNKNOWN);
}

static int build_core_fixture(uint8_t *buffer)
{
    if (buffer == NULL) return 1;
    initialize_core_header(buffer);
    initialize_core_directory(buffer);
    initialize_core_cpu_records(buffer);
    initialize_core_memory_and_boot(buffer);
    initialize_core_interrupt_mmu_timer(buffer);
    initialize_core_optional_sections(buffer);
    return seal_fixture(buffer);
}

static int build_v2_core_fixture(uint8_t *buffer)
{
    uint32_t index = 0U;
    uint64_t config_offset = V2_CONFIG_OFFSET;
    uint64_t mapping_offset = V2_MAPPING_OFFSET;

    if (buffer == NULL) {
        return 1;
    }
    for (index = 0U; index < V2_TOTAL_SIZE; ++index) {
        buffer[index] = 0U;
    }
    if (build_core_fixture(buffer) != 0) {
        return 2;
    }
    for (index = CORE_TOTAL_SIZE; index > CORE_SUMMARY_OFFSET; --index) {
        buffer[index - 1U + V2_DIRECTORY_SHIFT] = buffer[index - 1U];
    }
    for (index = CORE_SUMMARY_OFFSET;
         index < CORE_SUMMARY_OFFSET + V2_DIRECTORY_SHIFT; ++index) {
        buffer[index] = 0U;
    }
    for (index = 0U; index < CORE_SECTIONS; ++index) {
        uint32_t entry_offset = CORE_DIRECTORY_OFFSET +
            index * HCD_SECTION_ENTRY_SIZE;
        uint64_t section_offset = 0U;
        uint32_t byte = 0U;

        for (byte = 0U; byte < 8U; ++byte) {
            section_offset |=
                ((uint64_t)buffer[entry_offset + 8U + byte]) << (byte * 8U);
        }
        put_u64(buffer, entry_offset + 8U,
                section_offset + V2_DIRECTORY_SHIFT);
    }
    put_u16(buffer, 4U, HCD_VERSION_MAJOR_V2);
    put_u64(buffer, 16U, V2_TOTAL_SIZE);
    put_u32(buffer, 36U, CORE_SECTIONS + 2U);
    put_section(buffer, CORE_SECTIONS, HCD_SECTION_MMU_TRANSLATION_CONFIGS,
                HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_PER_CPU,
                config_offset, sizeof(struct hcd_mmu_translation_config), 1U);
    put_section(buffer, CORE_SECTIONS + 1U, HCD_SECTION_MMU_MAPPING_SIZES,
                HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_PER_CPU,
                mapping_offset, sizeof(struct hcd_mmu_mapping_size), 3U);

    put_u32(buffer, (uint32_t)config_offset, HCD_ARCH_AMD64);
    put_u32(buffer, (uint32_t)config_offset + 4U, 0U);
    put_u32(buffer, (uint32_t)config_offset + 8U, 0U);
    put_u32(buffer, (uint32_t)config_offset + 12U, HCD_MMU_REGIME_AMD64);
    put_u32(buffer, (uint32_t)config_offset + 16U,
            HCD_MMU_CONFIG_F_CURRENT |
            HCD_MMU_CONFIG_F_TRANSLATION_ENABLED);
    put_u32(buffer, (uint32_t)config_offset + 20U,
            HCD_MMU_EXCEPTION_LEVEL_NONE);
    put_u32(buffer, (uint32_t)config_offset + 24U, 48U);
    put_u32(buffer, (uint32_t)config_offset + 32U, 48U);
    put_u32(buffer, (uint32_t)config_offset + 36U, 4096U);
    put_u32(buffer, (uint32_t)config_offset + 40U, 4U);
    put_u32(buffer, (uint32_t)config_offset + 44U,
            HCD_MMU_VA_RANGE_AMD64_CANONICAL);
    put_u32(buffer, (uint32_t)config_offset + 48U,
            HCD_MMU_TLB_KNOWN_MASK);
    put_u32(buffer, (uint32_t)config_offset + 52U,
            HCD_MMU_ATTR_KNOWN_MASK);

    put_u32(buffer, (uint32_t)mapping_offset, 0U);
    put_u32(buffer, (uint32_t)mapping_offset + 4U, 0U);
    put_u32(buffer, (uint32_t)mapping_offset + 8U, 1U);
    put_u32(buffer, (uint32_t)mapping_offset + 12U, HCD_MMU_MAPPING_BLOCK);
    put_u64(buffer, (uint32_t)mapping_offset + 16U, UINT64_C(0x40000000));
    mapping_offset += sizeof(struct hcd_mmu_mapping_size);
    put_u32(buffer, (uint32_t)mapping_offset, 0U);
    put_u32(buffer, (uint32_t)mapping_offset + 4U, 0U);
    put_u32(buffer, (uint32_t)mapping_offset + 8U, 2U);
    put_u32(buffer, (uint32_t)mapping_offset + 12U, HCD_MMU_MAPPING_BLOCK);
    put_u64(buffer, (uint32_t)mapping_offset + 16U, UINT64_C(0x200000));
    mapping_offset += sizeof(struct hcd_mmu_mapping_size);
    put_u32(buffer, (uint32_t)mapping_offset, 0U);
    put_u32(buffer, (uint32_t)mapping_offset + 4U, 0U);
    put_u32(buffer, (uint32_t)mapping_offset + 8U, 3U);
    put_u32(buffer, (uint32_t)mapping_offset + 12U, HCD_MMU_MAPPING_PAGE);
    put_u64(buffer, (uint32_t)mapping_offset + 16U, UINT64_C(0x1000));
    return seal_fixture_size(buffer, V2_TOTAL_SIZE);
}

static int test_valid_v2_additive_mmu_profile(void)
{
    uint8_t buffer[V2_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_v2_core_fixture(buffer) != 0) {
        return 1;
    }
    {
        enum hcd_error result = hcd_validate(
            buffer, sizeof(buffer), HCD_PROFILE_CORE,
            &workspace, &detail, &view);
        if (result != HCD_OK) {
#if __STDC_HOSTED__
            (void)fprintf(stderr,
                "HCD v2 error=%u section=%u record=%u offset=%llu\n",
                (unsigned int)result, (unsigned int)detail.section_type,
                (unsigned int)detail.record_index,
                (unsigned long long)detail.offset);
#endif
        return 2;
        }
    }
    if ((view.header.version_major != HCD_VERSION_MAJOR_V2) ||
        (view.size != sizeof(buffer))) {
        return 3;
    }
    return 0;
}

static int test_v2_accepts_amd64_five_level_configuration(void)
{
    uint8_t buffer[V2_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    uint32_t config_offset = (uint32_t)V2_CONFIG_OFFSET;
    uint32_t mapping_offset = (uint32_t)V2_MAPPING_OFFSET;
    uint32_t mapping_directory_entry = CORE_DIRECTORY_OFFSET +
        (CORE_SECTIONS + 1U) * HCD_SECTION_ENTRY_SIZE;

    if (build_v2_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u32(buffer, config_offset + 32U, 57U);
    put_u32(buffer, config_offset + 40U, 5U);
    put_u64(buffer, mapping_offset + 16U, UINT64_C(0x8000000000));
    put_u32(buffer, mapping_offset + 8U, 1U);
    mapping_offset += sizeof(struct hcd_mmu_mapping_size);
    put_u64(buffer, mapping_offset + 16U, UINT64_C(0x40000000));
    put_u32(buffer, mapping_offset + 8U, 2U);
    mapping_offset += sizeof(struct hcd_mmu_mapping_size);
    put_u64(buffer, mapping_offset + 16U, UINT64_C(0x200000));
    put_u32(buffer, mapping_offset + 8U, 3U);
    put_u32(buffer, mapping_offset + 12U, HCD_MMU_MAPPING_BLOCK);
    mapping_offset += sizeof(struct hcd_mmu_mapping_size);
    put_u32(buffer, mapping_offset, 0U);
    put_u32(buffer, mapping_offset + 4U, 0U);
    put_u32(buffer, mapping_offset + 8U, 4U);
    put_u32(buffer, mapping_offset + 12U, HCD_MMU_MAPPING_PAGE);
    put_u64(buffer, mapping_offset + 16U, UINT64_C(0x1000));
    put_u64(buffer, mapping_directory_entry + 16U,
            4U * sizeof(struct hcd_mmu_mapping_size));
    put_u32(buffer, mapping_directory_entry + 28U, 4U);
    if (seal_fixture_size(buffer, sizeof(buffer)) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_OK) {
        return 3;
    }
    return 0;
}

static int test_v2_rejects_invalid_mapping_size(void)
{
    uint8_t buffer[V2_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_v2_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, (uint32_t)V2_MAPPING_OFFSET + 16U,
            UINT64_C(0x300000));
    if (seal_fixture_size(buffer, sizeof(buffer)) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) !=
        HCD_E_SEMANTIC_INCONSISTENCY) {
        return 3;
    }
    if (detail.section_type != HCD_SECTION_MMU_MAPPING_SIZES) {
        return 4;
    }
    return 0;
}

static int expect_v2_semantic_rejection(
    uint8_t *buffer,
    uint64_t size,
    uint32_t expected_section)
{
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (seal_fixture_size(buffer, size) != 0) {
        return 1;
    }
    if (hcd_validate(buffer, size, HCD_PROFILE_CORE,
                     &workspace, &detail, &view) !=
        HCD_E_SEMANTIC_INCONSISTENCY) {
        return 2;
    }
    return (detail.section_type == expected_section) ? 0 : 3;
}

static int test_v2_rejects_invalid_config_and_mapping_semantics(void)
{
    uint8_t buffer[V2_TOTAL_SIZE];
    uint32_t mapping_offset = (uint32_t)V2_MAPPING_OFFSET;

    if (build_v2_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u32(buffer, (uint32_t)V2_CONFIG_OFFSET + 16U, 0U);
    if (expect_v2_semantic_rejection(
            buffer, sizeof(buffer), HCD_SECTION_MMU_TRANSLATION_CONFIGS) != 0) {
        return 2;
    }

    if (build_v2_core_fixture(buffer) != 0) {
        return 3;
    }
    put_u32(buffer, (uint32_t)V2_CONFIG_OFFSET + 48U,
            HCD_MMU_TLB_KNOWN_MASK | UINT32_C(0x80000000));
    if (expect_v2_semantic_rejection(
            buffer, sizeof(buffer), HCD_SECTION_MMU_TRANSLATION_CONFIGS) != 0) {
        return 4;
    }

    if (build_v2_core_fixture(buffer) != 0) {
        return 5;
    }
    put_u32(buffer, mapping_offset + 8U, 2U);
    put_u32(buffer, mapping_offset + sizeof(struct hcd_mmu_mapping_size) + 8U,
            1U);
    if (expect_v2_semantic_rejection(
            buffer, sizeof(buffer), HCD_SECTION_MMU_MAPPING_SIZES) != 0) {
        return 6;
    }

    if (build_v2_core_fixture(buffer) != 0) {
        return 7;
    }
    put_u32(buffer, mapping_offset + 4U, 1U);
    if (expect_v2_semantic_rejection(
            buffer, sizeof(buffer), HCD_SECTION_MMU_MAPPING_SIZES) != 0) {
        return 8;
    }
    return 0;
}

static int test_v2_rejects_duplicate_current_configuration(void)
{
    uint8_t buffer[V2_DUPLICATE_CURRENT_SIZE];
    uint32_t config_offset = (uint32_t)V2_CONFIG_OFFSET;
    uint32_t original_mapping_offset = (uint32_t)V2_MAPPING_OFFSET;
    uint32_t expanded_mapping_offset =
        original_mapping_offset + (uint32_t)sizeof(
            struct hcd_mmu_translation_config);
    uint32_t config_directory_entry = CORE_DIRECTORY_OFFSET +
        CORE_SECTIONS * HCD_SECTION_ENTRY_SIZE;
    uint32_t mapping_directory_entry = config_directory_entry +
        HCD_SECTION_ENTRY_SIZE;
    uint32_t index = 0U;

    if (build_v2_core_fixture(buffer) != 0) {
        return 1;
    }
    for (index = 3U * (uint32_t)sizeof(struct hcd_mmu_mapping_size);
         index > 0U; --index) {
        buffer[expanded_mapping_offset + index - 1U] =
            buffer[original_mapping_offset + index - 1U];
    }
    for (index = 0U;
         index < (uint32_t)sizeof(struct hcd_mmu_translation_config);
         ++index) {
        buffer[config_offset +
               (uint32_t)sizeof(struct hcd_mmu_translation_config) + index] =
            buffer[config_offset + index];
    }
    put_u32(buffer, config_offset +
            (uint32_t)sizeof(struct hcd_mmu_translation_config) + 8U, 1U);
    put_u32(buffer, expanded_mapping_offset + 3U *
            (uint32_t)sizeof(struct hcd_mmu_mapping_size), 0U);
    put_u32(buffer, expanded_mapping_offset + 3U *
            (uint32_t)sizeof(struct hcd_mmu_mapping_size) + 4U, 1U);
    put_u32(buffer, expanded_mapping_offset + 3U *
            (uint32_t)sizeof(struct hcd_mmu_mapping_size) + 8U, 3U);
    put_u32(buffer, expanded_mapping_offset + 3U *
            (uint32_t)sizeof(struct hcd_mmu_mapping_size) + 12U,
            HCD_MMU_MAPPING_PAGE);
    put_u64(buffer, expanded_mapping_offset + 3U *
            (uint32_t)sizeof(struct hcd_mmu_mapping_size) + 16U,
            UINT64_C(0x1000));
    put_u32(buffer, expanded_mapping_offset + 3U *
            (uint32_t)sizeof(struct hcd_mmu_mapping_size) + 24U, 0U);
    put_u32(buffer, expanded_mapping_offset + 3U *
            (uint32_t)sizeof(struct hcd_mmu_mapping_size) + 28U, 0U);
    put_u64(buffer, 16U, V2_DUPLICATE_CURRENT_SIZE);
    put_u64(buffer, config_directory_entry + 16U,
            2U * sizeof(struct hcd_mmu_translation_config));
    put_u32(buffer, config_directory_entry + 28U, 2U);
    put_u64(buffer, mapping_directory_entry + 8U, expanded_mapping_offset);
    put_u64(buffer, mapping_directory_entry + 16U,
            4U * sizeof(struct hcd_mmu_mapping_size));
    put_u32(buffer, mapping_directory_entry + 28U, 4U);
    if (expect_v2_semantic_rejection(
            buffer, sizeof(buffer), HCD_SECTION_MMU_TRANSLATION_CONFIGS) != 0) {
        return 2;
    }
    return 0;
}

static void convert_v2_fixture_to_arm64_stage1(uint8_t *buffer)
{
    uint32_t config_offset = (uint32_t)V2_CONFIG_OFFSET;
    uint32_t mapping_offset = (uint32_t)V2_MAPPING_OFFSET;
    uint32_t upper_config_offset = config_offset +
        (uint32_t)sizeof(struct hcd_mmu_translation_config);
    uint32_t context_offset = (uint32_t)V2_CONTEXT_OFFSET;
    uint32_t mapping_index = 0U;

    put_u32(buffer, 36U, CORE_SECTIONS + 3U);
    put_section(buffer, CORE_SECTIONS,
                HCD_SECTION_MMU_TRANSLATION_CONFIGS,
                HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_PER_CPU,
                config_offset, sizeof(struct hcd_mmu_translation_config), 2U);
    put_section(buffer, CORE_SECTIONS + 1U, HCD_SECTION_MMU_MAPPING_SIZES,
                HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_PER_CPU,
                mapping_offset, sizeof(struct hcd_mmu_mapping_size), 6U);
    put_section(buffer, CORE_SECTIONS + 2U, HCD_SECTION_MMU_ARM64_CONTEXTS,
                HCD_SF_REQUIRED | HCD_SF_ARRAY | HCD_SF_PER_CPU,
                context_offset, sizeof(struct hcd_mmu_arm64_context), 2U);
    put_u32(buffer, 12U, HCD_ARCH_ARM64);
    put_u32(buffer, 32U, HCD_F_EXTERNAL_REFERENCES | HCD_F_INCOMPLETE);
    put_u32(buffer, V2_SHIFTED_OFFSET(CORE_SUMMARY_OFFSET) + 8U,
            HCD_ARCH_ARM64);
    put_u32(buffer, V2_SHIFTED_OFFSET(CORE_CPU_OFFSET) + 32U, HCD_ARCH_ARM64);
    put_u32(buffer, V2_SHIFTED_OFFSET(CORE_FEATURE_OFFSET) + 4U,
            HCD_ARCH_MASK_ARM64);
    put_u32(buffer, V2_SHIFTED_OFFSET(CORE_RAW_OFFSET) + 4U,
            HCD_RAW_SPACE_ARM_ID);
    put_u32(buffer, V2_SHIFTED_OFFSET(CORE_MMU_OFFSET), HCD_ARCH_ARM64);
    put_u32(buffer, V2_SHIFTED_OFFSET(CORE_VIRT_OFFSET), HCD_ARCH_ARM64);
    put_u32(buffer, config_offset, HCD_ARCH_ARM64);
    put_u32(buffer, config_offset + 12U, HCD_MMU_REGIME_ARM64_STAGE1);
    put_u32(buffer, config_offset + 20U, HCD_MMU_EXCEPTION_LEVEL_EL1);
    put_u32(buffer, config_offset + 28U, 0U);
    put_u32(buffer, config_offset + 36U, 16384U);
    put_u32(buffer, config_offset + 44U, HCD_MMU_VA_RANGE_ARM64_LOWER);
    put_u32(buffer, upper_config_offset, HCD_ARCH_ARM64);
    put_u32(buffer, upper_config_offset + 4U, 0U);
    put_u32(buffer, upper_config_offset + 8U, 1U);
    put_u32(buffer, upper_config_offset + 12U, HCD_MMU_REGIME_ARM64_STAGE1);
    put_u32(buffer, upper_config_offset + 16U, 0U);
    put_u32(buffer, upper_config_offset + 20U, HCD_MMU_EXCEPTION_LEVEL_EL1);
    put_u32(buffer, upper_config_offset + 24U, 48U);
    put_u32(buffer, upper_config_offset + 28U, 0U);
    put_u32(buffer, upper_config_offset + 32U, 48U);
    put_u32(buffer, upper_config_offset + 36U, 16384U);
    put_u32(buffer, upper_config_offset + 40U, 4U);
    put_u32(buffer, upper_config_offset + 44U, HCD_MMU_VA_RANGE_ARM64_UPPER);
    put_u64(buffer, mapping_offset + 16U, UINT64_C(0x1000000000));
    put_u64(buffer, mapping_offset + sizeof(struct hcd_mmu_mapping_size) + 16U,
            UINT64_C(0x2000000));
    put_u64(buffer,
            mapping_offset + 2U * sizeof(struct hcd_mmu_mapping_size) + 16U,
            UINT64_C(0x4000));
    for (mapping_index = 0U; mapping_index < 3U; ++mapping_index) {
        uint32_t source_offset = mapping_offset +
            mapping_index * (uint32_t)sizeof(struct hcd_mmu_mapping_size);
        uint32_t target_offset = mapping_offset +
            (mapping_index + 3U) *
                (uint32_t)sizeof(struct hcd_mmu_mapping_size);
        uint32_t byte = 0U;

        for (byte = 0U; byte < sizeof(struct hcd_mmu_mapping_size); ++byte) {
            buffer[target_offset + byte] = buffer[source_offset + byte];
        }
        put_u32(buffer, target_offset + 4U, 1U);
    }
    put_u32(buffer, context_offset, 0U);
    put_u32(buffer, context_offset + 4U, 0U);
    put_u32(buffer, context_offset + 8U, HCD_MMU_ARM64_CONTEXT_TTBR0);
    put_u32(buffer, context_offset + 12U,
            HCD_MMU_ARM64_CONTEXT_F_TRANSLATION_ENABLED);
    put_u32(buffer, context_offset + 16U, HCD_MMU_EXCEPTION_LEVEL_EL1);
    context_offset += sizeof(struct hcd_mmu_arm64_context);
    put_u32(buffer, context_offset, 0U);
    put_u32(buffer, context_offset + 4U, 1U);
    put_u32(buffer, context_offset + 8U, HCD_MMU_ARM64_CONTEXT_TTBR1);
    put_u32(buffer, context_offset + 12U,
            HCD_MMU_ARM64_CONTEXT_F_TRANSLATION_ENABLED);
    put_u32(buffer, context_offset + 16U, HCD_MMU_EXCEPTION_LEVEL_EL1);
}

static int test_v2_accepts_arm64_stage1_configuration(void)
{
    uint8_t buffer[V2_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_v2_core_fixture(buffer) != 0) {
        return 1;
    }
    convert_v2_fixture_to_arm64_stage1(buffer);
    if (seal_fixture_size(buffer, sizeof(buffer)) != 0) {
        return 2;
    }
    {
        enum hcd_error result = hcd_validate(
            buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
            &workspace, &detail, &view);
        if (result != HCD_OK) {
#if __STDC_HOSTED__
            (void)fprintf(stderr,
                "HCD v2 ARM64 error=%u section=%u record=%u offset=%llu\n",
                (unsigned int)result, (unsigned int)detail.section_type,
                (unsigned int)detail.record_index,
                (unsigned long long)detail.offset);
#endif
            return 3;
        }
    }
    return 0;
}

static int test_v2_rejects_arm64_context_inconsistencies(void)
{
    uint8_t buffer[V2_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    uint32_t upper_config_offset = (uint32_t)V2_CONFIG_OFFSET +
        (uint32_t)sizeof(struct hcd_mmu_translation_config);

    if (build_v2_core_fixture(buffer) != 0) {
        return 1;
    }
    convert_v2_fixture_to_arm64_stage1(buffer);
    put_u32(buffer, upper_config_offset + 16U,
            HCD_MMU_CONFIG_F_TRANSLATION_ENABLED);
    if ((seal_fixture_size(buffer, sizeof(buffer)) != 0) ||
        (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
            &workspace, &detail, &view) !=
         HCD_E_SEMANTIC_INCONSISTENCY) ||
        (detail.section_type != HCD_SECTION_MMU_TRANSLATION_CONFIGS)) {
        return 2;
    }

    if (build_v2_core_fixture(buffer) != 0) {
        return 3;
    }
    convert_v2_fixture_to_arm64_stage1(buffer);
    put_u32(buffer, (uint32_t)V2_CONTEXT_OFFSET + 12U, 0U);
    workspace = (struct hcd_validation_workspace){0};
    detail = (struct hcd_validation_error){0};
    view = (struct hcd_validated_view){0};
    if ((seal_fixture_size(buffer, sizeof(buffer)) != 0) ||
        (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_RECOVERY,
            &workspace, &detail, &view) !=
         HCD_E_SEMANTIC_INCONSISTENCY) ||
        (detail.section_type != HCD_SECTION_MMU_ARM64_CONTEXTS)) {
        return 4;
    }
    return 0;
}

static int test_v1_skips_optional_v2_extensions(void)
{
    uint8_t buffer[V2_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    uint32_t first_flags = CORE_DIRECTORY_OFFSET +
        CORE_SECTIONS * HCD_SECTION_ENTRY_SIZE + 6U;
    uint32_t second_flags = first_flags + HCD_SECTION_ENTRY_SIZE;

    if (build_v2_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u16(buffer, 4U, HCD_VERSION_MAJOR_V1);
    put_u16(buffer, first_flags, HCD_SF_ARRAY | HCD_SF_PER_CPU);
    put_u16(buffer, second_flags, HCD_SF_ARRAY | HCD_SF_PER_CPU);
    if (seal_fixture_size(buffer, sizeof(buffer)) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_OK) {
        return 3;
    }
    return 0;
}

static int test_v1_rejects_required_v2_extension(void)
{
    uint8_t buffer[V2_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_v2_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u16(buffer, 4U, HCD_VERSION_MAJOR_V1);
    if (seal_fixture_size(buffer, sizeof(buffer)) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) !=
        HCD_E_SECTION_UNKNOWN_REQUIRED) {
        return 3;
    }
    return 0;
}

static int test_valid_core_profile(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    {
        enum hcd_error result = hcd_validate(buffer, sizeof(buffer),
            HCD_PROFILE_CORE, &workspace, &detail, &view);
        if (result != HCD_OK) {
#if __STDC_HOSTED__
            (void)fprintf(stderr, "core HCD error=%u section=%u record=%u offset=%llu\n",
                (unsigned int)result, (unsigned int)detail.section_type,
                (unsigned int)detail.record_index,
                (unsigned long long)detail.offset);
#endif
            return 2;
        }
    }
    return 0;
}

static int test_core_rejects_invalid_mmu_fields(void)
{
    static const struct {
        uint32_t offset;
        uint32_t value;
    } invalid_fields[] = {
        {8U, 0U},
        {8U, 53U},
        {12U, 0U},
        {12U, 58U},
        {16U, 0U},
        {16U, 8U},
        {20U, 8U},
        {24U, 0U},
        {24U, 32U},
        {28U, 0U},
        {28U, 8U},
        {44U, 1U}
    };
    uint32_t index = 0U;

    for (index = 0U;
         index < (uint32_t)(sizeof(invalid_fields) / sizeof(invalid_fields[0]));
         ++index) {
        uint8_t buffer[CORE_TOTAL_SIZE];
        struct hcd_validation_workspace workspace = {0};
        struct hcd_validation_error detail = {0};
        struct hcd_validated_view view = {0};

        if (build_core_fixture(buffer) != 0) {
            return 1;
        }
        put_u32(buffer, CORE_MMU_OFFSET + invalid_fields[index].offset,
                invalid_fields[index].value);
        if (seal_fixture(buffer) != 0) {
            return 2;
        }
        if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                         &workspace, &detail, &view) !=
            HCD_E_SEMANTIC_INCONSISTENCY) {
            return 3;
        }
        if (detail.section_type != HCD_SECTION_MMU_CAPABILITIES) {
            return 4;
        }
    }
    return 0;
}

static int test_core_rejects_invalid_boot_reservation(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, CORE_BOOT_OFFSET + 32U, UINT64_C(0x300000));
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) !=
        HCD_E_SEMANTIC_INCONSISTENCY) {
        return 3;
    }
    return 0;
}


static int test_core_rejects_topology_parent_cycle(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u32(buffer, CORE_TOPOLOGY_OFFSET + 32U + 4U, 1U);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) !=
        HCD_E_SEMANTIC_INCONSISTENCY) {
        return 3;
    }
    return 0;
}

static int test_core_rejects_controller_outside_mmio(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, CORE_INTERRUPT_OFFSET + 16U, UINT64_C(0xFEF00000));
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) !=
        HCD_E_EXTERNAL_REFERENCE_INVALID) {
        return 3;
    }
    return 0;
}

static int test_core_rejects_unmapped_timer_interrupt(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u32(buffer, CORE_TIMER_OFFSET + 32U, 300U);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) !=
        HCD_E_SEMANTIC_INCONSISTENCY) {
        return 3;
    }
    return 0;
}

static int test_core_rejects_loader_outside_memory_map(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, CORE_BOOT_OFFSET + 16U, UINT64_C(0x300000));
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_E_MEMORY_MAP_INVALID) {
        return 3;
    }
    return 0;
}


static int test_core_rejects_interrupt_parent_cycle(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u32(buffer, CORE_INTERRUPT_OFFSET + 40U, 0U);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) !=
        HCD_E_SEMANTIC_INCONSISTENCY) {
        return 3;
    }
    return 0;
}


static int test_core_rejects_raw_index_mismatch(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u32(buffer, CORE_CPU_OFFSET + 272U, HCD_ID_UNKNOWN);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_E_CPU_FEATURE_SET_INVALID) {
        return 3;
    }
    return 0;
}

static int test_core_rejects_cache_geometry(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, CORE_CACHE_OFFSET + 16U, 1U);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_E_SEMANTIC_INCONSISTENCY) {
        return 3;
    }
    return 0;
}

static int test_core_rejects_virtualization_subset(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u32(buffer, CORE_VIRT_OFFSET + 8U, 2U);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_E_SEMANTIC_INCONSISTENCY) {
        return 3;
    }
    return 0;
}

static int test_core_rejects_firmware_reference_range(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, CORE_FW_OFFSET + 16U, UINT64_C(0x300000));
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_E_EXTERNAL_REFERENCE_INVALID) {
        return 3;
    }
    return 0;
}

static int test_core_rejects_platform_device_range(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u64(buffer, CORE_DEVICE_OFFSET + 16U, UINT64_C(0xFEF00000));
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_E_EXTERNAL_REFERENCE_INVALID) {
        return 3;
    }
    return 0;
}


static int test_core_rejects_memory_region_count_overflow(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    uint64_t memory_directory_entry = CORE_DIRECTORY_OFFSET +
        (UINT64_C(4) * (uint64_t)HCD_SECTION_ENTRY_SIZE);

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u32(buffer, memory_directory_entry + 28U,
            HCD_MAX_MEMORY_REGIONS + 1U);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, sizeof(buffer), HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_E_COUNT_LIMIT) {
        return 3;
    }
    if (detail.section_type != HCD_SECTION_MEMORY_MAP) {
        return 4;
    }
    return 0;
}

int main(void)
{
    if (test_valid_core_profile() != 0) {
        return 1;
    }
    if (test_valid_v2_additive_mmu_profile() != 0) {
        return 2;
    }
    if (test_v2_accepts_amd64_five_level_configuration() != 0) {
        return 3;
    }
    if (test_v2_rejects_invalid_mapping_size() != 0) {
        return 4;
    }
    if (test_v2_rejects_invalid_config_and_mapping_semantics() != 0) {
        return 5;
    }
    if (test_v2_rejects_duplicate_current_configuration() != 0) {
        return 6;
    }
    if (test_v2_accepts_arm64_stage1_configuration() != 0) {
        return 7;
    }
    if (test_v2_rejects_arm64_context_inconsistencies() != 0) {
        return 8;
    }
    if (test_v1_skips_optional_v2_extensions() != 0) {
        return 9;
    }
    if (test_v1_rejects_required_v2_extension() != 0) {
        return 10;
    }
    if (test_core_rejects_invalid_mmu_fields() != 0) {
        return 11;
    }
    if (test_core_rejects_invalid_boot_reservation() != 0) {
        return 12;
    }
    if (test_core_rejects_topology_parent_cycle() != 0) {
        return 13;
    }
    if (test_core_rejects_controller_outside_mmio() != 0) {
        return 14;
    }
    if (test_core_rejects_unmapped_timer_interrupt() != 0) {
        return 14;
    }
    if (test_core_rejects_loader_outside_memory_map() != 0) {
        return 15;
    }
    if (test_core_rejects_interrupt_parent_cycle() != 0) {
        return 16;
    }
    if (test_core_rejects_raw_index_mismatch() != 0) {
        return 17;
    }
    if (test_core_rejects_cache_geometry() != 0) {
        return 16;
    }
    if (test_core_rejects_virtualization_subset() != 0) {
        return 17;
    }
    if (test_core_rejects_firmware_reference_range() != 0) {
        return 18;
    }
    if (test_core_rejects_platform_device_range() != 0) {
        return 19;
    }
    if (test_core_rejects_memory_region_count_overflow() != 0) {
        return 20;
    }
    return 0;
}
