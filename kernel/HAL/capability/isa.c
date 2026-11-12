#include <stddef.h>
#include <stdint.h>

#include "isa.h"

void preboot_isa_snapshot_init(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t architecture,
    uint32_t logical_cpu_id)
{
    uint32_t index = 0U;

    snapshot->abi_version = PREBOOT_ISA_ABI_VERSION;
    snapshot->architecture = architecture;
    snapshot->logical_cpu_id = logical_cpu_id;
    snapshot->privilege_level = 0U;
    snapshot->family = 0U;
    snapshot->model = 0U;
    snapshot->stepping = 0U;
    snapshot->physical_address_bits = 0U;
    snapshot->virtual_address_bits = 0U;
    snapshot->page_granule_mask = 0U;
    snapshot->state_flags = 0U;
    snapshot->raw_register_count = 0U;
    for (index = 0U; index < 16U; ++index) {
        snapshot->vendor[index] = 0U;
    }
    snapshot->raw_identity0 = 0U;
    snapshot->raw_identity1 = 0U;
    snapshot->reserved0 = 0U;
    snapshot->reserved1 = 0U;
    for (index = 0U; index < PREBOOT_ISA_MAX_RAW_REGISTERS; ++index) {
        snapshot->raw[index].register_space = 0U;
        snapshot->raw[index].selector = 0U;
        snapshot->raw[index].subselector = 0U;
        snapshot->raw[index].access_state = 0U;
        snapshot->raw[index].value = 0U;
    }
}

enum preboot_isa_error preboot_isa_raw_append(
    struct preboot_isa_cpu_snapshot *snapshot,
    uint32_t register_space,
    uint32_t selector,
    uint32_t subselector,
    uint32_t access_state,
    uint64_t value)
{
    uint32_t index = 0U;
    struct preboot_isa_raw_register *record = NULL;

    if (snapshot == NULL) {
        return PREBOOT_ISA_E_NULL_INPUT;
    }
    if (snapshot->raw_register_count >= PREBOOT_ISA_MAX_RAW_REGISTERS) {
        return PREBOOT_ISA_E_RAW_CAPACITY;
    }
    if (((access_state != PREBOOT_ISA_ACCESS_READ) &&
         (access_state != PREBOOT_ISA_ACCESS_UNAVAILABLE)) ||
        ((access_state == PREBOOT_ISA_ACCESS_UNAVAILABLE) && (value != 0U))) {
        return PREBOOT_ISA_E_INVALID_SNAPSHOT;
    }
    for (index = 0U; index < snapshot->raw_register_count; ++index) {
        const struct preboot_isa_raw_register *existing =
            &snapshot->raw[index];
        if ((existing->register_space == register_space) &&
            (existing->selector == selector) &&
            (existing->subselector == subselector)) {
            return PREBOOT_ISA_E_RAW_DUPLICATE;
        }
    }
    record = &snapshot->raw[snapshot->raw_register_count];
    record->register_space = register_space;
    record->selector = selector;
    record->subselector = subselector;
    record->access_state = access_state;
    record->value = value;
    snapshot->raw_register_count++;
    return PREBOOT_ISA_OK;
}

static int preboot_isa_vendor_present(
    const struct preboot_isa_cpu_snapshot *snapshot)
{
    uint32_t index = 0U;

    for (index = 0U; index < 16U; ++index) {
        if (snapshot->vendor[index] != 0U) {
            return 1;
        }
    }
    return 0;
}

static enum preboot_isa_error preboot_isa_validate_architecture(
    const struct preboot_isa_cpu_snapshot *snapshot)
{
    if (snapshot->architecture == PREBOOT_ISA_ARCH_AMD64) {
        if (snapshot->privilege_level != 0U) {
            return PREBOOT_ISA_E_INVALID_SNAPSHOT;
        }
        if ((snapshot->page_granule_mask &
             ~(PREBOOT_ISA_GRANULE_4K | PREBOOT_ISA_GRANULE_2M |
               PREBOOT_ISA_GRANULE_1G)) != 0U) {
            return PREBOOT_ISA_E_INVALID_SNAPSHOT;
        }
    } else if (snapshot->architecture == PREBOOT_ISA_ARCH_ARM64) {
        if ((snapshot->privilege_level < 1U) ||
            (snapshot->privilege_level > 3U)) {
            return PREBOOT_ISA_E_INVALID_SNAPSHOT;
        }
        if ((snapshot->page_granule_mask &
             ~(PREBOOT_ISA_GRANULE_4K | PREBOOT_ISA_GRANULE_16K |
               PREBOOT_ISA_GRANULE_64K)) != 0U) {
            return PREBOOT_ISA_E_INVALID_SNAPSHOT;
        }
    } else {
        return PREBOOT_ISA_E_BAD_ARCH;
    }
    return PREBOOT_ISA_OK;
}

static enum preboot_isa_error preboot_isa_validate_raw(
    const struct preboot_isa_cpu_snapshot *snapshot)
{
    uint32_t index = 0U;
    uint32_t previous = 0U;

    for (index = 0U; index < snapshot->raw_register_count; ++index) {
        const struct preboot_isa_raw_register *record =
            &snapshot->raw[index];
        if (((record->register_space < PREBOOT_ISA_REGSPACE_X86_CPUID_EAX) ||
             (record->register_space > PREBOOT_ISA_REGSPACE_X86_CONTROL)) &&
            (record->register_space != PREBOOT_ISA_REGSPACE_ARM_SYSREG)) {
            return PREBOOT_ISA_E_INVALID_SNAPSHOT;
        }
        if (((snapshot->architecture == PREBOOT_ISA_ARCH_AMD64) &&
             (record->register_space == PREBOOT_ISA_REGSPACE_ARM_SYSREG)) ||
            ((snapshot->architecture == PREBOOT_ISA_ARCH_ARM64) &&
             (record->register_space != PREBOOT_ISA_REGSPACE_ARM_SYSREG))) {
            return PREBOOT_ISA_E_INVALID_SNAPSHOT;
        }
        if (((record->access_state != PREBOOT_ISA_ACCESS_READ) &&
             (record->access_state != PREBOOT_ISA_ACCESS_UNAVAILABLE))) {
            return PREBOOT_ISA_E_INVALID_SNAPSHOT;
        }
        if ((record->access_state == PREBOOT_ISA_ACCESS_UNAVAILABLE) &&
            (record->value != 0U)) {
            return PREBOOT_ISA_E_INVALID_SNAPSHOT;
        }
        for (previous = 0U; previous < index; ++previous) {
            const struct preboot_isa_raw_register *other =
                &snapshot->raw[previous];
            if ((other->register_space == record->register_space) &&
                (other->selector == record->selector) &&
                (other->subselector == record->subselector)) {
                return PREBOOT_ISA_E_RAW_DUPLICATE;
            }
        }
    }
    return PREBOOT_ISA_OK;
}

enum preboot_isa_error preboot_isa_snapshot_validate(
    const struct preboot_isa_cpu_snapshot *snapshot)
{
    enum preboot_isa_error result = PREBOOT_ISA_OK;

    if (snapshot == NULL) {
        return PREBOOT_ISA_E_NULL_INPUT;
    }
    if (snapshot->abi_version != PREBOOT_ISA_ABI_VERSION) {
        return PREBOOT_ISA_E_BAD_ABI;
    }
    if ((snapshot->architecture != PREBOOT_ISA_ARCH_AMD64) &&
        (snapshot->architecture != PREBOOT_ISA_ARCH_ARM64)) {
        return PREBOOT_ISA_E_BAD_ARCH;
    }
    if ((snapshot->logical_cpu_id == UINT32_MAX) ||
        (snapshot->physical_address_bits < 32U) ||
        (snapshot->physical_address_bits > 64U) ||
        (snapshot->virtual_address_bits < 32U) ||
        (snapshot->virtual_address_bits > 64U) ||
        (snapshot->page_granule_mask == 0U) ||
        ((snapshot->page_granule_mask & ~PREBOOT_ISA_GRANULE_KNOWN_MASK) != 0U) ||
        ((snapshot->state_flags & ~PREBOOT_ISA_STATE_KNOWN_MASK) != 0U) ||
        (snapshot->raw_register_count == 0U) ||
        (snapshot->raw_register_count > PREBOOT_ISA_MAX_RAW_REGISTERS) ||
        (snapshot->reserved0 != 0U) || (snapshot->reserved1 != 0U) ||
        (preboot_isa_vendor_present(snapshot) == 0)) {
        return PREBOOT_ISA_E_INVALID_SNAPSHOT;
    }
    result = preboot_isa_validate_architecture(snapshot);
    if (result != PREBOOT_ISA_OK) {
        return result;
    }
    return preboot_isa_validate_raw(snapshot);
}
