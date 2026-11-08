#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "common/hcd_endian.h"
#include "common/hcd_wire.h"
#include "capability/multiboot2_efi_map.h"
#include "capability/uefi_memory_map.h"

#define TEST_INFO_HEADER_SIZE ((size_t)8U)
#define TEST_EFI_MMAP_TAG_SIZE ((size_t)56U)
#define TEST_EFI_BS_TAG_SIZE ((size_t)8U)
#define TEST_END_TAG_SIZE ((size_t)8U)
#define TEST_MEMORY_DESCRIPTOR_SIZE ((size_t)40U)
#define TEST_INFO_WITH_MAP_SIZE \
    (TEST_INFO_HEADER_SIZE + TEST_EFI_MMAP_TAG_SIZE + TEST_END_TAG_SIZE)
#define TEST_EFI_MMAP_TYPE UINT32_C(17)
#define TEST_EFI_BS_TYPE UINT32_C(18)
#define TEST_END_TYPE UINT32_C(0)

static void write_descriptor(uint8_t *buffer)
{
    hcd_store_le32(buffer, 0U, 7U);
    hcd_store_le32(buffer, 4U, 0U);
    hcd_store_le64(buffer, 8U, 0x1000U);
    hcd_store_le64(buffer, 16U, 0U);
    hcd_store_le64(buffer, 24U, 1U);
    hcd_store_le64(buffer, 32U, UINT64_C(8));
}

static void initialize_info_with_map(uint8_t *info)
{
    hcd_store_le32(info, 0U, (uint32_t)TEST_INFO_WITH_MAP_SIZE);
    hcd_store_le32(info, 4U, 0U);
    hcd_store_le32(info, TEST_INFO_HEADER_SIZE, TEST_EFI_MMAP_TYPE);
    hcd_store_le32(info, TEST_INFO_HEADER_SIZE + 4U,
                   (uint32_t)TEST_EFI_MMAP_TAG_SIZE);
    hcd_store_le32(info, TEST_INFO_HEADER_SIZE + 8U,
                   (uint32_t)TEST_MEMORY_DESCRIPTOR_SIZE);
    hcd_store_le32(info, TEST_INFO_HEADER_SIZE + 12U,
                   PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1);
    write_descriptor(info + TEST_INFO_HEADER_SIZE + 16U);
    hcd_store_le32(
        info, TEST_INFO_HEADER_SIZE + TEST_EFI_MMAP_TAG_SIZE,
        TEST_END_TYPE);
    hcd_store_le32(
        info, TEST_INFO_HEADER_SIZE + TEST_EFI_MMAP_TAG_SIZE + 4U,
        (uint32_t)TEST_END_TAG_SIZE);
}

static void test_extracts_only_post_exit_efi_map(void)
{
    uint8_t info[TEST_INFO_WITH_MAP_SIZE] = {0U};
    struct preboot_uefi_exit_map map;
    uint32_t count = 0U;

    initialize_info_with_map(info);
    assert(preboot_multiboot2_get_efi_memory_map(
               info, sizeof(info), &map) == PREBOOT_MULTIBOOT2_EFI_MAP_OK);
    assert(map.buffer == (void *)(info + TEST_INFO_HEADER_SIZE + 16U));
    assert(map.size == TEST_MEMORY_DESCRIPTOR_SIZE);
    assert(map.descriptor_size == TEST_MEMORY_DESCRIPTOR_SIZE);
    assert(map.descriptor_version == PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1);
    assert(map.exit_attempts == 1U);
    assert(preboot_uefi_normalize_memory_map(
               &map, map.size, &count) == PREBOOT_UEFI_MEMORY_MAP_OK);
    assert(count == 1U);
    assert(hcd_load_le32(map.buffer, 16U) == HCD_MEMORY_USABLE);
}

static void test_rejects_active_services_and_missing_map(void)
{
    uint8_t info[TEST_INFO_WITH_MAP_SIZE + TEST_EFI_BS_TAG_SIZE] = {0U};
    struct preboot_uefi_exit_map map;
    size_t efi_bs_offset = TEST_INFO_HEADER_SIZE + TEST_EFI_MMAP_TAG_SIZE;
    size_t end_offset = efi_bs_offset + TEST_EFI_BS_TAG_SIZE;

    initialize_info_with_map(info);
    hcd_store_le32(info, 0U, (uint32_t)sizeof(info));
    hcd_store_le32(info, efi_bs_offset, TEST_EFI_BS_TYPE);
    hcd_store_le32(info, efi_bs_offset + 4U, (uint32_t)TEST_EFI_BS_TAG_SIZE);
    hcd_store_le32(info, end_offset, TEST_END_TYPE);
    hcd_store_le32(info, end_offset + 4U, (uint32_t)TEST_END_TAG_SIZE);
    assert(preboot_multiboot2_get_efi_memory_map(
               info, sizeof(info), &map) ==
           PREBOOT_MULTIBOOT2_EFI_MAP_E_BOOT_SERVICES_ACTIVE);

    hcd_store_le32(info, 0U, TEST_INFO_HEADER_SIZE + TEST_END_TAG_SIZE);
    hcd_store_le32(info, TEST_INFO_HEADER_SIZE, TEST_END_TYPE);
    hcd_store_le32(info, TEST_INFO_HEADER_SIZE + 4U, (uint32_t)TEST_END_TAG_SIZE);
    assert(preboot_multiboot2_get_efi_memory_map(
               info, sizeof(info), &map) ==
           PREBOOT_MULTIBOOT2_EFI_MAP_E_MAP_NOT_FOUND);
}

static void test_rejects_bad_info_and_map_metadata(void)
{
    uint8_t info[TEST_INFO_WITH_MAP_SIZE] = {0U};
    struct preboot_uefi_exit_map map;

    initialize_info_with_map(info);
    assert(preboot_multiboot2_get_efi_memory_map(
               info, sizeof(info) - 1U, &map) ==
           PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_INFO);
    hcd_store_le32(info, 4U, 1U);
    assert(preboot_multiboot2_get_efi_memory_map(
               info, sizeof(info), &map) ==
           PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_INFO);

    initialize_info_with_map(info);
    hcd_store_le32(info, TEST_INFO_HEADER_SIZE + 8U, 39U);
    assert(preboot_multiboot2_get_efi_memory_map(
               info, sizeof(info), &map) ==
           PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_DESCRIPTOR_SIZE);
    hcd_store_le32(info, TEST_INFO_HEADER_SIZE + 8U, 40U);
    hcd_store_le32(info, TEST_INFO_HEADER_SIZE + 12U, 2U);
    assert(preboot_multiboot2_get_efi_memory_map(
               info, sizeof(info), &map) ==
           PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_DESCRIPTOR_VERSION);
}

static void test_rejects_duplicates_and_truncated_tags(void)
{
    uint8_t info[TEST_INFO_HEADER_SIZE + (2U * TEST_EFI_MMAP_TAG_SIZE) +
                 TEST_END_TAG_SIZE] = {0U};
    struct preboot_uefi_exit_map map;
    size_t second_offset = TEST_INFO_HEADER_SIZE + TEST_EFI_MMAP_TAG_SIZE;
    size_t end_offset = second_offset + TEST_EFI_MMAP_TAG_SIZE;

    initialize_info_with_map(info);
    hcd_store_le32(info, 0U, (uint32_t)sizeof(info));
    hcd_store_le32(info, second_offset, TEST_EFI_MMAP_TYPE);
    hcd_store_le32(info, second_offset + 4U, (uint32_t)TEST_EFI_MMAP_TAG_SIZE);
    hcd_store_le32(info, second_offset + 8U, 40U);
    hcd_store_le32(info, second_offset + 12U,
                   PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1);
    hcd_store_le32(info, end_offset, TEST_END_TYPE);
    hcd_store_le32(info, end_offset + 4U, (uint32_t)TEST_END_TAG_SIZE);
    assert(preboot_multiboot2_get_efi_memory_map(
               info, sizeof(info), &map) ==
           PREBOOT_MULTIBOOT2_EFI_MAP_E_DUPLICATE_MAP);

    hcd_store_le32(info, 0U, (uint32_t)TEST_INFO_WITH_MAP_SIZE);
    hcd_store_le32(info, TEST_INFO_HEADER_SIZE + 4U, UINT32_MAX);
    assert(preboot_multiboot2_get_efi_memory_map(
               info, sizeof(info), &map) ==
           PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_TAG);
}

int main(void)
{
    test_extracts_only_post_exit_efi_map();
    test_rejects_active_services_and_missing_map();
    test_rejects_bad_info_and_map_metadata();
    test_rejects_duplicates_and_truncated_tags();
    return 0;
}
