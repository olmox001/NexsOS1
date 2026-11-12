#ifndef NXEC_PREBOOT_ISA_H
#define NXEC_PREBOOT_ISA_H

#include <stdint.h>

#include "../common/hcd_wire.h"

#define PREBOOT_ISA_ABI_VERSION UINT32_C(1)
#define PREBOOT_ISA_MAX_RAW_REGISTERS UINT32_C(320)
#define PREBOOT_ISA_MAX_CACHE_SUBLEAVES UINT32_C(16)
#define PREBOOT_ISA_MAX_TOPOLOGY_SUBLEAVES UINT32_C(8)
#define PREBOOT_ISA_MAX_FEATURE_SUBLEAVES UINT32_C(16)

#define PREBOOT_ISA_ARCH_AMD64 UINT32_C(1)
#define PREBOOT_ISA_ARCH_ARM64 UINT32_C(2)

#define PREBOOT_ISA_GRANULE_4K UINT32_C(0x00000001)
#define PREBOOT_ISA_GRANULE_16K UINT32_C(0x00000002)
#define PREBOOT_ISA_GRANULE_64K UINT32_C(0x00000004)
#define PREBOOT_ISA_GRANULE_2M UINT32_C(0x00000008)
#define PREBOOT_ISA_GRANULE_1G UINT32_C(0x00000010)

#define PREBOOT_ISA_STATE_MMU_ENABLED UINT32_C(0x00000001)
#define PREBOOT_ISA_STATE_INTERRUPTS_ENABLED UINT32_C(0x00000002)
#define PREBOOT_ISA_STATE_HYPERVISOR_SUPPORTED UINT32_C(0x00000004)
#define PREBOOT_ISA_STATE_GUEST_EXECUTION UINT32_C(0x00000008)
#define PREBOOT_ISA_STATE_NX_ENABLED UINT32_C(0x00000010)
#define PREBOOT_ISA_STATE_LA57_ENABLED UINT32_C(0x00000020)
#define PREBOOT_ISA_STATE_SVE_SUPPORTED UINT32_C(0x00000040)
#define PREBOOT_ISA_STATE_HYPERVISOR_EL_ACTIVE UINT32_C(0x00000080)
#define PREBOOT_ISA_STATE_KNOWN_MASK UINT32_C(0x000000FF)
#define PREBOOT_ISA_GRANULE_KNOWN_MASK UINT32_C(0x0000001F)

#define PREBOOT_ISA_REGSPACE_X86_CPUID_EAX UINT32_C(1)
#define PREBOOT_ISA_REGSPACE_X86_CPUID_EBX UINT32_C(2)
#define PREBOOT_ISA_REGSPACE_X86_CPUID_ECX UINT32_C(3)
#define PREBOOT_ISA_REGSPACE_X86_CPUID_EDX UINT32_C(4)
#define PREBOOT_ISA_REGSPACE_X86_MSR UINT32_C(5)
#define PREBOOT_ISA_REGSPACE_X86_CONTROL UINT32_C(6)
#define PREBOOT_ISA_REGSPACE_ARM_SYSREG UINT32_C(16)

#define PREBOOT_ISA_ACCESS_READ UINT32_C(1)
#define PREBOOT_ISA_ACCESS_UNAVAILABLE UINT32_C(2)

enum preboot_amd64_control_register {
    PREBOOT_AMD64_REG_CR0 = 0,
    PREBOOT_AMD64_REG_RFLAGS = 1,
    PREBOOT_AMD64_REG_CR4 = 4
};

enum preboot_arm64_system_register {
    PREBOOT_ARM64_REG_MIDR_EL1 = 1,
    PREBOOT_ARM64_REG_MPIDR_EL1 = 2,
    PREBOOT_ARM64_REG_CURRENT_EL = 3,
    PREBOOT_ARM64_REG_PFR0_EL1 = 4,
    PREBOOT_ARM64_REG_ISAR0_EL1 = 5,
    PREBOOT_ARM64_REG_MMFR0_EL1 = 6,
    PREBOOT_ARM64_REG_MMFR1_EL1 = 7,
    PREBOOT_ARM64_REG_MMFR2_EL1 = 8,
    PREBOOT_ARM64_REG_CTR_EL0 = 9,
    PREBOOT_ARM64_REG_CNTFRQ_EL0 = 10,
    PREBOOT_ARM64_REG_DAIF = 11,
    PREBOOT_ARM64_REG_SCTLR = 12,
    PREBOOT_ARM64_REG_TCR = 13,
    PREBOOT_ARM64_REG_MAIR = 14,
    PREBOOT_ARM64_REG_PC = 15
};

#define PREBOOT_ISA_MMU_MAX_CONFIGURATIONS UINT32_C(6)
#define PREBOOT_ISA_MMU_MAX_MAPPING_SIZES UINT32_C(24)
#define PREBOOT_ISA_MMU_MAX_ARM64_CONTEXTS UINT32_C(6)

enum preboot_isa_mmu_error {
    PREBOOT_ISA_MMU_OK = 0,
    PREBOOT_ISA_MMU_E_NULL_INPUT = 1,
    PREBOOT_ISA_MMU_E_INVALID_SNAPSHOT = 2,
    PREBOOT_ISA_MMU_E_RAW_REGISTER_MISSING = 3,
    PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE = 4,
    PREBOOT_ISA_MMU_E_OUTPUT_RANGE = 5,
    PREBOOT_ISA_MMU_E_CAPACITY = 6
};

struct preboot_isa_mmu_profile {
    struct hcd_mmu_capabilities legacy;
    struct hcd_mmu_translation_config
        configurations[PREBOOT_ISA_MMU_MAX_CONFIGURATIONS];
    struct hcd_mmu_mapping_size
        mapping_sizes[PREBOOT_ISA_MMU_MAX_MAPPING_SIZES];
    struct hcd_mmu_arm64_context
        arm64_contexts[PREBOOT_ISA_MMU_MAX_ARM64_CONTEXTS];
    uint32_t configuration_count;
    uint32_t mapping_size_count;
    uint32_t arm64_context_count;
};

enum preboot_isa_error {
    PREBOOT_ISA_OK = 0,
    PREBOOT_ISA_E_NULL_INPUT = 1,
    PREBOOT_ISA_E_BAD_ABI = 2,
    PREBOOT_ISA_E_BAD_ARCH = 3,
    PREBOOT_ISA_E_PRIVILEGE = 4,
    PREBOOT_ISA_E_UNSUPPORTED = 5,
    PREBOOT_ISA_E_RAW_CAPACITY = 6,
    PREBOOT_ISA_E_RAW_DUPLICATE = 7,
    PREBOOT_ISA_E_INVALID_SNAPSHOT = 8
};

struct preboot_isa_raw_register {
    uint32_t register_space;
    uint32_t selector;
    uint32_t subselector;
    uint32_t access_state;
    uint64_t value;
};

struct preboot_isa_cpu_snapshot {
    uint32_t abi_version;
    uint32_t architecture;
    uint32_t logical_cpu_id;
    uint32_t privilege_level;
    uint32_t family;
    uint32_t model;
    uint32_t stepping;
    uint32_t physical_address_bits;
    uint32_t virtual_address_bits;
    uint32_t page_granule_mask;
    uint32_t state_flags;
    uint32_t raw_register_count;
    uint8_t vendor[16];
    uint64_t raw_identity0;
    uint64_t raw_identity1;
    uint32_t reserved0;
    uint32_t reserved1;
    struct preboot_isa_raw_register raw[PREBOOT_ISA_MAX_RAW_REGISTERS];
};

_Static_assert(sizeof(struct preboot_isa_raw_register) == 24U,
               "Preboot ISA raw register size");
_Static_assert(sizeof(struct preboot_isa_cpu_snapshot) <= 8192U,
               "Preboot ISA snapshot size");

void preboot_isa_snapshot_init(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t architecture,
    uint32_t logical_cpu_id);

enum preboot_isa_error preboot_isa_raw_append(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t register_space,
    uint32_t selector,
    uint32_t subselector,
    uint32_t access_state,
    uint64_t value);

enum preboot_isa_error preboot_isa_snapshot_validate(
    const struct preboot_isa_cpu_snapshot *snapshot);

/* Each preboot image links exactly one ISA adapter implementation. */
enum preboot_isa_error preboot_isa_discover(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t logical_cpu_id);

enum preboot_isa_mmu_error preboot_isa_mmu_build(
    const struct preboot_isa_cpu_snapshot *snapshot,
    struct preboot_isa_mmu_profile *profile);

#endif
