#include "efi_exit_adapter.h"

#include "capability/uefi_exit.h"
#include "capability/uefi_memory_map.h"

#include <stddef.h>
#include <stdint.h>

struct nexs_efi_exit_context {
    EFI_BOOT_SERVICES *boot_services;
    size_t map_capacity;
};

static enum preboot_uefi_status nexs_efi_status(EFI_STATUS status) {
    if (status == EFI_SUCCESS)
        return PREBOOT_UEFI_STATUS_SUCCESS;
    if (status == EFI_BUFFER_TOO_SMALL)
        return PREBOOT_UEFI_STATUS_BUFFER_TOO_SMALL;
    if (status == EFI_INVALID_PARAMETER)
        return PREBOOT_UEFI_STATUS_INVALID_PARAMETER;
    return PREBOOT_UEFI_STATUS_FAILURE;
}

static enum preboot_uefi_status nexs_efi_get_map(
    void *opaque, void *buffer, size_t *buffer_size, uint64_t *map_key,
    size_t *descriptor_size, uint32_t *descriptor_version) {
    struct nexs_efi_exit_context *context = opaque;
    UINTN size = (UINTN)*buffer_size;
    UINTN key = 0U;
    UINTN stride = 0U;
    EFI_STATUS status;

    status = context->boot_services->GetMemoryMap(
        &size, buffer, &key, &stride, descriptor_version);
    *buffer_size = (size_t)size;
    *map_key = (uint64_t)key;
    *descriptor_size = (size_t)stride;
    return nexs_efi_status(status);
}

static enum preboot_uefi_status nexs_efi_allocate_map(
    void *opaque, size_t minimum_capacity, void **buffer, size_t *capacity) {
    struct nexs_efi_exit_context *context = opaque;
    EFI_STATUS status;

    status = context->boot_services->AllocatePool(
        EFI_LOADER_DATA, (UINTN)minimum_capacity, buffer);
    if (status != EFI_SUCCESS)
        return nexs_efi_status(status);
    context->map_capacity = minimum_capacity;
    *capacity = minimum_capacity;
    return PREBOOT_UEFI_STATUS_SUCCESS;
}

static enum preboot_uefi_status nexs_efi_exit(
    void *opaque, uintptr_t image_handle, uint64_t map_key) {
    struct nexs_efi_exit_context *context = opaque;
    EFI_STATUS status = context->boot_services->ExitBootServices(
        (EFI_HANDLE)image_handle, (UINTN)map_key);

    return nexs_efi_status(status);
}

static const struct preboot_uefi_exit_services nexs_efi_services = {
    .get_memory_map = nexs_efi_get_map,
    .allocate_map_buffer = nexs_efi_allocate_map,
    .exit_boot_services = nexs_efi_exit,
};

static void nexs_efi_clear_handoff(struct nexs_efi_memory_handoff *handoff) {
    handoff->record_base = 0U;
    handoff->record_count = 0U;
    handoff->record_size = 0U;
    handoff->exit_attempts = 0U;
    handoff->buffer_growths = 0U;
}

enum nexs_efi_exit_result nexs_efi_exit_and_normalize(
    EFI_HANDLE image_handle, EFI_BOOT_SERVICES *boot_services,
    struct nexs_efi_memory_handoff *handoff) {
    struct nexs_efi_exit_context context = {boot_services, 0U};
    struct preboot_uefi_exit_map map;
    enum preboot_uefi_exit_error exit_error;
    enum preboot_uefi_memory_map_error map_error;
    uint32_t record_count = 0U;

    if (image_handle == NULL || boot_services == NULL || handoff == NULL)
        return NEXS_EFI_EXIT_PRE_EXIT_FAILURE;
    nexs_efi_clear_handoff(handoff);
    exit_error = preboot_uefi_exit_boot_services(
        &nexs_efi_services, &context, (uintptr_t)image_handle,
        NULL, 0U, &map);
    if (exit_error != PREBOOT_UEFI_EXIT_OK)
        return NEXS_EFI_EXIT_PRE_EXIT_FAILURE;
    map_error = preboot_uefi_normalize_memory_map(
        &map, context.map_capacity, &record_count);
    if (map_error != PREBOOT_UEFI_MEMORY_MAP_OK || record_count == 0U)
        return NEXS_EFI_EXIT_POST_EXIT_FAILURE;
    handoff->record_base = (UINT64)(uintptr_t)map.buffer;
    handoff->record_count = record_count;
    handoff->record_size = NXHAL_UEFI_MEMORY_RECORD_SIZE;
    handoff->exit_attempts = map.exit_attempts;
    handoff->buffer_growths = map.buffer_growths;
    return NEXS_EFI_EXIT_OK;
}
