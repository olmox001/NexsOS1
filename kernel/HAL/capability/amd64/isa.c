#include <stddef.h>
#include <stdint.h>
#include <arch/isa.h>
#include "../isa.h"

#if !defined(__x86_64__)
#error "AMD64 preboot HAL requires an x86_64 target"
#endif

static enum preboot_isa_error preboot_amd64_capture_values(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t leaf,
    uint32_t subleaf,
    uint32_t eax,
    uint32_t ebx,
    uint32_t ecx,
    uint32_t edx)
{
    enum preboot_isa_error result = PREBOOT_ISA_OK;

    result = preboot_isa_raw_append(snapshot, PREBOOT_ISA_REGSPACE_X86_CPUID_EAX,
        leaf, subleaf, PREBOOT_ISA_ACCESS_READ, eax);
    if (result != PREBOOT_ISA_OK) return result;
    result = preboot_isa_raw_append(snapshot, PREBOOT_ISA_REGSPACE_X86_CPUID_EBX,
        leaf, subleaf, PREBOOT_ISA_ACCESS_READ, ebx);
    if (result != PREBOOT_ISA_OK) return result;
    result = preboot_isa_raw_append(snapshot, PREBOOT_ISA_REGSPACE_X86_CPUID_ECX,
        leaf, subleaf, PREBOOT_ISA_ACCESS_READ, ecx);
    if (result != PREBOOT_ISA_OK) return result;
    return preboot_isa_raw_append(snapshot, PREBOOT_ISA_REGSPACE_X86_CPUID_EDX,
        leaf, subleaf, PREBOOT_ISA_ACCESS_READ, edx);
}

static enum preboot_isa_error preboot_amd64_capture_leaf(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t leaf,
    uint32_t subleaf)
{
    uint32_t eax = 0U;
    uint32_t ebx = 0U;
    uint32_t ecx = 0U;
    uint32_t edx = 0U;

    arch_isa_cpuid(leaf, subleaf, &eax, &ebx, &ecx, &edx);
    return preboot_amd64_capture_values(snapshot, leaf, subleaf,
                                        eax, ebx, ecx, edx);
}

static enum preboot_isa_error preboot_amd64_capture_cache_leaf(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t leaf)
{
    uint32_t subleaf = 0U;

    for (subleaf = 0U; subleaf < PREBOOT_ISA_MAX_CACHE_SUBLEAVES; ++subleaf) {
        uint32_t eax = 0U;
        uint32_t ebx = 0U;
        uint32_t ecx = 0U;
        uint32_t edx = 0U;
        enum preboot_isa_error result = PREBOOT_ISA_OK;

        arch_isa_cpuid(leaf, subleaf, &eax, &ebx, &ecx, &edx);
        if ((eax & UINT32_C(0x1F)) == 0U) {
            return PREBOOT_ISA_OK;
        }
        result = preboot_amd64_capture_values(snapshot, leaf, subleaf,
                                               eax, ebx, ecx, edx);
        if (result != PREBOOT_ISA_OK) return result;
    }
    return PREBOOT_ISA_E_RAW_CAPACITY;
}


static enum preboot_isa_error preboot_amd64_capture_feature_leaf(
    struct preboot_isa_cpu_snapshot *snapshot)
{
    uint32_t eax = 0U;
    uint32_t ebx = 0U;
    uint32_t ecx = 0U;
    uint32_t edx = 0U;
    uint32_t subleaf = 0U;
    enum preboot_isa_error result = PREBOOT_ISA_OK;

    arch_isa_cpuid(7U, 0U, &eax, &ebx, &ecx, &edx);
    if (eax >= PREBOOT_ISA_MAX_FEATURE_SUBLEAVES) {
        return PREBOOT_ISA_E_RAW_CAPACITY;
    }
    result = preboot_amd64_capture_values(snapshot, 7U, 0U,
                                          eax, ebx, ecx, edx);
    for (subleaf = 1U; (result == PREBOOT_ISA_OK) && (subleaf <= eax);
         ++subleaf) {
        result = preboot_amd64_capture_leaf(snapshot, 7U, subleaf);
    }
    return result;
}

static enum preboot_isa_error preboot_amd64_capture_topology_leaf(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t leaf)
{
    uint32_t subleaf = 0U;

    for (subleaf = 0U; subleaf < PREBOOT_ISA_MAX_TOPOLOGY_SUBLEAVES;
         ++subleaf) {
        uint32_t eax = 0U;
        uint32_t ebx = 0U;
        uint32_t ecx = 0U;
        uint32_t edx = 0U;
        enum preboot_isa_error result = PREBOOT_ISA_OK;

        arch_isa_cpuid(leaf, subleaf, &eax, &ebx, &ecx, &edx);
        if (((ebx & UINT32_C(0xFFFF)) == 0U) ||
            (((ecx >> 8U) & UINT32_C(0xFF)) == 0U)) {
            return PREBOOT_ISA_OK;
        }
        result = preboot_amd64_capture_values(snapshot, leaf, subleaf,
                                               eax, ebx, ecx, edx);
        if (result != PREBOOT_ISA_OK) return result;
    }
    return PREBOOT_ISA_E_RAW_CAPACITY;
}

static enum preboot_isa_error preboot_amd64_capture_control(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t selector,
    uint64_t value)
{
    return preboot_isa_raw_append(snapshot,
        PREBOOT_ISA_REGSPACE_X86_CONTROL, selector, 0U,
        PREBOOT_ISA_ACCESS_READ, value);
}

static void preboot_amd64_set_vendor(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t ebx,
    uint32_t edx,
    uint32_t ecx)
{
    uint32_t values[3] = {ebx, edx, ecx};
    uint32_t word = 0U;
    uint32_t byte = 0U;

    for (word = 0U; word < 3U; ++word) {
        for (byte = 0U; byte < 4U; ++byte) {
            snapshot->vendor[(word * 4U) + byte] =
                (uint8_t)(values[word] >> (byte * 8U));
        }
    }
}

static void preboot_amd64_decode_identity(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t eax,
    uint32_t ebx)
{
    uint32_t base_family = (eax >> 8U) & UINT32_C(0x0F);
    uint32_t base_model = (eax >> 4U) & UINT32_C(0x0F);
    uint32_t ext_family = (eax >> 20U) & UINT32_C(0xFF);
    uint32_t ext_model = (eax >> 16U) & UINT32_C(0x0F);

    snapshot->family = (base_family == 0x0FU) ?
        (base_family + ext_family) : base_family;
    snapshot->model = ((base_family == 0x06U) || (base_family == 0x0FU)) ?
        ((ext_model << 4U) | base_model) : base_model;
    snapshot->stepping = eax & UINT32_C(0x0F);
    snapshot->raw_identity0 = eax;
    snapshot->raw_identity1 = ebx;
}

static enum preboot_isa_error preboot_amd64_capture_basic(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t *max_basic,
    uint32_t *leaf1_eax,
    uint32_t *leaf1_ebx,
    uint32_t *leaf1_ecx,
    uint32_t *leaf1_edx)
{
    uint32_t eax = 0U;
    uint32_t ebx = 0U;
    uint32_t ecx = 0U;
    uint32_t edx = 0U;
    enum preboot_isa_error result = PREBOOT_ISA_OK;

    arch_isa_cpuid(0U, 0U, &eax, &ebx, &ecx, &edx);
    *max_basic = eax;
    preboot_amd64_set_vendor(snapshot, ebx, edx, ecx);
    result = preboot_amd64_capture_leaf(snapshot, 0U, 0U);
    if ((result != PREBOOT_ISA_OK) || (*max_basic < 1U)) {
        return (result == PREBOOT_ISA_OK) ?
            PREBOOT_ISA_E_UNSUPPORTED : result;
    }
    arch_isa_cpuid(1U, 0U, leaf1_eax, leaf1_ebx, leaf1_ecx, leaf1_edx);
    preboot_amd64_decode_identity(snapshot, *leaf1_eax, *leaf1_ebx);
    result = preboot_amd64_capture_leaf(snapshot, 1U, 0U);
    if ((result == PREBOOT_ISA_OK) && (*max_basic >= 4U)) {
        result = preboot_amd64_capture_cache_leaf(snapshot, 4U);
    }
    if ((result == PREBOOT_ISA_OK) && (*max_basic >= 7U)) {
        result = preboot_amd64_capture_feature_leaf(snapshot);
    }
    if ((result == PREBOOT_ISA_OK) && (*max_basic >= 11U)) {
        result = preboot_amd64_capture_topology_leaf(snapshot, 11U);
    }
    if ((result == PREBOOT_ISA_OK) && (*max_basic >= 13U)) {
        result = preboot_amd64_capture_leaf(snapshot, 13U, 0U);
        if (result == PREBOOT_ISA_OK) {
            result = preboot_amd64_capture_leaf(snapshot, 13U, 1U);
        }
    }
    if ((result == PREBOOT_ISA_OK) && (*max_basic >= 31U)) {
        result = preboot_amd64_capture_topology_leaf(snapshot, 31U);
    }
    return result;
}


static enum preboot_isa_error preboot_amd64_capture_extended_leaves(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t max_extended,
    uint32_t *extended_leaf1_ecx,
    uint32_t *extended_leaf1_edx)
{
    uint32_t eax = 0U;
    uint32_t ebx = 0U;
    enum preboot_isa_error result = PREBOOT_ISA_OK;

    arch_isa_cpuid(UINT32_C(0x80000001), 0U,
                        &eax, &ebx, extended_leaf1_ecx, extended_leaf1_edx);
    result = preboot_amd64_capture_leaf(snapshot, UINT32_C(0x80000001), 0U);
    if (result == PREBOOT_ISA_OK) {
        result = preboot_amd64_capture_leaf(snapshot, UINT32_C(0x80000007), 0U);
    }
    if (result == PREBOOT_ISA_OK) {
        result = preboot_amd64_capture_leaf(snapshot, UINT32_C(0x80000008), 0U);
    }
    if ((result == PREBOOT_ISA_OK) &&
        (max_extended >= UINT32_C(0x8000000A))) {
        result = preboot_amd64_capture_leaf(snapshot, UINT32_C(0x8000000A), 0U);
    }
    if ((result == PREBOOT_ISA_OK) &&
        (max_extended >= UINT32_C(0x8000001D))) {
        result = preboot_amd64_capture_cache_leaf(snapshot, UINT32_C(0x8000001D));
    }
    if ((result == PREBOOT_ISA_OK) &&
        (max_extended >= UINT32_C(0x8000001E))) {
        result = preboot_amd64_capture_leaf(snapshot, UINT32_C(0x8000001E), 0U);
    }
    if ((result == PREBOOT_ISA_OK) &&
        (max_extended >= UINT32_C(0x8000001F))) {
        result = preboot_amd64_capture_leaf(snapshot, UINT32_C(0x8000001F), 0U);
    }
    return result;
}

static enum preboot_isa_error preboot_amd64_decode_extended(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t leaf1_edx,
    uint32_t extended_leaf1_edx)
{
    uint32_t eax = 0U;
    uint32_t ebx = 0U;
    uint32_t ecx = 0U;
    uint32_t edx = 0U;

    arch_isa_cpuid(UINT32_C(0x80000008), 0U, &eax, &ebx, &ecx, &edx);
    snapshot->physical_address_bits = eax & UINT32_C(0xFF);
    snapshot->virtual_address_bits = (eax >> 8U) & UINT32_C(0xFF);
    if ((snapshot->physical_address_bits < 32U) ||
        (snapshot->physical_address_bits > 64U) ||
        (snapshot->virtual_address_bits < 32U) ||
        (snapshot->virtual_address_bits > 64U)) {
        return PREBOOT_ISA_E_UNSUPPORTED;
    }
    snapshot->page_granule_mask = PREBOOT_ISA_GRANULE_4K;
    if ((leaf1_edx & (UINT32_C(1) << 3U)) != 0U) {
        snapshot->page_granule_mask |= PREBOOT_ISA_GRANULE_2M;
    }
    if ((extended_leaf1_edx & (UINT32_C(1) << 26U)) != 0U) {
        snapshot->page_granule_mask |= PREBOOT_ISA_GRANULE_1G;
    }
    return PREBOOT_ISA_OK;
}

static enum preboot_isa_error preboot_amd64_capture_extended(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t *extended_leaf1_ecx,
    uint32_t *extended_leaf1_edx,
    uint32_t leaf1_edx)
{
    uint32_t max_extended = 0U;
    uint32_t ebx = 0U;
    uint32_t ecx = 0U;
    uint32_t edx = 0U;
    enum preboot_isa_error result = PREBOOT_ISA_OK;

    arch_isa_cpuid(UINT32_C(0x80000000), 0U,
                        &max_extended, &ebx, &ecx, &edx);
    result = preboot_amd64_capture_leaf(snapshot, UINT32_C(0x80000000), 0U);
    if ((result != PREBOOT_ISA_OK) ||
        (max_extended < UINT32_C(0x80000008))) {
        return (result == PREBOOT_ISA_OK) ?
            PREBOOT_ISA_E_UNSUPPORTED : result;
    }
    result = preboot_amd64_capture_extended_leaves(snapshot, max_extended,
        extended_leaf1_ecx, extended_leaf1_edx);
    if (result != PREBOOT_ISA_OK) return result;
    return preboot_amd64_decode_extended(snapshot, leaf1_edx,
                                         *extended_leaf1_edx);
}

static enum preboot_isa_error preboot_amd64_capture_state(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t leaf1_ecx,
    uint32_t extended_leaf1_ecx)
{
    uint64_t rflags = arch_isa_rflags();
    uint64_t cr0 = arch_isa_cr0();
    uint64_t cr4 = arch_isa_cr4();
    uint64_t efer = arch_isa_rdmsr(UINT32_C(0xC0000080));
    enum preboot_isa_error result = PREBOOT_ISA_OK;

    if ((rflags & (UINT64_C(1) << 9U)) != 0U) {
        snapshot->state_flags |= PREBOOT_ISA_STATE_INTERRUPTS_ENABLED;
    }
    if ((cr0 & (UINT64_C(1) << 31U)) != 0U) {
        snapshot->state_flags |= PREBOOT_ISA_STATE_MMU_ENABLED;
    }
    if ((cr4 & (UINT64_C(1) << 12U)) != 0U) {
        snapshot->state_flags |= PREBOOT_ISA_STATE_LA57_ENABLED;
    }
    if ((efer & (UINT64_C(1) << 11U)) != 0U) {
        snapshot->state_flags |= PREBOOT_ISA_STATE_NX_ENABLED;
    }
    if (((leaf1_ecx & (UINT32_C(1) << 5U)) != 0U) ||
        ((extended_leaf1_ecx & (UINT32_C(1) << 2U)) != 0U)) {
        snapshot->state_flags |= PREBOOT_ISA_STATE_HYPERVISOR_SUPPORTED;
    }
    if ((leaf1_ecx & (UINT32_C(1) << 31U)) != 0U) {
        snapshot->state_flags |= PREBOOT_ISA_STATE_GUEST_EXECUTION;
    }
    result = preboot_amd64_capture_control(
        snapshot, PREBOOT_AMD64_REG_CR0, cr0);
    if (result == PREBOOT_ISA_OK) {
        result = preboot_amd64_capture_control(
            snapshot, PREBOOT_AMD64_REG_RFLAGS, rflags);
    }
    if (result == PREBOOT_ISA_OK) {
        result = preboot_amd64_capture_control(
            snapshot, PREBOOT_AMD64_REG_CR4, cr4);
    }
    if (result == PREBOOT_ISA_OK) {
        result = preboot_isa_raw_append(snapshot, PREBOOT_ISA_REGSPACE_X86_MSR,
            UINT32_C(0xC0000080), 0U, PREBOOT_ISA_ACCESS_READ, efer);
    }
    return result;
}

enum preboot_isa_error preboot_isa_discover(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t logical_cpu_id)
{
    uint32_t max_basic = 0U;
    uint32_t leaf1_eax = 0U;
    uint32_t leaf1_ebx = 0U;
    uint32_t leaf1_ecx = 0U;
    uint32_t leaf1_edx = 0U;
    uint32_t extended_leaf1_ecx = 0U;
    uint32_t extended_leaf1_edx = 0U;
    enum preboot_isa_error result = PREBOOT_ISA_OK;

    if (snapshot == NULL) return PREBOOT_ISA_E_NULL_INPUT;
    if (arch_isa_cpl() != 0U) return PREBOOT_ISA_E_PRIVILEGE;
    preboot_isa_snapshot_init(snapshot, PREBOOT_ISA_ARCH_AMD64,
                              logical_cpu_id);
    result = preboot_amd64_capture_basic(snapshot, &max_basic,
        &leaf1_eax, &leaf1_ebx, &leaf1_ecx, &leaf1_edx);
    if (result == PREBOOT_ISA_OK) {
        result = preboot_amd64_capture_extended(snapshot,
            &extended_leaf1_ecx, &extended_leaf1_edx, leaf1_edx);
    }
    if (result == PREBOOT_ISA_OK) {
        result = preboot_amd64_capture_state(snapshot, leaf1_ecx,
                                             extended_leaf1_ecx);
    }
    if (result != PREBOOT_ISA_OK) return result;
    return preboot_isa_snapshot_validate(snapshot);
}
