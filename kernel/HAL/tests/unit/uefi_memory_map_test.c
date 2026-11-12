#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "common/hcd_endian.h"
#include "common/hcd_wire.h"
#include "capability/uefi_memory_map.h"

#define TEST_DESCRIPTOR_SIZE ((size_t)40U)
#define TEST_EXTENDED_DESCRIPTOR_SIZE ((size_t)48U)
#define TEST_EFI_MEMORY_WB UINT64_C(0x0000000000000008)
#define TEST_EFI_MEMORY_WP UINT64_C(0x0000000000001000)
#define TEST_EFI_MEMORY_RP UINT64_C(0x0000000000002000)
#define TEST_EFI_MEMORY_XP UINT64_C(0x0000000000004000)
#define TEST_EFI_MEMORY_NV UINT64_C(0x0000000000008000)
#define TEST_EFI_MEMORY_RO UINT64_C(0x0000000000020000)
#define TEST_EFI_MEMORY_RUNTIME UINT64_C(0x8000000000000000)

static void write_descriptor(
    uint8_t *buffer,
    size_t stride,
    uint32_t index,
    uint32_t type,
    uint64_t base,
    uint64_t pages,
    uint64_t attributes)
{
    uint64_t offset = (uint64_t)index * (uint64_t)stride;
    hcd_store_le32(buffer, offset, type);
    hcd_store_le32(buffer, offset + 4U, 0U);
    hcd_store_le64(buffer, offset + 8U, base);
    hcd_store_le64(buffer, offset + 16U, 0U);
    hcd_store_le64(buffer, offset + 24U, pages);
    hcd_store_le64(buffer, offset + 32U, attributes);
}

static struct preboot_uefi_exit_map make_map(
    void *buffer,
    size_t size,
    size_t descriptor_size,
    uint32_t exit_attempts)
{
    struct preboot_uefi_exit_map map = {
        buffer,
        size,
        UINT64_C(1),
        descriptor_size,
        PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1,
        exit_attempts,
        0U
    };
    return map;
}

static void test_sort_and_coalesce(void)
{
    uint8_t buffer[TEST_DESCRIPTOR_SIZE * 3U] = {0U};
    struct preboot_uefi_exit_map map;
    uint32_t count = UINT32_MAX;

    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 0U, 7U, 0x3000U, 1U,
                     TEST_EFI_MEMORY_WB);
    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 1U, 7U, 0x1000U, 1U,
                     TEST_EFI_MEMORY_WB);
    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 2U, 7U, 0x2000U, 1U,
                     TEST_EFI_MEMORY_WB);
    map = make_map(buffer, sizeof(buffer), TEST_DESCRIPTOR_SIZE, 1U);

    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) == PREBOOT_UEFI_MEMORY_MAP_OK);
    assert(count == 1U);
    assert(hcd_load_le64(buffer, 0U) == UINT64_C(0x1000));
    assert(hcd_load_le64(buffer, 8U) == UINT64_C(0x3000));
    assert(hcd_load_le32(buffer, 16U) == HCD_MEMORY_USABLE);
    assert((hcd_load_le32(buffer, 20U) & HCD_MEMORY_A_CACHE_WB) != 0U);
    assert(hcd_load_le32(buffer, 24U) == HCD_ID_UNKNOWN);
    assert(hcd_load_le32(buffer, 28U) == HCD_ID_UNKNOWN);
    assert(hcd_load_le64(buffer, 32U) == 0U);
}

static void test_descriptor_stride_and_attributes(void)
{
    uint8_t buffer[TEST_EXTENDED_DESCRIPTOR_SIZE * 2U];
    struct preboot_uefi_exit_map map;
    uint32_t count = 0U;
    uint32_t index;

    for (index = 0U; index < (uint32_t)sizeof(buffer); ++index) {
        buffer[index] = UINT8_C(0xA5);
    }
    write_descriptor(buffer, TEST_EXTENDED_DESCRIPTOR_SIZE, 0U, 5U,
                     0x5000U, 1U, TEST_EFI_MEMORY_RUNTIME | TEST_EFI_MEMORY_WP);
    write_descriptor(buffer, TEST_EXTENDED_DESCRIPTOR_SIZE, 1U, 14U,
                     0x9000U, 2U, TEST_EFI_MEMORY_NV | TEST_EFI_MEMORY_XP);
    map = make_map(buffer, sizeof(buffer), TEST_EXTENDED_DESCRIPTOR_SIZE, 1U);

    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) == PREBOOT_UEFI_MEMORY_MAP_OK);
    assert(count == 2U);
    assert(hcd_load_le32(buffer, 16U) == HCD_MEMORY_FIRMWARE_RUNTIME);
    assert((hcd_load_le32(buffer, 20U) & HCD_MEMORY_A_RUNTIME) != 0U);
    assert((hcd_load_le32(buffer, 20U) & HCD_MEMORY_A_WRITE) == 0U);
    assert(hcd_load_le32(buffer, 56U) == HCD_MEMORY_PERSISTENT);
    assert((hcd_load_le32(buffer, 60U) & HCD_MEMORY_A_PERSISTENT) != 0U);
    assert((hcd_load_le32(buffer, 60U) & HCD_MEMORY_A_EXECUTE) == 0U);
}

static void test_access_restrictions(void)
{
    uint8_t buffer[TEST_DESCRIPTOR_SIZE] = {0U};
    struct preboot_uefi_exit_map map;
    uint32_t count = 0U;
    uint32_t attributes;

    write_descriptor(
        buffer, TEST_DESCRIPTOR_SIZE, 0U, 7U, 0x1000U, 1U,
        TEST_EFI_MEMORY_WB | TEST_EFI_MEMORY_RO | TEST_EFI_MEMORY_RP |
            TEST_EFI_MEMORY_XP);
    map = make_map(buffer, sizeof(buffer), TEST_DESCRIPTOR_SIZE, 1U);
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) == PREBOOT_UEFI_MEMORY_MAP_OK);
    assert(count == 1U);
    attributes = hcd_load_le32(buffer, 20U);
    assert((attributes & HCD_MEMORY_A_READ) == 0U);
    assert((attributes & HCD_MEMORY_A_WRITE) == 0U);
    assert((attributes & HCD_MEMORY_A_EXECUTE) == 0U);
    assert((attributes & HCD_MEMORY_A_CACHE_WB) != 0U);
}

static void test_all_memory_type_mappings(void)
{
    uint8_t buffer[TEST_DESCRIPTOR_SIZE * 9U] = {0U};
    struct preboot_uefi_exit_map map;
    const uint32_t source_types[9] = {0U, 1U, 3U, 8U, 9U, 10U, 11U, 14U, 15U};
    const uint32_t target_types[9] = {
        HCD_MEMORY_RESERVED,
        HCD_MEMORY_BOOTSTRAP,
        HCD_MEMORY_USABLE,
        HCD_MEMORY_UNUSABLE,
        HCD_MEMORY_ACPI_RECLAIMABLE,
        HCD_MEMORY_ACPI_NVS,
        HCD_MEMORY_MMIO,
        HCD_MEMORY_PERSISTENT,
        HCD_MEMORY_UNACCEPTED
    };
    uint32_t count = 0U;
    uint32_t index;

    for (index = 0U; index < 9U; ++index) {
        write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, index,
                         source_types[index],
                         UINT64_C(0x1000) + ((uint64_t)index * UINT64_C(0x2000)),
                         1U, 0U);
    }
    map = make_map(buffer, sizeof(buffer), TEST_DESCRIPTOR_SIZE, 1U);

    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) == PREBOOT_UEFI_MEMORY_MAP_OK);
    assert(count == 9U);
    for (index = 0U; index < count; ++index) {
        assert(hcd_load_le32(
                   buffer, ((uint64_t)index * UINT64_C(40)) + 16U) ==
               target_types[index]);
    }
}

static void test_more_than_hcd_limit_can_coalesce(void)
{
    const uint32_t descriptor_count = HCD_MAX_MEMORY_REGIONS + 1U;
    const size_t map_size =
        (size_t)descriptor_count * TEST_DESCRIPTOR_SIZE;
    static uint8_t buffer[(HCD_MAX_MEMORY_REGIONS + 1U) * 40U];
    struct preboot_uefi_exit_map map;
    uint32_t count = 0U;
    uint32_t index;

    for (index = 0U; index < descriptor_count; ++index) {
        write_descriptor(
            buffer, TEST_DESCRIPTOR_SIZE, index, 7U,
            UINT64_C(0x100000) + ((uint64_t)index * UINT64_C(0x1000)),
            1U, TEST_EFI_MEMORY_WB);
    }
    map = make_map(buffer, map_size, TEST_DESCRIPTOR_SIZE, 1U);
    assert(preboot_uefi_normalize_memory_map(
               &map, map_size, &count) == PREBOOT_UEFI_MEMORY_MAP_OK);
    assert(count == 1U);
    assert(hcd_load_le64(buffer, 8U) ==
           (uint64_t)descriptor_count * UINT64_C(0x1000));
}

static void test_rejects_bad_ownership_and_bounds(void)
{
    uint8_t buffer[TEST_DESCRIPTOR_SIZE] = {0U};
    struct preboot_uefi_exit_map map =
        make_map(buffer, sizeof(buffer), TEST_DESCRIPTOR_SIZE, 0U);
    uint32_t count = UINT32_MAX;

    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_EBS_NOT_COMPLETE);
    assert(count == 0U);
    map.exit_attempts = 1U;
    map.descriptor_version = 2U;
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_BAD_VERSION);
    map.descriptor_version = PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1;
    map.descriptor_size = 39U;
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_BAD_DESCRIPTOR_SIZE);
    map.descriptor_size = TEST_DESCRIPTOR_SIZE;
    map.size = TEST_DESCRIPTOR_SIZE - 1U;
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_BAD_MAP_SIZE);
    map.size = sizeof(buffer);
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer) - 1U, &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_BAD_MAP_SIZE);
}

static void test_rejects_malformed_descriptors(void)
{
    uint8_t buffer[TEST_DESCRIPTOR_SIZE * 2U] = {0U};
    struct preboot_uefi_exit_map map;
    uint32_t count = 0U;

    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 0U, 7U, 0x1000U, 0U, 0U);
    map = make_map(buffer, TEST_DESCRIPTOR_SIZE, TEST_DESCRIPTOR_SIZE, 1U);
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_BAD_DESCRIPTOR);

    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 0U, 99U, 0x1000U, 1U, 0U);
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_UNSUPPORTED_TYPE);

    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 0U, 7U, 0x1000U, 1U,
                     UINT64_C(0x0000000000000018));
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_BAD_DESCRIPTOR);

    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 0U, 7U, 0x1000U, 1U,
                     UINT64_C(0x0000000000080000));
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_UNSUPPORTED_ATTRIBUTES);

    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 0U, 7U, 0x1000U, 1U,
                     0U);
    hcd_store_le32(buffer, 4U, 1U);
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_BAD_DESCRIPTOR);

    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 0U, 7U, 0x1000U,
                     (UINT64_MAX / UINT64_C(4096)) + 1U, 0U);
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_BAD_DESCRIPTOR);

    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 0U, 12U, 0x1000U, 1U, 0U);
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_UNSUPPORTED_TYPE);
}

static void test_rejects_overlap_and_overflow(void)
{
    uint8_t buffer[TEST_DESCRIPTOR_SIZE * 2U] = {0U};
    struct preboot_uefi_exit_map map;
    uint32_t count = 0U;

    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 0U, 7U, 0x2000U, 2U, 0U);
    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 1U, 7U, 0x1000U, 2U, 0U);
    map = make_map(buffer, sizeof(buffer), TEST_DESCRIPTOR_SIZE, 1U);
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_OVERLAP);
    assert(count == 0U);

    write_descriptor(buffer, TEST_DESCRIPTOR_SIZE, 0U, 7U,
                     UINT64_MAX - UINT64_C(0xFFF), 1U, 0U);
    map = make_map(buffer, TEST_DESCRIPTOR_SIZE, TEST_DESCRIPTOR_SIZE, 1U);
    assert(preboot_uefi_normalize_memory_map(
               &map, sizeof(buffer), &count) ==
           PREBOOT_UEFI_MEMORY_MAP_E_BAD_DESCRIPTOR);
}

int main(void)
{
    test_sort_and_coalesce();
    test_descriptor_stride_and_attributes();
    test_access_restrictions();
    test_all_memory_type_mappings();
    test_more_than_hcd_limit_can_coalesce();
    test_rejects_bad_ownership_and_bounds();
    test_rejects_malformed_descriptors();
    test_rejects_overlap_and_overflow();
    return 0;
}
