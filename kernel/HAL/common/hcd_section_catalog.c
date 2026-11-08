#include <stdint.h>

#include "hcd_section_catalog.h"

#include "hcd_wire.h"

uint32_t hcd_section_expected_entry_size(uint32_t type)
{
    switch (type) {
        case HCD_SECTION_CPU_SUMMARY:
            return (uint32_t)sizeof(struct hcd_cpu_summary);
        case HCD_SECTION_CPU_RECORDS:
            return (uint32_t)sizeof(struct hcd_cpu_record);
        case HCD_SECTION_CPU_FEATURE_CATALOG:
            return (uint32_t)sizeof(struct hcd_cpu_feature_record);
        case HCD_SECTION_CPU_RAW_REGISTERS:
            return (uint32_t)sizeof(struct hcd_cpu_raw_register_record);
        case HCD_SECTION_TOPOLOGY:
            return (uint32_t)sizeof(struct hcd_topology_record);
        case HCD_SECTION_CACHE:
            return (uint32_t)sizeof(struct hcd_cache_record);
        case HCD_SECTION_MEMORY_MAP:
            return (uint32_t)sizeof(struct hcd_memory_record);
        case HCD_SECTION_INTERRUPT_CONTROLLERS:
            return (uint32_t)sizeof(struct hcd_interrupt_controller_record);
        case HCD_SECTION_VECTOR_LAYOUT:
            return (uint32_t)sizeof(struct hcd_vector_record);
        case HCD_SECTION_MMU_CAPABILITIES:
            return (uint32_t)sizeof(struct hcd_mmu_capabilities);
        case HCD_SECTION_MMU_TRANSLATION_CONFIGS:
            return (uint32_t)sizeof(struct hcd_mmu_translation_config);
        case HCD_SECTION_MMU_MAPPING_SIZES:
            return (uint32_t)sizeof(struct hcd_mmu_mapping_size);
        case HCD_SECTION_MMU_ARM64_CONTEXTS:
            return (uint32_t)sizeof(struct hcd_mmu_arm64_context);
        case HCD_SECTION_VIRTUALIZATION:
            return (uint32_t)sizeof(struct hcd_virtualization_record);
        case HCD_SECTION_FIRMWARE_REFERENCES:
            return (uint32_t)sizeof(struct hcd_firmware_reference);
        case HCD_SECTION_BOOT_CONTEXT:
            return (uint32_t)sizeof(struct hcd_boot_context);
        case HCD_SECTION_TIMER_CAPABILITIES:
            return (uint32_t)sizeof(struct hcd_timer_record);
        case HCD_SECTION_PLATFORM_DEVICES:
            return (uint32_t)sizeof(struct hcd_platform_device_record);
        default:
            return 0U;
    }
}

uint32_t hcd_section_expected_entry_size_for_version(
    uint32_t type,
    uint16_t version_major)
{
    if ((version_major != HCD_VERSION_MAJOR_V2) &&
        ((type == HCD_SECTION_MMU_TRANSLATION_CONFIGS) ||
         (type == HCD_SECTION_MMU_MAPPING_SIZES) ||
         (type == HCD_SECTION_MMU_ARM64_CONTEXTS))) {
        return 0U;
    }
    return hcd_section_expected_entry_size(type);
}

int hcd_section_is_array(uint32_t type)
{
    return (type != HCD_SECTION_CPU_SUMMARY) &&
           (type != HCD_SECTION_BOOT_CONTEXT) &&
           (hcd_section_expected_entry_size(type) != 0U);
}

int hcd_section_is_per_cpu(uint32_t type)
{
    switch (type) {
        case HCD_SECTION_CPU_RECORDS:
        case HCD_SECTION_CPU_RAW_REGISTERS:
        case HCD_SECTION_TOPOLOGY:
        case HCD_SECTION_CACHE:
        case HCD_SECTION_VECTOR_LAYOUT:
        case HCD_SECTION_MMU_CAPABILITIES:
        case HCD_SECTION_MMU_TRANSLATION_CONFIGS:
        case HCD_SECTION_MMU_MAPPING_SIZES:
        case HCD_SECTION_MMU_ARM64_CONTEXTS:
        case HCD_SECTION_VIRTUALIZATION:
            return 1;
        default:
            return 0;
    }
}

int hcd_section_is_external(uint32_t type)
{
    return (type == HCD_SECTION_INTERRUPT_CONTROLLERS) ||
           (type == HCD_SECTION_FIRMWARE_REFERENCES) ||
           (type == HCD_SECTION_PLATFORM_DEVICES);
}
