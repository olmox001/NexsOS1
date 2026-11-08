#include "capability/isa.h"

static enum preboot_isa_error append_cpuid(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t eax,
    uint32_t ebx,
    uint32_t ecx,
    uint32_t edx)
{
    enum preboot_isa_error result = PREBOOT_ISA_OK;
    const uint32_t spaces[4] = {
        PREBOOT_ISA_REGSPACE_X86_CPUID_EAX,
        PREBOOT_ISA_REGSPACE_X86_CPUID_EBX,
        PREBOOT_ISA_REGSPACE_X86_CPUID_ECX,
        PREBOOT_ISA_REGSPACE_X86_CPUID_EDX
    };
    const uint32_t values[4] = {eax, ebx, ecx, edx};
    uint32_t index = 0U;

    for (index = 0U; index < 4U; ++index) {
        result = preboot_isa_raw_append(snapshot, spaces[index],
            leaf, subleaf, PREBOOT_ISA_ACCESS_READ, values[index]);
        if (result != PREBOOT_ISA_OK) {
            return result;
        }
    }
    return PREBOOT_ISA_OK;
}

static int initialize_amd64_snapshot(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t cr4,
    uint32_t la57_supported)
{
    uint32_t leaf7_ecx = (la57_supported != 0U) ?
        (UINT32_C(1) << 16U) : 0U;

    preboot_isa_snapshot_init(snapshot, PREBOOT_ISA_ARCH_AMD64, 0U);
    snapshot->physical_address_bits = 52U;
    snapshot->virtual_address_bits = (la57_supported != 0U) ? 57U : 48U;
    snapshot->page_granule_mask = PREBOOT_ISA_GRANULE_4K |
        PREBOOT_ISA_GRANULE_2M | PREBOOT_ISA_GRANULE_1G;
    snapshot->vendor[0] = 'T';
    if (append_cpuid(snapshot, 1U, 0U, 0U, 0U, 0U,
            (UINT32_C(1) << 3U) | (UINT32_C(1) << 16U)) !=
        PREBOOT_ISA_OK) {
        return 1;
    }
    if (append_cpuid(snapshot, 7U, 0U, 0U, 0U, leaf7_ecx, 0U) !=
        PREBOOT_ISA_OK) {
        return 2;
    }
    if (append_cpuid(snapshot, UINT32_C(0x80000001), 0U, 0U, 0U, 0U,
            UINT32_C(1) << 26U) != PREBOOT_ISA_OK) {
        return 3;
    }
    if (preboot_isa_raw_append(snapshot, PREBOOT_ISA_REGSPACE_X86_CONTROL,
            PREBOOT_AMD64_REG_CR0, 0U, PREBOOT_ISA_ACCESS_READ,
            UINT64_C(1) << 31U) != PREBOOT_ISA_OK) {
        return 4;
    }
    if (preboot_isa_raw_append(snapshot, PREBOOT_ISA_REGSPACE_X86_CONTROL,
            PREBOOT_AMD64_REG_CR4, 0U, PREBOOT_ISA_ACCESS_READ, cr4) !=
        PREBOOT_ISA_OK) {
        return 5;
    }
    if (preboot_isa_raw_append(snapshot, PREBOOT_ISA_REGSPACE_X86_MSR,
            UINT32_C(0xC0000080), 0U, PREBOOT_ISA_ACCESS_READ,
            (UINT64_C(1) << 10U) | (UINT64_C(1) << 11U)) !=
        PREBOOT_ISA_OK) {
        return 6;
    }
    return (preboot_isa_snapshot_validate(snapshot) == PREBOOT_ISA_OK) ? 0 : 7;
}

static int test_amd64_active_five_level_mmu(void)
{
    struct preboot_isa_cpu_snapshot snapshot;
    struct preboot_isa_mmu_profile profile;

    if (initialize_amd64_snapshot(&snapshot, UINT32_C(1) << 12U, 1U) != 0) {
        return 1;
    }
    if (preboot_isa_mmu_build(&snapshot, &profile) != PREBOOT_ISA_MMU_OK) {
        return 2;
    }
    if ((profile.configuration_count != 1U) ||
        (profile.mapping_size_count != 3U) ||
        (profile.configurations[0].level_count != 5U) ||
        (profile.configurations[0].virtual_address_bits != 57U) ||
        (profile.configurations[0].flags !=
            (HCD_MMU_CONFIG_F_CURRENT |
             HCD_MMU_CONFIG_F_TRANSLATION_ENABLED))) {
        return 3;
    }
    if ((profile.mapping_sizes[0].level_index != 2U) ||
        (profile.mapping_sizes[0].mapping_kind != HCD_MMU_MAPPING_BLOCK) ||
        (profile.mapping_sizes[0].size_bytes != UINT64_C(0x40000000)) ||
        (profile.mapping_sizes[1].level_index != 3U) ||
        (profile.mapping_sizes[1].size_bytes != UINT64_C(0x200000)) ||
        (profile.mapping_sizes[2].level_index != 4U) ||
        (profile.mapping_sizes[2].size_bytes != UINT64_C(0x1000))) {
        return 4;
    }
    return 0;
}

static int test_amd64_rejects_unavailable_la57(void)
{
    struct preboot_isa_cpu_snapshot snapshot;
    struct preboot_isa_mmu_profile profile;

    if (initialize_amd64_snapshot(&snapshot, UINT32_C(1) << 12U, 0U) != 0) {
        return 1;
    }
    if (preboot_isa_mmu_build(&snapshot, &profile) !=
        PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE) {
        return 2;
    }
    return 0;
}

static int test_amd64_accepts_four_level_mmu(void)
{
    struct preboot_isa_cpu_snapshot snapshot;
    struct preboot_isa_mmu_profile profile;

    if (initialize_amd64_snapshot(&snapshot, 0U, 0U) != 0) {
        return 1;
    }
    if (preboot_isa_mmu_build(&snapshot, &profile) != PREBOOT_ISA_MMU_OK) {
        return 2;
    }
    if ((profile.configuration_count != 1U) ||
        (profile.configurations[0].level_count != 4U) ||
        (profile.configurations[0].virtual_address_bits != 48U) ||
        (profile.configurations[0].flags !=
            (HCD_MMU_CONFIG_F_CURRENT |
             HCD_MMU_CONFIG_F_TRANSLATION_ENABLED))) {
        return 3;
    }
    return 0;
}

static int initialize_arm64_snapshot(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint64_t tcr,
    uint64_t mair)
{
    const uint32_t selectors[5] = {
        PREBOOT_ARM64_REG_CURRENT_EL,
        PREBOOT_ARM64_REG_SCTLR,
        PREBOOT_ARM64_REG_TCR,
        PREBOOT_ARM64_REG_MAIR,
        PREBOOT_ARM64_REG_PC
    };
    const uint64_t values[5] = {4U, 1U, tcr, mair, UINT64_C(0x40000000)};
    uint32_t index = 0U;

    preboot_isa_snapshot_init(snapshot, PREBOOT_ISA_ARCH_ARM64, 0U);
    snapshot->privilege_level = 1U;
    snapshot->physical_address_bits = 48U;
    snapshot->virtual_address_bits = 48U;
    snapshot->page_granule_mask = PREBOOT_ISA_GRANULE_4K |
        PREBOOT_ISA_GRANULE_16K | PREBOOT_ISA_GRANULE_64K;
    snapshot->state_flags = PREBOOT_ISA_STATE_MMU_ENABLED;
    snapshot->vendor[0] = UINT8_C(0x41);
    for (index = 0U; index < 5U; ++index) {
        if (preboot_isa_raw_append(snapshot,
                PREBOOT_ISA_REGSPACE_ARM_SYSREG, selectors[index], 0U,
                PREBOOT_ISA_ACCESS_READ, values[index]) != PREBOOT_ISA_OK) {
            return 1;
        }
    }
    return (preboot_isa_snapshot_validate(snapshot) == PREBOOT_ISA_OK) ? 0 : 2;
}

static int set_arm64_register(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t selector,
    uint64_t value)
{
    uint32_t index = 0U;

    for (index = 0U; index < snapshot->raw_register_count; ++index) {
        if ((snapshot->raw[index].register_space ==
             PREBOOT_ISA_REGSPACE_ARM_SYSREG) &&
            (snapshot->raw[index].selector == selector)) {
            snapshot->raw[index].value = value;
            return 0;
        }
    }
    return 1;
}

static int test_arm64_dual_stage1_mmu(void)
{
    struct preboot_isa_cpu_snapshot snapshot;
    struct preboot_isa_mmu_profile profile;
    uint64_t tcr =
        UINT64_C(16) |
        (UINT64_C(3) << 12U) |
        (UINT64_C(2) << 14U) |
        (UINT64_C(16) << 16U) |
        (UINT64_C(3) << 28U) |
        (UINT64_C(3) << 30U) |
        (UINT64_C(5) << 32U);

    if (initialize_arm64_snapshot(&snapshot, tcr, UINT64_C(0xFF)) != 0) {
        return 1;
    }
    if (preboot_isa_mmu_build(&snapshot, &profile) != PREBOOT_ISA_MMU_OK) {
        return 2;
    }
    if ((profile.configuration_count != 2U) ||
        (profile.mapping_size_count != 6U) ||
        (profile.configurations[0].virtual_address_range !=
            HCD_MMU_VA_RANGE_ARM64_LOWER) ||
        (profile.configurations[1].virtual_address_range !=
            HCD_MMU_VA_RANGE_ARM64_UPPER) ||
        (profile.configurations[0].flags !=
            (HCD_MMU_CONFIG_F_CURRENT |
             HCD_MMU_CONFIG_F_TRANSLATION_ENABLED)) ||
        (profile.configurations[1].flags != 0U) ||
        (profile.arm64_context_count != 2U) ||
        (profile.arm64_contexts[0].logical_cpu_id != 0U) ||
        (profile.arm64_contexts[0].configuration_id != 0U) ||
        (profile.arm64_contexts[0].context_kind !=
            HCD_MMU_ARM64_CONTEXT_TTBR0) ||
        (profile.arm64_contexts[0].flags !=
            HCD_MMU_ARM64_CONTEXT_F_TRANSLATION_ENABLED) ||
        (profile.arm64_contexts[1].configuration_id != 1U) ||
        (profile.arm64_contexts[1].context_kind !=
            HCD_MMU_ARM64_CONTEXT_TTBR1) ||
        (profile.arm64_contexts[1].flags !=
            HCD_MMU_ARM64_CONTEXT_F_TRANSLATION_ENABLED)) {
        return 3;
    }
    if ((profile.configurations[0].page_granule_bytes != 16384U) ||
        (profile.configurations[1].page_granule_bytes != 65536U) ||
        (profile.configurations[0].level_count != 4U) ||
        (profile.configurations[1].level_count != 3U) ||
        (profile.mapping_sizes[0].configuration_id != 0U) ||
        (profile.mapping_sizes[3].configuration_id != 1U) ||
        (profile.mapping_sizes[0].size_bytes != UINT64_C(0x1000000000)) ||
        (profile.mapping_sizes[1].size_bytes != UINT64_C(0x2000000)) ||
        (profile.mapping_sizes[2].size_bytes != UINT64_C(0x4000)) ||
        (profile.mapping_sizes[3].size_bytes != UINT64_C(0x40000000000)) ||
        (profile.mapping_sizes[4].size_bytes != UINT64_C(0x20000000)) ||
        (profile.mapping_sizes[5].size_bytes != UINT64_C(0x10000))) {
        return 4;
    }
    if ((profile.configurations[0].memory_attributes &
         HCD_MMU_ATTR_INNER_SHAREABLE) == 0U) {
        return 5;
    }
    return 0;
}

static int test_arm64_current_context_tracks_pc_and_translation_disable(void)
{
    struct preboot_isa_cpu_snapshot snapshot;
    struct preboot_isa_mmu_profile profile;
    uint64_t tcr = UINT64_C(16) | (UINT64_C(2) << 14U) |
        (UINT64_C(16) << 16U) | (UINT64_C(2) << 30U) |
        (UINT64_C(5) << 32U);
    uint64_t upper_pc = UINT64_MAX - UINT64_C(0x100000000);
    enum preboot_isa_mmu_error mmu_result = PREBOOT_ISA_MMU_OK;

    if (initialize_arm64_snapshot(&snapshot, tcr, UINT64_C(0xFF)) != 0) {
        return 1;
    }
    if (set_arm64_register(&snapshot, PREBOOT_ARM64_REG_PC, upper_pc) != 0) {
        return 12;
    }
    mmu_result = preboot_isa_mmu_build(&snapshot, &profile);
    if (mmu_result != PREBOOT_ISA_MMU_OK) {
        return 2;
    }
    if (((profile.configurations[0].flags & HCD_MMU_CONFIG_F_CURRENT) != 0U) ||
        (profile.configurations[1].flags !=
            (HCD_MMU_CONFIG_F_CURRENT |
             HCD_MMU_CONFIG_F_TRANSLATION_ENABLED)) ||
        ((profile.arm64_contexts[0].flags &
          HCD_MMU_ARM64_CONTEXT_F_TRANSLATION_ENABLED) == 0U) ||
        ((profile.arm64_contexts[1].flags &
          HCD_MMU_ARM64_CONTEXT_F_TRANSLATION_ENABLED) == 0U)) {
        return 3;
    }

    if ((set_arm64_register(&snapshot, PREBOOT_ARM64_REG_PC,
            UINT64_C(0x40000000)) != 0) ||
        (set_arm64_register(&snapshot, PREBOOT_ARM64_REG_TCR,
            tcr | (UINT64_C(1) << 7U)) != 0) ||
        (preboot_isa_mmu_build(&snapshot, &profile) != PREBOOT_ISA_MMU_OK)) {
        return 4;
    }
    if ((profile.configurations[0].flags != HCD_MMU_CONFIG_F_CURRENT) ||
        (profile.arm64_contexts[0].flags != 0U) ||
        ((profile.arm64_contexts[1].flags &
          HCD_MMU_ARM64_CONTEXT_F_TRANSLATION_ENABLED) == 0U)) {
        return 5;
    }
    return 0;
}

static int test_arm64_rejects_pc_outside_ttbr_ranges(void)
{
    struct preboot_isa_cpu_snapshot snapshot;
    struct preboot_isa_mmu_profile profile;
    uint64_t tcr = UINT64_C(16) | (UINT64_C(16) << 16U) |
        (UINT64_C(5) << 32U);

    if (initialize_arm64_snapshot(&snapshot, tcr, UINT64_C(0xFF)) != 0) {
        return 1;
    }
    if (set_arm64_register(&snapshot, PREBOOT_ARM64_REG_PC,
            UINT64_C(0x0000800000000000)) != 0) {
        return 2;
    }
    if (preboot_isa_mmu_build(&snapshot, &profile) !=
        PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE) {
        return 3;
    }
    return 0;
}

static int test_arm64_rejects_pa_width_above_discovered_capability(void)
{
    struct preboot_isa_cpu_snapshot snapshot;
    struct preboot_isa_mmu_profile profile;
    uint64_t tcr = UINT64_C(16) | (UINT64_C(6) << 32U);

    if (initialize_arm64_snapshot(&snapshot, tcr, UINT64_C(0xFF)) != 0) {
        return 1;
    }
    if (preboot_isa_mmu_build(&snapshot, &profile) !=
        PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE) {
        return 2;
    }
    return 0;
}

int main(void)
{
    if (test_amd64_active_five_level_mmu() != 0) {
        return 1;
    }
    if (test_amd64_accepts_four_level_mmu() != 0) {
        return 2;
    }
    if (test_amd64_rejects_unavailable_la57() != 0) {
        return 3;
    }
    if (test_arm64_dual_stage1_mmu() != 0) {
        return 4;
    }
    if (test_arm64_current_context_tracks_pc_and_translation_disable() != 0) {
        return 5;
    }
    if (test_arm64_rejects_pc_outside_ttbr_ranges() != 0) {
        return 6;
    }
    if (test_arm64_rejects_pa_width_above_discovered_capability() != 0) {
        return 7;
    }
    return 0;
}
