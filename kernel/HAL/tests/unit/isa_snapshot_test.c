#include <stddef.h>

#include "capability/isa.h"

static int test_snapshot_amd64_validates(void)
{
    struct preboot_isa_cpu_snapshot snapshot;
    uint32_t index = 0U;

    preboot_isa_snapshot_init(&snapshot, PREBOOT_ISA_ARCH_AMD64, 0U);
    snapshot.privilege_level = 0U;
    snapshot.physical_address_bits = 52U;
    snapshot.virtual_address_bits = 48U;
    snapshot.page_granule_mask = PREBOOT_ISA_GRANULE_4K |
        PREBOOT_ISA_GRANULE_2M;
    snapshot.vendor[0] = 'T';
    snapshot.vendor[1] = 'E';
    snapshot.vendor[2] = 'S';
    snapshot.vendor[3] = 'T';
    if (preboot_isa_raw_append(&snapshot,
            PREBOOT_ISA_REGSPACE_X86_CPUID_EAX, 0U, 0U,
            PREBOOT_ISA_ACCESS_READ, 1U) != PREBOOT_ISA_OK) {
        return 1;
    }
    if (preboot_isa_snapshot_validate(&snapshot) != PREBOOT_ISA_OK) {
        return 2;
    }
    if (preboot_isa_raw_append(&snapshot,
            PREBOOT_ISA_REGSPACE_X86_CPUID_EAX, 0U, 0U,
            PREBOOT_ISA_ACCESS_READ, 1U) != PREBOOT_ISA_E_RAW_DUPLICATE) {
        return 3;
    }
    for (index = 1U; index < PREBOOT_ISA_MAX_RAW_REGISTERS; ++index) {
        if (preboot_isa_raw_append(&snapshot,
                PREBOOT_ISA_REGSPACE_X86_CPUID_EBX, index, 0U,
                PREBOOT_ISA_ACCESS_READ, index) != PREBOOT_ISA_OK) {
            return 4;
        }
    }
    if (preboot_isa_raw_append(&snapshot,
            PREBOOT_ISA_REGSPACE_X86_CPUID_ECX, 0U, 0U,
            PREBOOT_ISA_ACCESS_READ, 0U) != PREBOOT_ISA_E_RAW_CAPACITY) {
        return 5;
    }
    return 0;
}

static int test_snapshot_arm64_validates(void)
{
    struct preboot_isa_cpu_snapshot snapshot;

    preboot_isa_snapshot_init(&snapshot, PREBOOT_ISA_ARCH_ARM64, 0U);
    snapshot.privilege_level = 1U;
    snapshot.physical_address_bits = 48U;
    snapshot.virtual_address_bits = 48U;
    snapshot.page_granule_mask = PREBOOT_ISA_GRANULE_4K |
        PREBOOT_ISA_GRANULE_16K | PREBOOT_ISA_GRANULE_64K;
    snapshot.vendor[0] = UINT8_C(0x41);
    if (preboot_isa_raw_append(&snapshot, PREBOOT_ISA_REGSPACE_ARM_SYSREG,
            1U, 0U, PREBOOT_ISA_ACCESS_READ, UINT64_C(0x410FD0B0)) !=
        PREBOOT_ISA_OK) {
        return 1;
    }
    if (preboot_isa_snapshot_validate(&snapshot) != PREBOOT_ISA_OK) {
        return 2;
    }
    return 0;
}

static int test_snapshot_rejects_wrong_privilege_and_reserved_bits(void)
{
    struct preboot_isa_cpu_snapshot snapshot;

    preboot_isa_snapshot_init(&snapshot, PREBOOT_ISA_ARCH_AMD64, 0U);
    snapshot.privilege_level = 3U;
    snapshot.physical_address_bits = 48U;
    snapshot.virtual_address_bits = 48U;
    snapshot.page_granule_mask = PREBOOT_ISA_GRANULE_4K;
    snapshot.vendor[0] = 'X';
    if (preboot_isa_raw_append(&snapshot, PREBOOT_ISA_REGSPACE_X86_CPUID_EAX,
            0U, 0U, PREBOOT_ISA_ACCESS_READ, 1U) != PREBOOT_ISA_OK) {
        return 1;
    }
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 2;
    }
    snapshot.privilege_level = 0U;
    snapshot.reserved0 = 1U;
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 3;
    }
    return 0;
}

static int initialize_valid_amd64_snapshot(
    struct preboot_isa_cpu_snapshot *snapshot)
{
    preboot_isa_snapshot_init(snapshot, PREBOOT_ISA_ARCH_AMD64, 0U);
    snapshot->physical_address_bits = 52U;
    snapshot->virtual_address_bits = 48U;
    snapshot->page_granule_mask = PREBOOT_ISA_GRANULE_4K;
    snapshot->vendor[0] = 'T';
    return (int)preboot_isa_raw_append(snapshot,
        PREBOOT_ISA_REGSPACE_X86_CPUID_EAX, 0U, 0U,
        PREBOOT_ISA_ACCESS_READ, 1U);
}

static int test_raw_append_rejects_invalid_access_state(void)
{
    struct preboot_isa_cpu_snapshot snapshot;

    preboot_isa_snapshot_init(&snapshot, PREBOOT_ISA_ARCH_AMD64, 0U);
    if (preboot_isa_raw_append(NULL, PREBOOT_ISA_REGSPACE_X86_CPUID_EAX,
            0U, 0U, PREBOOT_ISA_ACCESS_READ, 1U) !=
        PREBOOT_ISA_E_NULL_INPUT) {
        return 1;
    }
    if (preboot_isa_raw_append(&snapshot, PREBOOT_ISA_REGSPACE_X86_CPUID_EAX,
            0U, 0U, 0U, 0U) != PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 2;
    }
    if (preboot_isa_raw_append(&snapshot, PREBOOT_ISA_REGSPACE_X86_CPUID_EAX,
            0U, 0U, PREBOOT_ISA_ACCESS_UNAVAILABLE, 1U) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 3;
    }
    if (snapshot.raw_register_count != 0U) {
        return 4;
    }
    if (preboot_isa_snapshot_validate(NULL) != PREBOOT_ISA_E_NULL_INPUT) {
        return 5;
    }
    return 0;
}

static int test_snapshot_rejects_invalid_metadata(void)
{
    struct preboot_isa_cpu_snapshot snapshot;

    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 1;
    }
    snapshot.state_flags = UINT32_C(0x80000000);
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 2;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 3;
    }
    snapshot.page_granule_mask |= UINT32_C(0x20);
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 4;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 5;
    }
    snapshot.raw[0].register_space = UINT32_C(99);
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 6;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 7;
    }
    snapshot.raw[0].register_space = PREBOOT_ISA_REGSPACE_ARM_SYSREG;
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 8;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 9;
    }
    snapshot.raw[0].access_state = 0U;
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 10;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 11;
    }
    snapshot.raw[0].access_state = PREBOOT_ISA_ACCESS_UNAVAILABLE;
    snapshot.raw[0].value = 1U;
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 12;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 13;
    }
    snapshot.raw_register_count = PREBOOT_ISA_MAX_RAW_REGISTERS + 1U;
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 14;
    }
    return 0;
}


static int test_snapshot_rejects_missing_or_out_of_range_required_fields(void)
{
    struct preboot_isa_cpu_snapshot snapshot;

    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 1;
    }
    snapshot.abi_version = PREBOOT_ISA_ABI_VERSION + 1U;
    if (preboot_isa_snapshot_validate(&snapshot) != PREBOOT_ISA_E_BAD_ABI) {
        return 2;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 3;
    }
    snapshot.architecture = UINT32_C(99);
    if (preboot_isa_snapshot_validate(&snapshot) != PREBOOT_ISA_E_BAD_ARCH) {
        return 4;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 5;
    }
    snapshot.logical_cpu_id = UINT32_MAX;
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 6;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 7;
    }
    snapshot.physical_address_bits = 31U;
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 8;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 9;
    }
    snapshot.virtual_address_bits = 65U;
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 10;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 11;
    }
    snapshot.page_granule_mask = 0U;
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 12;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 13;
    }
    snapshot.raw_register_count = 0U;
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 14;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 15;
    }
    snapshot.reserved1 = 1U;
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 16;
    }
    if (initialize_valid_amd64_snapshot(&snapshot) != PREBOOT_ISA_OK) {
        return 17;
    }
    snapshot.vendor[0] = 0U;
    if (preboot_isa_snapshot_validate(&snapshot) !=
        PREBOOT_ISA_E_INVALID_SNAPSHOT) {
        return 18;
    }
    return 0;
}

int main(void)
{
    if (test_snapshot_amd64_validates() != 0) {
        return 1;
    }
    if (test_snapshot_arm64_validates() != 0) {
        return 2;
    }
    if (test_snapshot_rejects_wrong_privilege_and_reserved_bits() != 0) {
        return 3;
    }
    if (test_raw_append_rejects_invalid_access_state() != 0) {
        return 4;
    }
    if (test_snapshot_rejects_invalid_metadata() != 0) {
        return 5;
    }
    if (test_snapshot_rejects_missing_or_out_of_range_required_fields() != 0) {
        return 6;
    }
    return 0;
}
