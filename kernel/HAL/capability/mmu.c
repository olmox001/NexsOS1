#include <stddef.h>
#include <stdint.h>

#include "isa.h"
#include "../common/hcd_wire.h"

static void preboot_isa_mmu_zero(
    struct preboot_isa_mmu_profile *profile)
{
    uint8_t *bytes = (uint8_t *)profile;
    size_t index = 0U;

    for (index = 0U; index < sizeof(*profile); ++index) {
        bytes[index] = 0U;
    }
}

static int preboot_isa_mmu_ranges_overlap(
    const void *left,
    size_t left_size,
    const void *right,
    size_t right_size)
{
    uintptr_t left_start = (uintptr_t)left;
    uintptr_t right_start = (uintptr_t)right;
    uintptr_t left_end = 0U;
    uintptr_t right_end = 0U;

    if ((left_size > UINTPTR_MAX - left_start) ||
        (right_size > UINTPTR_MAX - right_start)) {
        return 1;
    }
    left_end = left_start + (uintptr_t)left_size;
    right_end = right_start + (uintptr_t)right_size;
    return (left_start < right_end) && (right_start < left_end);
}

static const struct preboot_isa_raw_register *preboot_isa_mmu_find_raw(
    const struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t register_space,
    uint32_t selector,
    uint32_t subselector)
{
    uint32_t index = 0U;

    for (index = 0U; index < snapshot->raw_register_count; ++index) {
        const struct preboot_isa_raw_register *record =
            &snapshot->raw[index];

        if ((record->register_space == register_space) &&
            (record->selector == selector) &&
            (record->subselector == subselector) &&
            (record->access_state == PREBOOT_ISA_ACCESS_READ)) {
            return record;
        }
    }
    return NULL;
}

static int preboot_isa_mmu_read_cpuid(
    const struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t *eax,
    uint32_t *ebx,
    uint32_t *ecx,
    uint32_t *edx)
{
    const struct preboot_isa_raw_register *registers[4];
    uint32_t subselectors[4] = {subleaf, subleaf, subleaf, subleaf};
    uint32_t spaces[4] = {
        PREBOOT_ISA_REGSPACE_X86_CPUID_EAX,
        PREBOOT_ISA_REGSPACE_X86_CPUID_EBX,
        PREBOOT_ISA_REGSPACE_X86_CPUID_ECX,
        PREBOOT_ISA_REGSPACE_X86_CPUID_EDX
    };
    uint32_t index = 0U;

    for (index = 0U; index < 4U; ++index) {
        registers[index] = preboot_isa_mmu_find_raw(
            snapshot, spaces[index], leaf, subselectors[index]);
        if (registers[index] == NULL) {
            return 0;
        }
    }
    *eax = (uint32_t)registers[0]->value;
    *ebx = (uint32_t)registers[1]->value;
    *ecx = (uint32_t)registers[2]->value;
    *edx = (uint32_t)registers[3]->value;
    return 1;
}

static int preboot_isa_mmu_append_mapping(
    struct preboot_isa_mmu_profile *profile,
    uint32_t cpu_id,
    uint32_t config_id,
    uint32_t level,
    uint32_t kind,
    uint64_t size)
{
    struct hcd_mmu_mapping_size *mapping = NULL;

    if (profile->mapping_size_count >=
        PREBOOT_ISA_MMU_MAX_MAPPING_SIZES) {
        return 0;
    }
    mapping = &profile->mapping_sizes[profile->mapping_size_count];
    mapping->logical_cpu_id = cpu_id;
    mapping->configuration_id = config_id;
    mapping->level_index = level;
    mapping->mapping_kind = kind;
    mapping->size_bytes = size;
    profile->mapping_size_count++;
    return 1;
}

static uint32_t preboot_isa_mmu_levels_mask(uint32_t level_count)
{
    return (UINT32_C(1) << level_count) - UINT32_C(1);
}

static enum preboot_isa_mmu_error preboot_isa_mmu_build_amd64(
    const struct preboot_isa_cpu_snapshot *snapshot,
    struct preboot_isa_mmu_profile *profile)
{
    const struct preboot_isa_raw_register *cr0_record =
        preboot_isa_mmu_find_raw(snapshot, PREBOOT_ISA_REGSPACE_X86_CONTROL,
                                 PREBOOT_AMD64_REG_CR0, 0U);
    const struct preboot_isa_raw_register *cr4_record =
        preboot_isa_mmu_find_raw(snapshot, PREBOOT_ISA_REGSPACE_X86_CONTROL,
                                 PREBOOT_AMD64_REG_CR4, 0U);
    const struct preboot_isa_raw_register *efer_record =
        preboot_isa_mmu_find_raw(snapshot, PREBOOT_ISA_REGSPACE_X86_MSR,
                                 UINT32_C(0xC0000080), 0U);
    struct hcd_mmu_capabilities *legacy = &profile->legacy;
    struct hcd_mmu_translation_config *config =
        &profile->configurations[0];
    uint32_t leaf1_eax = 0U;
    uint32_t leaf1_ebx = 0U;
    uint32_t leaf1_ecx = 0U;
    uint32_t leaf1_edx = 0U;
    uint32_t leaf7_eax = 0U;
    uint32_t leaf7_ebx = 0U;
    uint32_t leaf7_ecx = 0U;
    uint32_t leaf7_edx = 0U;
    uint32_t extended1_eax = 0U;
    uint32_t extended1_ebx = 0U;
    uint32_t extended1_ecx = 0U;
    uint32_t extended1_edx = 0U;
    uint64_t cr0 = 0U;
    uint64_t cr4 = 0U;
    uint64_t efer = 0U;
    uint32_t level_count = 0U;
    uint32_t virtual_bits = 0U;
    uint32_t granule_mask = UINT32_C(1);
    uint32_t block_mask = 0U;
    uint32_t tlb_features = HCD_MMU_TLB_INVALIDATE_PAGE;
    uint32_t attributes = HCD_MMU_ATTR_NORMAL_WB |
        HCD_MMU_ATTR_NORMAL_UC | HCD_MMU_ATTR_DEVICE;
    uint32_t levels_supported = 4U;
    uint32_t config_flags = HCD_MMU_CONFIG_F_CURRENT;
    uint32_t leaf1_present = 0U;
    uint32_t leaf7_present = 0U;
    uint32_t extended1_present = 0U;
    uint32_t page_2m = 0U;
    uint32_t page_1g = 0U;
    uint32_t page_index = 0U;

    if ((cr0_record == NULL) || (cr4_record == NULL) ||
        (efer_record == NULL)) {
        return PREBOOT_ISA_MMU_E_RAW_REGISTER_MISSING;
    }
    cr0 = cr0_record->value;
    cr4 = cr4_record->value;
    efer = efer_record->value;
    if (((efer & (UINT64_C(1) << 10U)) == 0U) ||
        ((cr0 & (UINT64_C(1) << 31U)) == 0U)) {
        return PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE;
    }
    leaf1_present = (uint32_t)preboot_isa_mmu_read_cpuid(
        snapshot, 1U, 0U, &leaf1_eax, &leaf1_ebx, &leaf1_ecx, &leaf1_edx);
    leaf7_present = (uint32_t)preboot_isa_mmu_read_cpuid(
        snapshot, 7U, 0U, &leaf7_eax, &leaf7_ebx, &leaf7_ecx, &leaf7_edx);
    extended1_present = (uint32_t)preboot_isa_mmu_read_cpuid(
        snapshot, UINT32_C(0x80000001), 0U,
        &extended1_eax, &extended1_ebx, &extended1_ecx, &extended1_edx);
    if ((leaf1_present == 0U) || (leaf7_present == 0U) ||
        (extended1_present == 0U)) {
        return PREBOOT_ISA_MMU_E_RAW_REGISTER_MISSING;
    }

    page_2m = (leaf1_edx >> 3U) & UINT32_C(1);
    page_1g = (extended1_edx >> 26U) & UINT32_C(1);
    if ((leaf7_ecx & (UINT32_C(1) << 16U)) != 0U) {
        levels_supported = 5U;
    }
    if ((cr4 & (UINT64_C(1) << 12U)) != 0U) {
        if (levels_supported != 5U) {
            return PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE;
        }
        level_count = 5U;
        virtual_bits = snapshot->virtual_address_bits;
        if (virtual_bits > 57U) {
            virtual_bits = 57U;
        }
        if (virtual_bits < 57U) {
            return PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE;
        }
    } else {
        level_count = 4U;
        virtual_bits = snapshot->virtual_address_bits;
        if (virtual_bits > 48U) {
            virtual_bits = 48U;
        }
        if (virtual_bits < 48U) {
            return PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE;
        }
    }
    if ((leaf1_edx & (UINT32_C(1) << 16U)) != 0U) {
        attributes |= HCD_MMU_ATTR_NORMAL_WT | HCD_MMU_ATTR_NORMAL_WC;
    }
    if ((efer & (UINT64_C(1) << 11U)) != 0U) {
        attributes |= HCD_MMU_ATTR_EXECUTE_NEVER;
    }
    if ((page_2m != 0U) && (level_count >= 2U)) {
        block_mask |= UINT32_C(2);
    }
    if ((page_1g != 0U) && (level_count >= 3U)) {
        block_mask |= UINT32_C(4);
    }
    if ((cr0 & (UINT64_C(1) << 31U)) != 0U) {
        config_flags |= HCD_MMU_CONFIG_F_TRANSLATION_ENABLED;
    }

    legacy->architecture = HCD_ARCH_AMD64;
    legacy->physical_address_bits = snapshot->physical_address_bits;
    legacy->virtual_address_bits = virtual_bits;
    legacy->page_granule_mask = granule_mask | block_mask;
    legacy->block_granule_mask = block_mask;
    legacy->levels_supported = preboot_isa_mmu_levels_mask(levels_supported);
    legacy->levels_active = preboot_isa_mmu_levels_mask(level_count);
    legacy->tlb_features = tlb_features;
    legacy->cache_attributes = attributes;
    legacy->translation_regime = HCD_MMU_REGIME_AMD64;
    legacy->logical_cpu_id = snapshot->logical_cpu_id;
    legacy->raw_register0 = cr4;
    legacy->raw_register1 = efer;

    config->architecture = HCD_ARCH_AMD64;
    config->logical_cpu_id = snapshot->logical_cpu_id;
    config->configuration_id = 0U;
    config->translation_regime = HCD_MMU_REGIME_AMD64;
    config->flags = config_flags;
    config->exception_level = HCD_MMU_EXCEPTION_LEVEL_NONE;
    config->physical_address_bits = snapshot->physical_address_bits;
    config->virtual_address_bits = virtual_bits;
    config->page_granule_bytes = 4096U;
    config->level_count = level_count;
    config->virtual_address_range = HCD_MMU_VA_RANGE_AMD64_CANONICAL;
    config->tlb_features = tlb_features;
    config->memory_attributes = attributes;
    profile->configuration_count = 1U;

    if (page_1g != 0U) {
        page_index = level_count - 3U;
        if (preboot_isa_mmu_append_mapping(profile, snapshot->logical_cpu_id,
                0U, page_index, HCD_MMU_MAPPING_BLOCK,
                UINT64_C(0x40000000)) == 0) {
            return PREBOOT_ISA_MMU_E_CAPACITY;
        }
    }
    if (page_2m != 0U) {
        page_index = level_count - 2U;
        if (preboot_isa_mmu_append_mapping(profile, snapshot->logical_cpu_id,
                0U, page_index, HCD_MMU_MAPPING_BLOCK,
                UINT64_C(0x200000)) == 0) {
            return PREBOOT_ISA_MMU_E_CAPACITY;
        }
    }
    if (preboot_isa_mmu_append_mapping(profile, snapshot->logical_cpu_id, 0U,
            level_count - 1U, HCD_MMU_MAPPING_PAGE, UINT64_C(0x1000)) == 0) {
        return PREBOOT_ISA_MMU_E_CAPACITY;
    }
    profile->mapping_size_count =
        page_1g + page_2m + UINT32_C(1);
    return PREBOOT_ISA_MMU_OK;
}

static uint32_t preboot_isa_mmu_arm64_granule_bytes(
    uint32_t granule_mask)
{
    if (granule_mask == PREBOOT_ISA_GRANULE_4K) return 4096U;
    if (granule_mask == PREBOOT_ISA_GRANULE_16K) return 16384U;
    if (granule_mask == PREBOOT_ISA_GRANULE_64K) return 65536U;
    return 0U;
}

static uint32_t preboot_isa_mmu_arm64_granule_mask(
    uint64_t tcr,
    uint32_t upper,
    uint32_t el)
{
    uint32_t encoding = 0U;

    if (upper != 0U) {
        encoding = (uint32_t)((tcr >> 30U) & UINT64_C(3));
        if (encoding == 1U) return PREBOOT_ISA_GRANULE_16K;
        if (encoding == 2U) return PREBOOT_ISA_GRANULE_4K;
        if (encoding == 3U) return PREBOOT_ISA_GRANULE_64K;
        return 0U;
    }
    encoding = (uint32_t)((tcr >> 14U) & UINT64_C(3));
    (void)el; /* TG0 encoding is identical for supported EL1/EL2 stage-1. */
    if (encoding == 0U) return PREBOOT_ISA_GRANULE_4K;
    if (encoding == 1U) return PREBOOT_ISA_GRANULE_64K;
    if (encoding == 2U) return PREBOOT_ISA_GRANULE_16K;
    return 0U;
}

static uint32_t preboot_isa_mmu_arm64_decode_pa_bits(uint32_t range)
{
    static const uint8_t widths[7] = {32U, 36U, 40U, 42U, 44U, 48U, 52U};

    return (range < 7U) ? widths[range] : 0U;
}

static int preboot_isa_mmu_arm64_pc_in_range(
    uint64_t pc,
    uint32_t virtual_bits,
    uint32_t upper)
{
    uint64_t range_size = UINT64_C(1) << virtual_bits;
    uint64_t upper_start = UINT64_C(0) - range_size;

    return (upper != 0U) ? (pc >= upper_start) : (pc < range_size);
}

static uint32_t preboot_isa_mmu_arm64_attributes(
    uint64_t mair,
    uint32_t shareability)
{
    uint32_t attributes = HCD_MMU_ATTR_EXECUTE_NEVER;
    uint32_t index = 0U;

    for (index = 0U; index < 8U; ++index) {
        uint32_t attr = (uint32_t)((mair >> (index * 8U)) & UINT64_C(0xFF));

        if ((attr == UINT32_C(0xFF)) ||
            (attr == UINT32_C(0xEE)) ||
            (attr == UINT32_C(0xDD))) {
            attributes |= HCD_MMU_ATTR_NORMAL_WB;
        } else if ((attr == UINT32_C(0xBB)) ||
                   (attr == UINT32_C(0xAA))) {
            attributes |= HCD_MMU_ATTR_NORMAL_WT;
        } else if (attr == UINT32_C(0x44)) {
            attributes |= HCD_MMU_ATTR_NORMAL_UC;
        } else if (((attr & UINT32_C(0xF0)) == 0U) &&
                   ((attr & UINT32_C(0x0F)) <= UINT32_C(0x0C)) &&
                   ((attr & UINT32_C(0x03)) == 0U)) {
            attributes |= HCD_MMU_ATTR_DEVICE;
        }
    }
    if (shareability == 3U) {
        attributes |= HCD_MMU_ATTR_INNER_SHAREABLE;
    } else if (shareability == 2U) {
        attributes |= HCD_MMU_ATTR_OUTER_SHAREABLE;
    } else {
        attributes |= HCD_MMU_ATTR_NON_SHAREABLE;
    }
    return attributes;
}

static int preboot_isa_mmu_arm64_add_mappings(
    struct preboot_isa_mmu_profile *profile,
    uint32_t cpu_id,
    uint32_t config_id,
    uint32_t granule,
    uint32_t level_count)
{
    uint32_t page_shift = 0U;
    uint32_t index_bits = 0U;
    uint32_t start_level = 0U;
    uint32_t level_index = 0U;

    if (granule == 4096U) {
        page_shift = 12U;
        index_bits = 9U;
    } else if (granule == 16384U) {
        page_shift = 14U;
        index_bits = 11U;
    } else if (granule == 65536U) {
        page_shift = 16U;
        index_bits = 13U;
    } else {
        return 0;
    }
    start_level = 4U - level_count;
    for (level_index = 0U; level_index < level_count; ++level_index) {
        uint32_t architecture_level = start_level + level_index;
        uint32_t shift = page_shift +
            (index_bits * (3U - architecture_level));
        uint64_t size = UINT64_C(1) << shift;

        if ((architecture_level == 1U) || (architecture_level == 2U)) {
            if (size <= (UINT64_C(1) << 52U)) {
                if (preboot_isa_mmu_append_mapping(profile, cpu_id, config_id,
                        level_index, HCD_MMU_MAPPING_BLOCK, size) == 0) {
                    return 0;
                }
            }
        }
    }
    return preboot_isa_mmu_append_mapping(profile, cpu_id, config_id,
        level_count - 1U, HCD_MMU_MAPPING_PAGE, (uint64_t)granule);
}

static enum preboot_isa_mmu_error preboot_isa_mmu_build_arm64(
    const struct preboot_isa_cpu_snapshot *snapshot,
    struct preboot_isa_mmu_profile *profile)
{
    const struct preboot_isa_raw_register *el_record =
        preboot_isa_mmu_find_raw(snapshot, PREBOOT_ISA_REGSPACE_ARM_SYSREG,
                                 PREBOOT_ARM64_REG_CURRENT_EL, 0U);
    const struct preboot_isa_raw_register *sctlr_record =
        preboot_isa_mmu_find_raw(snapshot, PREBOOT_ISA_REGSPACE_ARM_SYSREG,
                                 PREBOOT_ARM64_REG_SCTLR, 0U);
    const struct preboot_isa_raw_register *tcr_record =
        preboot_isa_mmu_find_raw(snapshot, PREBOOT_ISA_REGSPACE_ARM_SYSREG,
                                 PREBOOT_ARM64_REG_TCR, 0U);
    const struct preboot_isa_raw_register *mair_record =
        preboot_isa_mmu_find_raw(snapshot, PREBOOT_ISA_REGSPACE_ARM_SYSREG,
                                 PREBOOT_ARM64_REG_MAIR, 0U);
    const struct preboot_isa_raw_register *pc_record =
        preboot_isa_mmu_find_raw(snapshot, PREBOOT_ISA_REGSPACE_ARM_SYSREG,
                                 PREBOOT_ARM64_REG_PC, 0U);
    uint64_t sctlr = 0U;
    uint64_t tcr = 0U;
    uint64_t mair = 0U;
    uint64_t pc = 0U;
    uint32_t el = 0U;
    uint32_t ips_shift = 0U;
    uint32_t pa_bits = 0U;
    uint32_t mmu_enabled = 0U;
    uint32_t range_count = 0U;
    uint32_t range = 0U;
    uint32_t configuration_id = 0U;
    uint32_t supported_page_mask = 0U;
    uint32_t supported_block_mask = 0U;
    uint32_t active_level_mask = 0U;
    uint32_t maximum_va_bits = 0U;
    uint32_t memory_attributes = 0U;
    uint32_t current_context_count = 0U;
    struct hcd_mmu_capabilities *legacy = &profile->legacy;

    if ((el_record == NULL) || (sctlr_record == NULL) ||
        (tcr_record == NULL) || (mair_record == NULL) ||
        (pc_record == NULL)) {
        return PREBOOT_ISA_MMU_E_RAW_REGISTER_MISSING;
    }
    el = (uint32_t)((el_record->value >> 2U) & UINT64_C(3));
    if ((el != 1U) && (el != 2U)) {
        return PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE;
    }
    sctlr = sctlr_record->value;
    tcr = tcr_record->value;
    mair = mair_record->value;
    pc = pc_record->value;
    mmu_enabled = (uint32_t)(sctlr & UINT64_C(1));
    ips_shift = (el == 1U) ? 32U : 16U;
    pa_bits = preboot_isa_mmu_arm64_decode_pa_bits(
        (uint32_t)((tcr >> ips_shift) & UINT64_C(7)));
    if ((pa_bits == 0U) || (pa_bits > snapshot->physical_address_bits)) {
        return PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE;
    }
    memory_attributes = preboot_isa_mmu_arm64_attributes(mair, 0U);
    range_count = (el == 1U) ? 2U : 1U;
    supported_page_mask = snapshot->page_granule_mask &
        (PREBOOT_ISA_GRANULE_4K | PREBOOT_ISA_GRANULE_16K |
         PREBOOT_ISA_GRANULE_64K);
    supported_block_mask = supported_page_mask;

    for (range = 0U; range < range_count; ++range) {
        uint32_t upper = (range == 1U);
        uint32_t granule_mask =
            preboot_isa_mmu_arm64_granule_mask(tcr, upper, el);
        uint32_t granule = preboot_isa_mmu_arm64_granule_bytes(granule_mask);
        uint32_t size_shift = upper ? 16U : 0U;
        uint32_t size_bits = (uint32_t)((tcr >> size_shift) & UINT64_C(0x3F));
        uint32_t virtual_bits = 64U - size_bits;
        uint32_t page_shift = 0U;
        uint32_t index_bits = 0U;
        uint32_t level_count = 0U;
        uint32_t shareability_shift = upper ? 28U : 12U;
        uint32_t shareability =
            (uint32_t)((tcr >> shareability_shift) & UINT64_C(3));
        uint32_t epd = (upper != 0U) ?
            (uint32_t)((tcr >> 23U) & UINT64_C(1)) :
            (uint32_t)((tcr >> 7U) & UINT64_C(1));
        uint32_t config_flags = 0U;
        uint32_t config_id = configuration_id;
        struct hcd_mmu_translation_config *config = NULL;
        struct hcd_mmu_arm64_context *context = NULL;

        if ((granule == 0U) ||
            ((supported_page_mask & granule_mask) == 0U) ||
            (virtual_bits == 0U) || (virtual_bits > 52U) ||
            (shareability == 1U)) {
            return PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE;
        }
        if (granule == 4096U) {
            page_shift = 12U;
            index_bits = 9U;
        } else if (granule == 16384U) {
            page_shift = 14U;
            index_bits = 11U;
        } else {
            page_shift = 16U;
            index_bits = 13U;
        }
        level_count = (virtual_bits <= page_shift) ? 1U :
            (virtual_bits - page_shift + index_bits - 1U) / index_bits;
        if ((level_count == 0U) || (level_count > 4U)) {
            return PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE;
        }
        if (configuration_id >= PREBOOT_ISA_MMU_MAX_CONFIGURATIONS) {
            return PREBOOT_ISA_MMU_E_CAPACITY;
        }
        config = &profile->configurations[configuration_id];
        if (preboot_isa_mmu_arm64_pc_in_range(
                pc, virtual_bits, upper) != 0) {
            config_flags |= HCD_MMU_CONFIG_F_CURRENT;
            current_context_count++;
        }
        if (((config_flags & HCD_MMU_CONFIG_F_CURRENT) != 0U) &&
            (mmu_enabled != 0U) && (epd == 0U)) {
            config_flags |= HCD_MMU_CONFIG_F_TRANSLATION_ENABLED;
        }
        config->architecture = HCD_ARCH_ARM64;
        config->logical_cpu_id = snapshot->logical_cpu_id;
        config->configuration_id = configuration_id;
        config->translation_regime = HCD_MMU_REGIME_ARM64_STAGE1;
        config->flags = config_flags;
        config->exception_level = (el == 1U) ?
            HCD_MMU_EXCEPTION_LEVEL_EL1 : HCD_MMU_EXCEPTION_LEVEL_EL2;
        config->physical_address_bits = pa_bits;
        config->virtual_address_bits = virtual_bits;
        config->page_granule_bytes = granule;
        config->level_count = level_count;
        config->virtual_address_range = upper ?
            HCD_MMU_VA_RANGE_ARM64_UPPER : HCD_MMU_VA_RANGE_ARM64_LOWER;
        config->tlb_features = HCD_MMU_TLB_INVALIDATE_PAGE |
            HCD_MMU_TLB_INVALIDATE_CONTEXT | HCD_MMU_TLB_INVALIDATE_GLOBAL;
        config->memory_attributes =
            preboot_isa_mmu_arm64_attributes(mair, shareability);
        if (profile->arm64_context_count >=
            PREBOOT_ISA_MMU_MAX_ARM64_CONTEXTS) {
            return PREBOOT_ISA_MMU_E_CAPACITY;
        }
        context = &profile->arm64_contexts[profile->arm64_context_count];
        context->logical_cpu_id = snapshot->logical_cpu_id;
        context->configuration_id = config_id;
        context->context_kind = upper != 0U ?
            HCD_MMU_ARM64_CONTEXT_TTBR1 :
            HCD_MMU_ARM64_CONTEXT_TTBR0;
        context->flags = ((mmu_enabled != 0U) && (epd == 0U)) ?
            HCD_MMU_ARM64_CONTEXT_F_TRANSLATION_ENABLED : 0U;
        context->exception_level = (el == 1U) ?
            HCD_MMU_EXCEPTION_LEVEL_EL1 : HCD_MMU_EXCEPTION_LEVEL_EL2;
        profile->arm64_context_count++;
        if (preboot_isa_mmu_arm64_add_mappings(profile,
                snapshot->logical_cpu_id, config_id, granule,
                level_count) == 0) {
            return PREBOOT_ISA_MMU_E_CAPACITY;
        }
        if ((context->flags &
             HCD_MMU_ARM64_CONTEXT_F_TRANSLATION_ENABLED) != 0U) {
            active_level_mask |= preboot_isa_mmu_levels_mask(level_count);
        }
        if (virtual_bits > maximum_va_bits) {
            maximum_va_bits = virtual_bits;
        }
        configuration_id++;
    }
    if (current_context_count != 1U) {
        return PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE;
    }
    legacy->architecture = HCD_ARCH_ARM64;
    legacy->physical_address_bits = pa_bits;
    legacy->virtual_address_bits = maximum_va_bits;
    legacy->page_granule_mask = supported_page_mask;
    legacy->block_granule_mask = supported_block_mask;
    legacy->levels_supported = UINT32_C(0x0F);
    legacy->levels_active = (active_level_mask != 0U) ?
        active_level_mask :
        preboot_isa_mmu_levels_mask(
            profile->configurations[0].level_count);
    legacy->tlb_features = HCD_MMU_TLB_INVALIDATE_PAGE |
        HCD_MMU_TLB_INVALIDATE_CONTEXT | HCD_MMU_TLB_INVALIDATE_GLOBAL;
    legacy->cache_attributes = memory_attributes;
    legacy->translation_regime = HCD_MMU_REGIME_ARM64_STAGE1;
    legacy->logical_cpu_id = snapshot->logical_cpu_id;
    legacy->raw_register0 = tcr;
    legacy->raw_register1 = mair;
    profile->configuration_count = configuration_id;
    return PREBOOT_ISA_MMU_OK;
}

enum preboot_isa_mmu_error preboot_isa_mmu_build(
    const struct preboot_isa_cpu_snapshot *snapshot,
    struct preboot_isa_mmu_profile *profile)
{
    enum preboot_isa_error validation = PREBOOT_ISA_OK;
    enum preboot_isa_mmu_error result = PREBOOT_ISA_MMU_OK;

    if ((snapshot == NULL) || (profile == NULL)) {
        return PREBOOT_ISA_MMU_E_NULL_INPUT;
    }
    if (preboot_isa_mmu_ranges_overlap(snapshot, sizeof(*snapshot),
                                       profile, sizeof(*profile)) != 0) {
        return PREBOOT_ISA_MMU_E_OUTPUT_RANGE;
    }
    validation = preboot_isa_snapshot_validate(snapshot);
    if (validation != PREBOOT_ISA_OK) {
        return PREBOOT_ISA_MMU_E_INVALID_SNAPSHOT;
    }
    preboot_isa_mmu_zero(profile);
    if (snapshot->architecture == PREBOOT_ISA_ARCH_AMD64) {
        result = preboot_isa_mmu_build_amd64(snapshot, profile);
    } else if (snapshot->architecture == PREBOOT_ISA_ARCH_ARM64) {
        result = preboot_isa_mmu_build_arm64(snapshot, profile);
    } else {
        result = PREBOOT_ISA_MMU_E_UNSUPPORTED_STATE;
    }
    if (result != PREBOOT_ISA_MMU_OK) {
        profile->configuration_count = 0U;
        profile->mapping_size_count = 0U;
        return result;
    }
    return PREBOOT_ISA_MMU_OK;
}
