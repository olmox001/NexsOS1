#include "multiboot2_efi_map.h"

#include "common/hcd_endian.h"
#include "common/hcd_wire.h"
#include "capability/multiboot2_info.h"
#include "capability/uefi_exit.h"

#include <stddef.h>
#include <stdint.h>

#define MULTIBOOT2_TAG_TYPE_EFI_MMAP UINT32_C(17)
#define MULTIBOOT2_TAG_TYPE_EFI_BS UINT32_C(18)
#define MULTIBOOT2_EFI_MMAP_HEADER_SIZE UINT64_C(16)

static enum preboot_multiboot2_efi_map_error inspect_efi_map_tag(
    const struct preboot_multiboot2_tag *tag,
    uint8_t **descriptor_buffer,
    size_t *descriptor_bytes,
    size_t *descriptor_size,
    uint32_t *descriptor_version)
{
    uint32_t source_stride;
    uint32_t source_version;
    uint64_t payload_size;

    if ((uint64_t)tag->size < MULTIBOOT2_EFI_MMAP_HEADER_SIZE) {
        return PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_TAG;
    }
    source_stride = hcd_load_le32(tag->buffer, 8U);
    source_version = hcd_load_le32(tag->buffer, 12U);
    if ((size_t)source_stride < PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1_SIZE) {
        return PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_DESCRIPTOR_SIZE;
    }
    if (source_version != PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1) {
        return PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_DESCRIPTOR_VERSION;
    }
    payload_size = (uint64_t)tag->size - MULTIBOOT2_EFI_MMAP_HEADER_SIZE;
    if ((payload_size == 0U) || ((payload_size % source_stride) != 0U) ||
        (payload_size > (uint64_t)HCD_MAX_TOTAL_SIZE)) {
        return PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_MAP_SIZE;
    }
    *descriptor_buffer =
        (uint8_t *)(tag->buffer + MULTIBOOT2_EFI_MMAP_HEADER_SIZE);
    *descriptor_bytes = (size_t)payload_size;
    *descriptor_size = (size_t)source_stride;
    *descriptor_version = source_version;
    return PREBOOT_MULTIBOOT2_EFI_MAP_OK;
}

enum preboot_multiboot2_efi_map_error preboot_multiboot2_get_efi_memory_map(
    void *multiboot2_info,
    size_t info_capacity,
    struct preboot_uefi_exit_map *memory_map)
{
    uint8_t *info = (uint8_t *)multiboot2_info;
    uint32_t efi_map_found = 0U;
    uint32_t efi_boot_services_active = 0U;
    uint8_t *descriptor_buffer = (uint8_t *)0;
    size_t descriptor_bytes = 0U;
    size_t descriptor_size = 0U;
    uint32_t descriptor_version = 0U;
    uint32_t has_tag = 0U;
    struct preboot_multiboot2_info_iterator iterator;
    struct preboot_multiboot2_tag tag;
    enum preboot_multiboot2_efi_map_error error;
    enum preboot_multiboot2_info_error info_error;

    if ((info == (uint8_t *)0) ||
        (memory_map == (struct preboot_uefi_exit_map *)0)) {
        return PREBOOT_MULTIBOOT2_EFI_MAP_E_NULL_INPUT;
    }
    memory_map->buffer = (void *)0;
    memory_map->size = 0U;
    memory_map->map_key = 0U;
    memory_map->descriptor_size = 0U;
    memory_map->descriptor_version = 0U;
    memory_map->exit_attempts = 0U;
    memory_map->buffer_growths = 0U;
    info_error = preboot_multiboot2_info_iterator_init(
        info, info_capacity, &iterator);
    if (info_error == PREBOOT_MULTIBOOT2_INFO_E_BAD_INFO) {
        return PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_INFO;
    }
    if (info_error != PREBOOT_MULTIBOOT2_INFO_OK) {
        return PREBOOT_MULTIBOOT2_EFI_MAP_E_NULL_INPUT;
    }
    for (;;) {
        info_error = preboot_multiboot2_info_iterator_next(
            &iterator, &tag, &has_tag);
        if (info_error == PREBOOT_MULTIBOOT2_INFO_E_BAD_TAG) {
            return PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_TAG;
        }
        if (info_error != PREBOOT_MULTIBOOT2_INFO_OK) {
            return PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_INFO;
        }
        if (has_tag == 0U) {
            break;
        }
        if (tag.type == MULTIBOOT2_TAG_TYPE_EFI_MMAP) {
            if (efi_map_found != 0U) {
                return PREBOOT_MULTIBOOT2_EFI_MAP_E_DUPLICATE_MAP;
            }
            error = inspect_efi_map_tag(
                &tag, &descriptor_buffer,
                &descriptor_bytes, &descriptor_size, &descriptor_version);
            if (error != PREBOOT_MULTIBOOT2_EFI_MAP_OK) {
                return error;
            }
            efi_map_found = 1U;
        } else if (tag.type == MULTIBOOT2_TAG_TYPE_EFI_BS) {
            if (tag.size != (size_t)8U) {
                return PREBOOT_MULTIBOOT2_EFI_MAP_E_BAD_TAG;
            }
            efi_boot_services_active = 1U;
        }
    }
    if (efi_boot_services_active != 0U) {
        return PREBOOT_MULTIBOOT2_EFI_MAP_E_BOOT_SERVICES_ACTIVE;
    }
    if (efi_map_found == 0U) {
        return PREBOOT_MULTIBOOT2_EFI_MAP_E_MAP_NOT_FOUND;
    }
    memory_map->buffer = descriptor_buffer;
    memory_map->size = descriptor_bytes;
    memory_map->map_key = 0U;
    memory_map->descriptor_size = descriptor_size;
    memory_map->descriptor_version = descriptor_version;
    memory_map->exit_attempts = 1U;
    memory_map->buffer_growths = 0U;
    return PREBOOT_MULTIBOOT2_EFI_MAP_OK;
}
