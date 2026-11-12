#include <stddef.h>
#include <stdint.h>
#include <arch/isa.h>
#include "../isa.h"

#if !defined(__aarch64__)
#error "ARM64 preboot HAL requires an AArch64 target"
#endif

struct preboot_arm64_register_values {
    uint64_t current_el;
    uint64_t midr;
    uint64_t mpidr;
    uint64_t pfr0;
    uint64_t isar0;
    uint64_t mmfr0;
    uint64_t mmfr1;
    uint64_t mmfr2;
    uint64_t ctr;
    uint64_t cntfrq;
    uint64_t daif;
    uint64_t sctlr;
    uint64_t tcr;
    uint64_t mair;
    uint64_t pc;
};

static enum preboot_isa_error preboot_arm64_capture(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t selector,
    uint64_t value)
{
    return preboot_isa_raw_append(snapshot, PREBOOT_ISA_REGSPACE_ARM_SYSREG,
        selector, 0U, PREBOOT_ISA_ACCESS_READ, value);
}

static uint32_t preboot_arm64_decode_pa_bits(uint32_t range)
{
    static const uint8_t widths[7] = {32U, 36U, 40U, 42U, 44U, 48U, 52U};

    if (range >= 7U) return 0U;
    return widths[range];
}

static void preboot_arm64_decode_vendor(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint64_t midr)
{
    snapshot->vendor[0] = (uint8_t)(midr >> 24U);
    snapshot->vendor[1] = (uint8_t)(midr >> 20U);
    snapshot->vendor[2] = (uint8_t)(midr >> 16U);
    snapshot->vendor[3] = (uint8_t)(midr >> 12U);
    snapshot->vendor[4] = (uint8_t)(midr >> 4U);
    snapshot->vendor[5] = (uint8_t)midr;
    snapshot->family = (uint32_t)((midr >> 24U) & UINT64_C(0xFF));
    snapshot->model = (uint32_t)((midr >> 4U) & UINT64_C(0xFFF));
    snapshot->stepping = (uint32_t)(midr & UINT64_C(0xF));
}

static enum preboot_isa_error preboot_arm64_read_registers(
    struct preboot_arm64_register_values *values,
    uint32_t el)
{
    values->midr = arch_isa_midr();
    values->mpidr = arch_isa_mpidr();
    values->pfr0 = arch_isa_pfr0();
    values->isar0 = arch_isa_isar0();
    values->mmfr0 = arch_isa_mmfr0();
    values->mmfr1 = arch_isa_mmfr1();
    values->mmfr2 = arch_isa_mmfr2();
    values->ctr = arch_isa_ctr();
    values->cntfrq = arch_isa_cntfrq();
    values->daif = arch_isa_daif();
    values->sctlr = arch_isa_sctlr(el);
    values->tcr = arch_isa_tcr(el);
    values->mair = arch_isa_mair(el);
    values->pc = arch_isa_pc();
    return PREBOOT_ISA_OK;
}

static enum preboot_isa_error preboot_arm64_decode_capabilities(
    struct preboot_isa_cpu_snapshot *snapshot,
    const struct preboot_arm64_register_values *values,
    uint32_t el)
{
    uint32_t pa_range = (uint32_t)(values->mmfr0 & UINT64_C(0xF));
    uint32_t granule4 = (uint32_t)((values->mmfr0 >> 28U) & UINT64_C(0xF));
    uint32_t granule64 = (uint32_t)((values->mmfr0 >> 24U) & UINT64_C(0xF));
    uint32_t granule16 = (uint32_t)((values->mmfr0 >> 20U) & UINT64_C(0xF));
    uint32_t varange = (uint32_t)((values->mmfr2 >> 16U) & UINT64_C(0xF));
    uint32_t el2_field = (uint32_t)((values->pfr0 >> 8U) & UINT64_C(0xF));
    uint32_t sve_field = (uint32_t)((values->pfr0 >> 32U) & UINT64_C(0xF));

    if (((granule4 > 1U) && (granule4 != 0xFU)) ||
        ((granule16 > 2U) && (granule16 != 0xFU)) ||
        ((granule64 != 0U) && (granule64 != 0xFU)) ||
        (varange > 1U) || (preboot_arm64_decode_pa_bits(pa_range) == 0U)) {
        return PREBOOT_ISA_E_UNSUPPORTED;
    }
    snapshot->privilege_level = el;
    snapshot->physical_address_bits = preboot_arm64_decode_pa_bits(pa_range);
    snapshot->virtual_address_bits = 48U;
    if ((granule4 == 0U) || (granule4 == 1U)) {
        snapshot->page_granule_mask |= PREBOOT_ISA_GRANULE_4K;
    }
    if ((granule16 == 1U) || (granule16 == 2U)) {
        snapshot->page_granule_mask |= PREBOOT_ISA_GRANULE_16K;
    }
    if (granule64 == 0U) {
        snapshot->page_granule_mask |= PREBOOT_ISA_GRANULE_64K;
    }
    if ((varange == 1U) && (granule64 == 0U)) {
        snapshot->virtual_address_bits = 52U;
    }
    if ((values->sctlr & UINT64_C(1)) != 0U) {
        snapshot->state_flags |= PREBOOT_ISA_STATE_MMU_ENABLED;
    }
    if ((values->daif & (UINT64_C(1) << 7U)) == 0U) {
        snapshot->state_flags |= PREBOOT_ISA_STATE_INTERRUPTS_ENABLED;
    }
    if ((el2_field == 1U) || (el2_field == 2U)) {
        snapshot->state_flags |= PREBOOT_ISA_STATE_HYPERVISOR_SUPPORTED;
    }
    if (el == 2U) {
        snapshot->state_flags |= PREBOOT_ISA_STATE_HYPERVISOR_EL_ACTIVE;
    }
    if (sve_field == 1U) {
        snapshot->state_flags |= PREBOOT_ISA_STATE_SVE_SUPPORTED;
    }
    if (snapshot->page_granule_mask == 0U) {
        return PREBOOT_ISA_E_UNSUPPORTED;
    }
    return PREBOOT_ISA_OK;
}

static enum preboot_isa_error preboot_arm64_capture_registers(
    struct preboot_isa_cpu_snapshot *snapshot,
    const struct preboot_arm64_register_values *values)
{
    enum preboot_isa_error result = PREBOOT_ISA_OK;

    result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_MIDR_EL1, values->midr);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_MPIDR_EL1, values->mpidr);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_CURRENT_EL, values->current_el);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_PFR0_EL1, values->pfr0);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_ISAR0_EL1, values->isar0);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_MMFR0_EL1, values->mmfr0);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_MMFR1_EL1, values->mmfr1);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_MMFR2_EL1, values->mmfr2);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_CTR_EL0, values->ctr);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_CNTFRQ_EL0, values->cntfrq);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_DAIF, values->daif);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_SCTLR, values->sctlr);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_TCR, values->tcr);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_MAIR, values->mair);
    if (result == PREBOOT_ISA_OK) result = preboot_arm64_capture(snapshot, PREBOOT_ARM64_REG_PC, values->pc);
    return result;
}

enum preboot_isa_error preboot_isa_discover(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t logical_cpu_id)
{
    struct preboot_arm64_register_values values;
    uint32_t el = 0U;
    enum preboot_isa_error result = PREBOOT_ISA_OK;

    if (snapshot == NULL) return PREBOOT_ISA_E_NULL_INPUT;
    values.current_el = arch_isa_current_el();
    el = (uint32_t)((values.current_el >> 2U) & UINT64_C(3));
    if ((el < 1U) || (el > 3U)) return PREBOOT_ISA_E_PRIVILEGE;
    preboot_isa_snapshot_init(snapshot, PREBOOT_ISA_ARCH_ARM64,
                              logical_cpu_id);
    result = preboot_arm64_read_registers(&values, el);
    if (result != PREBOOT_ISA_OK) return result;
    snapshot->raw_identity0 = values.midr;
    snapshot->raw_identity1 = values.mpidr;
    preboot_arm64_decode_vendor(snapshot, values.midr);
    result = preboot_arm64_decode_capabilities(snapshot, &values, el);
    if (result != PREBOOT_ISA_OK) return result;
    result = preboot_arm64_capture_registers(snapshot, &values);
    if (result != PREBOOT_ISA_OK) return result;
    return preboot_isa_snapshot_validate(snapshot);
}
