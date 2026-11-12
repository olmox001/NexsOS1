#ifndef NXEC_PREBOOT_MULTIBOOT2_EFI_MAP_H
#define NXEC_PREBOOT_MULTIBOOT2_EFI_MAP_H

#include "capability/uefi_exit.h"

#include <stddef.h>

enum preboot_multiboot2_efi_map_error {
    PREBOOT_MULTIBOOT2_EFI_MAP_OK = 0,
    PREBOOT_MULTIBOOT2_EFI_MAP_E_NULL_INPUT = 1,
    PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_INFO = 2,
    PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_TAG = 3,
    PREBOOT_MULTIBOOT2_EFI_MAP_E_DUPLICATE_MAP = 4,
    PREBOOT_MULTIBOOT2_EFI_MAP_E_MAP_NOT_FOUND = 5,
    PREBOOT_MULTIBOOT2_EFI_MAP_E_BOOT_SERVICES_ACTIVE = 6,
    PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_DESCRIPTOR_SIZE = 7,
    PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_DESCRIPTOR_VERSION = 8,
    PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_MAP_SIZE = 9
};

/*
 * Finds the post-ExitBootServices EFI memory-map tag in a complete,
 * caller-accessible Multiboot2 information block. The map points into the
 * writable EFI map tag payload for in-place normalization. EFI_BS tags are
 * rejected because they indicate Boot Services are still active.
 */
enum preboot_multiboot2_efi_map_error preboot_multiboot2_get_efi_memory_map(
    void *multiboot2_info,
    size_t info_capacity,
    struct preboot_uefi_exit_map *memory_map);

#endif
