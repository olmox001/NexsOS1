#ifndef NXEC_PREBOOT_UEFI_EXIT_H
#define NXEC_PREBOOT_UEFI_EXIT_H

#include <stddef.h>
#include <stdint.h>

#define PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1 UINT32_C(1)
#define PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1_SIZE ((size_t)40U)
#define PREBOOT_UEFI_MAP_SLACK_DESCRIPTORS UINT32_C(8)
#define PREBOOT_UEFI_EXIT_MAX_ATTEMPTS UINT32_C(3)
#define PREBOOT_UEFI_MAP_MAX_GROWTHS UINT32_C(4)

enum preboot_uefi_status {
    PREBOOT_UEFI_STATUS_SUCCESS = 0,
    PREBOOT_UEFI_STATUS_BUFFER_TOO_SMALL = 1,
    PREBOOT_UEFI_STATUS_INVALID_PARAMETER = 2,
    PREBOOT_UEFI_STATUS_FAILURE = 3
};

enum preboot_uefi_exit_error {
    PREBOOT_UEFI_EXIT_OK = 0,
    PREBOOT_UEFI_EXIT_E_NULL_INPUT = 1,
    PREBOOT_UEFI_EXIT_E_GET_MAP = 2,
    PREBOOT_UEFI_EXIT_E_MAP_CAPACITY = 3,
    PREBOOT_UEFI_EXIT_E_BAD_MAP = 4,
    PREBOOT_UEFI_EXIT_E_EXIT_FAILED = 5,
    PREBOOT_UEFI_EXIT_E_RETRIES_EXHAUSTED = 6,
    PREBOOT_UEFI_EXIT_E_ALLOC_FAILED = 7
};

struct preboot_uefi_exit_services {
    enum preboot_uefi_status (*get_memory_map)(
        void *context,
        void *buffer,
        size_t *buffer_size,
        uint64_t *map_key,
        size_t *descriptor_size,
        uint32_t *descriptor_version);
    enum preboot_uefi_status (*allocate_map_buffer)(
        void *context,
        size_t minimum_capacity,
        void **buffer,
        size_t *capacity);
    enum preboot_uefi_status (*exit_boot_services)(
        void *context,
        uintptr_t image_handle,
        uint64_t map_key);
};

struct preboot_uefi_exit_map {
    void *buffer;
    size_t size;
    uint64_t map_key;
    size_t descriptor_size;
    uint32_t descriptor_version;
    uint32_t exit_attempts;
    uint32_t buffer_growths;
};

/*
 * The callbacks must use only GetMemoryMap, memory allocation services, and
 * ExitBootServices. After a failed exit attempt, no other Boot Service may be
 * called. Buffers allocated while retrying remain allocated through handoff.
 */
enum preboot_uefi_exit_error preboot_uefi_exit_boot_services(
    const struct preboot_uefi_exit_services *services,
    void *context,
    uintptr_t image_handle,
    void *initial_buffer,
    size_t initial_capacity,
    struct preboot_uefi_exit_map *result);

#endif
