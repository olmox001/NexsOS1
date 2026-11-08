#ifndef NXEC_HCD_WIRE_H
#define NXEC_HCD_WIRE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Nxec HCD v1 wire contract.
 * All serialized scalars are little-endian. These structures contain no
 * native pointers, size_t, long, bool, bitfields, or packed attributes.
 * Input must be decoded with checked endian accessors before dereference.
 */

#define HCD_MAGIC UINT32_C(0x31444348)
#define HCD_VERSION_MAJOR UINT16_C(1)
#define HCD_VERSION_MAJOR_V1 UINT16_C(1)
#define HCD_VERSION_MAJOR_V2 UINT16_C(2)
#define HCD_VERSION_MINOR UINT16_C(0)
#define HCD_HEADER_SIZE UINT32_C(64)
#define HCD_SECTION_ENTRY_SIZE UINT32_C(40)
#define HCD_MAX_SECTIONS UINT32_C(64)
#define HCD_MAX_MEMORY_REGIONS UINT32_C(4096)
#define HCD_MAX_CPUS UINT32_C(65536)
#define HCD_MAX_TOTAL_SIZE UINT64_C(67108864)
#define HCD_FEATURE_WORDS UINT32_C(8)
#define HCD_FEATURE_BITS UINT32_C(512)
#define HCD_ID_UNKNOWN UINT32_C(0xFFFFFFFF)

#define HCD_F_INCOMPLETE UINT32_C(0x00000001)
#define HCD_F_HETEROGENEOUS_CPUS UINT32_C(0x00000002)
#define HCD_F_EXTERNAL_REFERENCES UINT32_C(0x00000004)
#define HCD_F_FIRMWARE_BOOT_SERVICES_ACTIVE UINT32_C(0x00000008)
#define HCD_F_EFI_EXIT_BOOT_SERVICES_DONE UINT32_C(0x00000010)
#define HCD_F_SECURE_BOOT_REPORTED UINT32_C(0x00000020)
#define HCD_F_KNOWN_MASK UINT32_C(0x0000003F)

#define HCD_SF_REQUIRED UINT16_C(0x0001)
#define HCD_SF_ARRAY UINT16_C(0x0002)
#define HCD_SF_EXTERNAL UINT16_C(0x0004)
#define HCD_SF_PER_CPU UINT16_C(0x0008)
#define HCD_SF_KNOWN_MASK UINT16_C(0x000F)

#define HCD_CPU_F_PRESENT UINT32_C(0x00000001)
#define HCD_CPU_F_BOOT UINT32_C(0x00000002)
#define HCD_CPU_F_ONLINE UINT32_C(0x00000004)
#define HCD_CPU_F_SCHEDULABLE UINT32_C(0x00000008)
#define HCD_CPU_F_KNOWN_MASK UINT32_C(0x0000000F)

#define HCD_CPU_SUMMARY_F_HETEROGENEOUS UINT32_C(0x00000001)
#define HCD_CPU_SUMMARY_F_KNOWN_MASK UINT32_C(0x00000001)

#define HCD_ARCH_MASK_AMD64 (UINT32_C(1) << (uint32_t)HCD_ARCH_AMD64)
#define HCD_ARCH_MASK_ARM64 (UINT32_C(1) << (uint32_t)HCD_ARCH_ARM64)

enum hcd_architecture {
    HCD_ARCH_AMD64 = 1,
    HCD_ARCH_ARM64 = 2
};

enum hcd_section_type {
    HCD_SECTION_CPU_SUMMARY = 1,
    HCD_SECTION_CPU_RECORDS = 2,
    HCD_SECTION_CPU_FEATURE_CATALOG = 3,
    HCD_SECTION_TOPOLOGY = 4,
    HCD_SECTION_CACHE = 5,
    HCD_SECTION_MEMORY_MAP = 6,
    HCD_SECTION_INTERRUPT_CONTROLLERS = 7,
    HCD_SECTION_VECTOR_LAYOUT = 8,
    HCD_SECTION_MMU_CAPABILITIES = 9,
    HCD_SECTION_VIRTUALIZATION = 10,
    HCD_SECTION_FIRMWARE_REFERENCES = 11,
    HCD_SECTION_BOOT_CONTEXT = 12,
    HCD_SECTION_TIMER_CAPABILITIES = 13,
    HCD_SECTION_PLATFORM_DEVICES = 14,
    HCD_SECTION_CPU_RAW_REGISTERS = 15,
    HCD_SECTION_MMU_TRANSLATION_CONFIGS = 16,
    HCD_SECTION_MMU_MAPPING_SIZES = 17,
    HCD_SECTION_MMU_ARM64_CONTEXTS = 18
};

enum hcd_mmu_translation_regime {
    HCD_MMU_REGIME_AMD64 = 1,
    HCD_MMU_REGIME_ARM64_STAGE1 = 2,
    HCD_MMU_REGIME_ARM64_STAGE2 = 3
};

#define HCD_MMU_CONFIG_F_CURRENT UINT32_C(0x00000001)
#define HCD_MMU_CONFIG_F_TRANSLATION_ENABLED UINT32_C(0x00000002)
#define HCD_MMU_CONFIG_F_KNOWN_MASK UINT32_C(0x00000003)

enum hcd_mmu_exception_level {
    HCD_MMU_EXCEPTION_LEVEL_NONE = 0,
    HCD_MMU_EXCEPTION_LEVEL_EL1 = 1,
    HCD_MMU_EXCEPTION_LEVEL_EL2 = 2
};

enum hcd_mmu_virtual_address_range {
    HCD_MMU_VA_RANGE_AMD64_CANONICAL = 1,
    HCD_MMU_VA_RANGE_ARM64_LOWER = 2,
    HCD_MMU_VA_RANGE_ARM64_UPPER = 3,
    HCD_MMU_VA_RANGE_ARM64_STAGE2 = 4
};

enum hcd_mmu_arm64_context_kind {
    HCD_MMU_ARM64_CONTEXT_TTBR0 = 1,
    HCD_MMU_ARM64_CONTEXT_TTBR1 = 2
};

#define HCD_MMU_ARM64_CONTEXT_F_TRANSLATION_ENABLED UINT32_C(0x00000001)
#define HCD_MMU_ARM64_CONTEXT_F_KNOWN_MASK UINT32_C(0x00000001)

#define HCD_MMU_TLB_INVALIDATE_PAGE UINT32_C(0x00000001)
#define HCD_MMU_TLB_INVALIDATE_CONTEXT UINT32_C(0x00000002)
#define HCD_MMU_TLB_INVALIDATE_GLOBAL UINT32_C(0x00000004)
#define HCD_MMU_TLB_INVALIDATE_RANGE UINT32_C(0x00000008)
#define HCD_MMU_TLB_INVALIDATE_BROADCAST UINT32_C(0x00000010)
#define HCD_MMU_TLB_KNOWN_MASK UINT32_C(0x0000001F)

#define HCD_MMU_ATTR_NORMAL_WB UINT32_C(0x00000001)
#define HCD_MMU_ATTR_NORMAL_WT UINT32_C(0x00000002)
#define HCD_MMU_ATTR_NORMAL_WC UINT32_C(0x00000004)
#define HCD_MMU_ATTR_NORMAL_UC UINT32_C(0x00000008)
#define HCD_MMU_ATTR_DEVICE UINT32_C(0x00000010)
#define HCD_MMU_ATTR_INNER_SHAREABLE UINT32_C(0x00000020)
#define HCD_MMU_ATTR_OUTER_SHAREABLE UINT32_C(0x00000040)
#define HCD_MMU_ATTR_NON_SHAREABLE UINT32_C(0x00000080)
#define HCD_MMU_ATTR_EXECUTE_NEVER UINT32_C(0x00000100)
#define HCD_MMU_ATTR_KNOWN_MASK UINT32_C(0x000001FF)

enum hcd_mmu_mapping_kind {
    HCD_MMU_MAPPING_PAGE = 1,
    HCD_MMU_MAPPING_BLOCK = 2
};

enum hcd_boot_protocol {
    HCD_BOOT_MULTIBOOT2 = 1,
    HCD_BOOT_UEFI_APPLICATION = 2
};

enum hcd_boot_mode {
    HCD_BOOT_MODE_BIOS = 1,
    HCD_BOOT_MODE_UEFI = 2
};

enum hcd_entry_cpu_state {
    HCD_ENTRY_AMD64_PROTECTED32 = 1,
    HCD_ENTRY_AMD64_LONG64 = 2,
    HCD_ENTRY_ARM64_EL1 = 3,
    HCD_ENTRY_ARM64_EL2 = 4
};

enum hcd_topology_level {
    HCD_TOPOLOGY_PACKAGE = 1,
    HCD_TOPOLOGY_DIE = 2,
    HCD_TOPOLOGY_CLUSTER = 3,
    HCD_TOPOLOGY_CORE = 4,
    HCD_TOPOLOGY_THREAD = 5
};

enum hcd_interrupt_controller_kind {
    HCD_INTERRUPT_XAPIC = 1,
    HCD_INTERRUPT_X2APIC = 2,
    HCD_INTERRUPT_GICV2 = 3,
    HCD_INTERRUPT_GICV3 = 4,
    HCD_INTERRUPT_GICV4 = 5,
    HCD_INTERRUPT_PIC = 6,
    HCD_INTERRUPT_IMSIC = 7
};

enum hcd_vector_kind {
    HCD_VECTOR_EXCEPTION = 1,
    HCD_VECTOR_INTERRUPT = 2,
    HCD_VECTOR_IPI = 3,
    HCD_VECTOR_RESERVED = 4
};

enum hcd_timer_kind {
    HCD_TIMER_TSC = 1,
    HCD_TIMER_HPET = 2,
    HCD_TIMER_APIC = 3,
    HCD_TIMER_ARM_GENERIC = 4,
    HCD_TIMER_ARM_ARCH = 5,
    HCD_TIMER_PLATFORM = 6
};

enum hcd_raw_access_state {
    HCD_RAW_ACCESS_READ = 1,
    HCD_RAW_ACCESS_UNIMPLEMENTED = 2,
    HCD_RAW_ACCESS_UNAVAILABLE = 3,
    HCD_RAW_ACCESS_FAULT = 4
};

enum hcd_raw_register_space {
    HCD_RAW_SPACE_CPUID = 1,
    HCD_RAW_SPACE_MSR = 2,
    HCD_RAW_SPACE_ARM_ID = 3,
    HCD_RAW_SPACE_ARM_SYSREG = 4,
    HCD_RAW_SPACE_PCI_CONFIG = 5,
    HCD_RAW_SPACE_FIRMWARE = 6
};

enum hcd_platform_device_kind {
    HCD_DEVICE_PCI = 1,
    HCD_DEVICE_ACPI = 2,
    HCD_DEVICE_DEVICETREE = 3,
    HCD_DEVICE_VIRTIO = 4,
    HCD_DEVICE_USB = 5,
    HCD_DEVICE_STORAGE = 6,
    HCD_DEVICE_NETWORK = 7,
    HCD_DEVICE_OTHER = 8
};

enum hcd_memory_type {
    HCD_MEMORY_USABLE = 1,
    HCD_MEMORY_RESERVED = 2,
    HCD_MEMORY_FIRMWARE_RUNTIME = 3,
    HCD_MEMORY_ACPI_RECLAIMABLE = 4,
    HCD_MEMORY_ACPI_NVS = 5,
    HCD_MEMORY_MMIO = 6,
    HCD_MEMORY_PERSISTENT = 7,
    HCD_MEMORY_UNUSABLE = 8,
    HCD_MEMORY_BOOTSTRAP = 9,
    HCD_MEMORY_UNACCEPTED = 10
};

#define HCD_MEMORY_A_READ UINT32_C(0x00000001)
#define HCD_MEMORY_A_WRITE UINT32_C(0x00000002)
#define HCD_MEMORY_A_EXECUTE UINT32_C(0x00000004)
#define HCD_MEMORY_A_CACHE_UC UINT32_C(0x00000008)
#define HCD_MEMORY_A_CACHE_WC UINT32_C(0x00000010)
#define HCD_MEMORY_A_CACHE_WT UINT32_C(0x00000020)
#define HCD_MEMORY_A_CACHE_WB UINT32_C(0x00000040)
#define HCD_MEMORY_A_ENCRYPTED UINT32_C(0x00000080)
#define HCD_MEMORY_A_PERSISTENT UINT32_C(0x00000100)
#define HCD_MEMORY_A_HOTPLUG UINT32_C(0x00000200)
#define HCD_MEMORY_A_DMA UINT32_C(0x00000400)
#define HCD_MEMORY_A_RUNTIME UINT32_C(0x00000800)
#define HCD_MEMORY_A_KNOWN_MASK UINT32_C(0x00000FFF)

#define HCD_MAX_INTERRUPT_CONTROLLERS 256U
#define HCD_MAX_MMU_CONFIGURATIONS (HCD_MAX_CPUS * UINT32_C(16))
#define HCD_MAX_MMU_MAPPING_SIZES (HCD_MAX_CPUS * UINT32_C(32))

enum hcd_error {
    HCD_OK = 0,
    HCD_E_NULL_INPUT = 1,
    HCD_E_BAD_MAGIC = 2,
    HCD_E_UNSUPPORTED_MAJOR = 3,
    HCD_E_UNSUPPORTED_MINOR_REQUIRED = 4,
    HCD_E_BAD_HEADER_SIZE = 5,
    HCD_E_BAD_TOTAL_SIZE = 6,
    HCD_E_BAD_CHECKSUM = 7,
    HCD_E_BAD_SECTION_DIRECTORY = 8,
    HCD_E_SECTION_DUPLICATE = 9,
    HCD_E_SECTION_UNKNOWN_REQUIRED = 10,
    HCD_E_SECTION_BOUNDS = 11,
    HCD_E_SECTION_OVERLAP = 12,
    HCD_E_ALIGNMENT = 13,
    HCD_E_RESERVED_NONZERO = 14,
    HCD_E_COUNT_LIMIT = 15,
    HCD_E_INTEGER_OVERFLOW = 16,
    HCD_E_SEMANTIC_INCONSISTENCY = 17,
    HCD_E_CPU_FEATURE_SET_INVALID = 18,
    HCD_E_MEMORY_MAP_INVALID = 19,
    HCD_E_EXTERNAL_REFERENCE_INVALID = 20,
    HCD_E_FIRMWARE_TABLE_INVALID = 21,
    HCD_E_INCOMPLETE_NOT_ALLOWED = 22,
    HCD_E_PROFILE_MISMATCH = 23,
    HCD_E_SECTION_NOT_FOUND = 24
};

struct hcd_header {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t header_size;
    uint32_t architecture;
    uint64_t total_size;
    uint64_t checksum;
    uint32_t flags;
    uint32_t section_count;
    uint64_t section_table_offset;
    uint32_t producer_id;
    uint32_t producer_version;
    uint64_t reserved0;
};

struct hcd_section_entry {
    uint32_t type;
    uint16_t version;
    uint16_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t entry_size;
    uint32_t entry_count;
    uint32_t alignment;
    uint32_t reserved0;
};

struct hcd_cpu_summary {
    uint32_t cpu_count;
    uint32_t boot_cpu_id;
    uint32_t architecture;
    uint32_t flags;
    uint32_t feature_word_count;
    uint32_t reserved0;
    uint64_t supported[HCD_FEATURE_WORDS];
    uint64_t enabled[HCD_FEATURE_WORDS];
    uint64_t usable[HCD_FEATURE_WORDS];
    uint64_t reserved1;
};

struct hcd_cpu_record {
    uint32_t logical_cpu_id;
    uint32_t package_id;
    uint32_t die_id;
    uint32_t cluster_id;
    uint32_t core_id;
    uint32_t thread_id;
    uint32_t numa_node_id;
    uint32_t flags;
    uint32_t architecture;
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint8_t vendor[16];
    uint64_t raw_identity0;
    uint64_t raw_identity1;
    uint64_t supported[HCD_FEATURE_WORDS];
    uint64_t enabled[HCD_FEATURE_WORDS];
    uint64_t usable[HCD_FEATURE_WORDS];
    uint32_t raw_record_index;
    uint32_t reserved0;
    uint64_t reserved1;
};

struct hcd_cpu_feature_record {
    uint32_t feature_id;
    uint32_t architecture_mask;
    uint32_t flags;
    uint32_t source_register;
    uint32_t source_selector;
    uint32_t source_subselector;
    uint32_t source_field;
    uint32_t reserved0;
};

struct hcd_cpu_raw_register_record {
    uint32_t logical_cpu_id;
    uint32_t register_space;
    uint32_t selector;
    uint32_t subselector;
    uint64_t value;
    uint32_t access_state;
    uint32_t reserved0;
};

struct hcd_topology_record {
    uint32_t node_id;
    uint32_t parent_node_id;
    uint32_t logical_cpu_id;
    uint32_t level;
    uint32_t flags;
    uint32_t source_id;
    uint64_t reserved1;
};

struct hcd_cache_record {
    uint32_t logical_cpu_id;
    uint32_t cache_id;
    uint16_t level;
    uint16_t type;
    uint32_t flags;
    uint64_t size_bytes;
    uint32_t line_size;
    uint32_t ways;
    uint32_t sets;
    uint32_t sharing_domain;
    uint32_t source_id;
    uint32_t reserved0;
};

struct hcd_memory_record {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attributes;
    uint32_t numa_node_id;
    uint32_t owner;
    uint64_t reserved0;
};

struct hcd_interrupt_controller_record {
    uint32_t controller_id;
    uint32_t kind;
    uint32_t flags;
    uint32_t source_id;
    uint64_t physical_address;
    uint64_t register_size;
    uint32_t interrupt_base;
    uint32_t interrupt_count;
    uint32_t parent_id;
    uint32_t numa_node_id;
    uint32_t version;
    uint32_t trigger_modes;
    uint32_t polarity_modes;
    uint32_t reserved0;
};

struct hcd_vector_record {
    uint32_t vector_id;
    uint32_t type;
    uint32_t flags;
    uint32_t owner_cpu;
    uint32_t source_id;
    uint32_t reserved0;
    uint64_t reserved1;
};

struct hcd_mmu_capabilities {
    uint32_t architecture;
    uint32_t flags;
    uint32_t physical_address_bits;
    uint32_t virtual_address_bits;
    uint32_t page_granule_mask;
    uint32_t block_granule_mask;
    uint32_t levels_supported;
    uint32_t levels_active;
    uint32_t tlb_features;
    uint32_t cache_attributes;
    uint32_t translation_regime;
    uint32_t logical_cpu_id;
    uint64_t raw_register0;
    uint64_t raw_register1;
};

struct hcd_mmu_translation_config {
    uint32_t architecture;
    uint32_t logical_cpu_id;
    uint32_t configuration_id;
    uint32_t translation_regime;
    uint32_t flags;
    uint32_t exception_level;
    uint32_t physical_address_bits;
    uint32_t intermediate_address_bits;
    uint32_t virtual_address_bits;
    uint32_t page_granule_bytes;
    uint32_t level_count;
    uint32_t virtual_address_range;
    uint32_t tlb_features;
    uint32_t memory_attributes;
    uint32_t reserved0;
    uint32_t reserved1;
};

struct hcd_mmu_mapping_size {
    uint32_t logical_cpu_id;
    uint32_t configuration_id;
    uint32_t level_index;
    uint32_t mapping_kind;
    uint64_t size_bytes;
    uint32_t flags;
    uint32_t reserved0;
};

struct hcd_mmu_arm64_context {
    uint32_t logical_cpu_id;
    uint32_t configuration_id;
    uint32_t context_kind;
    uint32_t flags;
    uint32_t exception_level;
    uint32_t reserved0;
};

struct hcd_virtualization_record {
    uint32_t architecture;
    uint32_t features_supported;
    uint32_t features_enabled;
    uint32_t flags;
    uint32_t privilege_level;
    uint32_t hypervisor_present;
    uint32_t vendor_id;
    uint32_t logical_cpu_id;
};

struct hcd_firmware_reference {
    uint32_t kind;
    uint32_t revision;
    uint32_t flags;
    uint32_t memory_type;
    uint64_t physical_address;
    uint64_t length;
    uint64_t table_checksum;
    uint32_t source_id;
    uint32_t reserved0;
};

struct hcd_boot_context {
    uint32_t boot_protocol;
    uint32_t boot_mode;
    uint32_t entry_cpu_state;
    uint32_t flags;
    uint64_t loader_base;
    uint64_t loader_size;
    uint64_t hcd_physical_address;
    uint64_t hcd_reserved_base;
    uint64_t hcd_reserved_size;
    uint32_t firmware_revision;
    uint32_t reserved0;
};

struct hcd_timer_record {
    uint32_t timer_id;
    uint32_t kind;
    uint32_t flags;
    uint32_t counter_width;
    uint64_t frequency_hz;
    uint64_t physical_address;
    uint32_t interrupt_id;
    uint32_t logical_cpu_id;
    uint32_t accuracy_ppm;
    uint32_t reserved0;
};

struct hcd_platform_device_record {
    uint32_t device_id;
    uint32_t kind;
    uint32_t flags;
    uint32_t source_id;
    uint64_t physical_address;
    uint64_t register_size;
    uint32_t interrupt_id;
    uint32_t numa_node_id;
    uint32_t vendor_id;
    uint32_t device_code;
    uint32_t class_code;
    uint32_t revision;
    uint32_t reserved0;
    uint32_t reserved1;
};

_Static_assert(sizeof(struct hcd_header) == 64U, "HCD header size");
_Static_assert(_Alignof(struct hcd_header) == 8U, "HCD header alignment");
_Static_assert(offsetof(struct hcd_header, total_size) == 16U, "HCD total_size offset");
_Static_assert(offsetof(struct hcd_header, checksum) == 24U, "HCD checksum offset");
_Static_assert(offsetof(struct hcd_header, section_table_offset) == 40U, "HCD directory offset");
_Static_assert(sizeof(struct hcd_section_entry) == 40U, "HCD section entry size");
_Static_assert(_Alignof(struct hcd_section_entry) == 8U, "HCD section entry alignment");
_Static_assert(sizeof(struct hcd_cpu_summary) == 224U, "HCD CPU summary size");
_Static_assert(sizeof(struct hcd_cpu_record) == 288U, "HCD CPU record size");
_Static_assert(sizeof(struct hcd_cpu_feature_record) == 32U, "HCD CPU feature record size");
_Static_assert(sizeof(struct hcd_cpu_raw_register_record) == 32U, "HCD raw register record size");
_Static_assert(sizeof(struct hcd_topology_record) == 32U, "HCD topology record size");
_Static_assert(sizeof(struct hcd_cache_record) == 48U, "HCD cache record size");
_Static_assert(sizeof(struct hcd_memory_record) == 40U, "HCD memory record size");
_Static_assert(sizeof(struct hcd_interrupt_controller_record) == 64U, "HCD interrupt controller record size");
_Static_assert(sizeof(struct hcd_vector_record) == 32U, "HCD vector record size");
_Static_assert(sizeof(struct hcd_mmu_capabilities) == 64U, "HCD MMU record size");
_Static_assert(sizeof(struct hcd_mmu_translation_config) == 64U,
               "HCD MMU translation config size");
_Static_assert(offsetof(struct hcd_mmu_translation_config, page_granule_bytes) == 36U,
               "HCD MMU config page granule offset");
_Static_assert(offsetof(struct hcd_mmu_translation_config, tlb_features) == 48U,
               "HCD MMU config TLB features offset");
_Static_assert(sizeof(struct hcd_mmu_mapping_size) == 32U,
               "HCD MMU mapping size record size");
_Static_assert(offsetof(struct hcd_mmu_mapping_size, size_bytes) == 16U,
               "HCD MMU mapping size offset");
_Static_assert(sizeof(struct hcd_mmu_arm64_context) == 24U,
               "HCD ARM64 MMU context record size");
_Static_assert(sizeof(struct hcd_virtualization_record) == 32U, "HCD virtualization record size");
_Static_assert(sizeof(struct hcd_firmware_reference) == 48U, "HCD firmware reference size");
_Static_assert(sizeof(struct hcd_boot_context) == 64U, "HCD boot context size");
_Static_assert(sizeof(struct hcd_timer_record) == 48U, "HCD timer record size");
_Static_assert(sizeof(struct hcd_platform_device_record) == 64U, "HCD platform device record size");

#endif
