#ifndef NXEC_PREBOOT_UEFI_MEMORY_MAP_H
#define NXEC_PREBOOT_UEFI_MEMORY_MAP_H

#include "uefi_exit.h"

#include <stddef.h>
#include <stdint.h>

enum preboot_uefi_memory_map_error {
    PREBOOT_UEFI_MEMORY_MAP_OK = 0,
    PREBOOT_UEFI_MEMORY_MAP_E_NULL_INPUT = 1,
    PREBOOT_UEFI_MEMORY_MAP_E_EBS_NOT_COMPLETE = 2,
    PREBOOT_UEFI_MEMORY_MAP_E_BAD_VERSION = 3,
    PREBOOT_UEFI_MEMORY_MAP_E_BAD_DESCRIPTOR_SIZE = 4,
    PREBOOT_UEFI_MEMORY_MAP_E_BAD_MAP_SIZE = 5,
    PREBOOT_UEFI_MEMORY_MAP_E_BAD_DESCRIPTOR = 6,
    PREBOOT_UEFI_MEMORY_MAP_E_UNSUPPORTED_TYPE = 7,
    PREBOOT_UEFI_MEMORY_MAP_E_UNSUPPORTED_ATTRIBUTES = 8,
    PREBOOT_UEFI_MEMORY_MAP_E_OVERLAP = 9,
    PREBOOT_UEFI_MEMORY_MAP_E_TOO_MANY_REGIONS = 10
};

/*
 * Converts a successful post-ExitBootServices map in place to sorted,
 * coalesced hcd_memory_record wire records. On error, record_count is zero
 * and the input buffer must not be used as normalized output.
 */
enum preboot_uefi_memory_map_error preboot_uefi_normalize_memory_map(
    const struct preboot_uefi_exit_map *map,
    size_t buffer_capacity,
    uint32_t *record_count);

#endif
