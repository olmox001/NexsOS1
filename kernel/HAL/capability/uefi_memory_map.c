#include "uefi_memory_map.h"

#include "common/hcd_endian.h"
#include "common/hcd_wire.h"
#include "capability/uefi_exit.h"

#include <stddef.h>
#include <stdint.h>

#define EFI_MEMORY_DESCRIPTOR_BASE_SIZE ((size_t)40U)
#define EFI_PAGE_SIZE UINT64_C(4096)
#define EFI_DESCRIPTOR_PAD_OFFSET UINT64_C(4)
#define EFI_DESCRIPTOR_PHYSICAL_START_OFFSET UINT64_C(8)
#define EFI_DESCRIPTOR_PAGE_COUNT_OFFSET UINT64_C(24)
#define EFI_DESCRIPTOR_ATTRIBUTES_OFFSET UINT64_C(32)
#define HCD_MEMORY_RECORD_SIZE UINT64_C(40)

#define EFI_MEMORY_UC UINT64_C(0x0000000000000001)
#define EFI_MEMORY_WC UINT64_C(0x0000000000000002)
#define EFI_MEMORY_WT UINT64_C(0x0000000000000004)
#define EFI_MEMORY_WB UINT64_C(0x0000000000000008)
#define EFI_MEMORY_UCE UINT64_C(0x0000000000000010)
#define EFI_MEMORY_WP UINT64_C(0x0000000000001000)
#define EFI_MEMORY_RP UINT64_C(0x0000000000002000)
#define EFI_MEMORY_XP UINT64_C(0x0000000000004000)
#define EFI_MEMORY_NV UINT64_C(0x0000000000008000)
#define EFI_MEMORY_RO UINT64_C(0x0000000000020000)
#define EFI_MEMORY_RUNTIME UINT64_C(0x8000000000000000)

#define EFI_MEMORY_CACHE_MASK \
    (EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WT | EFI_MEMORY_WB | \
     EFI_MEMORY_UCE)
#define EFI_MEMORY_SUPPORTED_MASK \
    (EFI_MEMORY_CACHE_MASK | EFI_MEMORY_WP | EFI_MEMORY_RP | EFI_MEMORY_XP | \
     EFI_MEMORY_NV | EFI_MEMORY_RO | EFI_MEMORY_RUNTIME)

enum efi_memory_type {
    EFI_MEMORY_RESERVED_TYPE = 0,
    EFI_MEMORY_LOADER_CODE = 1,
    EFI_MEMORY_LOADER_DATA = 2,
    EFI_MEMORY_BOOT_SERVICES_CODE = 3,
    EFI_MEMORY_BOOT_SERVICES_DATA = 4,
    EFI_MEMORY_RUNTIME_SERVICES_CODE = 5,
    EFI_MEMORY_RUNTIME_SERVICES_DATA = 6,
    EFI_MEMORY_CONVENTIONAL = 7,
    EFI_MEMORY_UNUSABLE = 8,
    EFI_MEMORY_ACPI_RECLAIM = 9,
    EFI_MEMORY_ACPI_NVS = 10,
    EFI_MEMORY_MMIO = 11,
    EFI_MEMORY_MMIO_PORT_SPACE = 12,
    EFI_MEMORY_PAL_CODE = 13,
    EFI_MEMORY_PERSISTENT = 14,
    EFI_MEMORY_UNACCEPTED = 15
};

static int map_type(uint32_t source_type, uint32_t *target_type)
{
    switch (source_type) {
    case EFI_MEMORY_RESERVED_TYPE:
    case EFI_MEMORY_PAL_CODE:
        *target_type = (uint32_t)HCD_MEMORY_RESERVED;
        return 1;
    case EFI_MEMORY_LOADER_CODE:
    case EFI_MEMORY_LOADER_DATA:
        *target_type = (uint32_t)HCD_MEMORY_BOOTSTRAP;
        return 1;
    case EFI_MEMORY_BOOT_SERVICES_CODE:
    case EFI_MEMORY_BOOT_SERVICES_DATA:
    case EFI_MEMORY_CONVENTIONAL:
        *target_type = (uint32_t)HCD_MEMORY_USABLE;
        return 1;
    case EFI_MEMORY_RUNTIME_SERVICES_CODE:
    case EFI_MEMORY_RUNTIME_SERVICES_DATA:
        *target_type = (uint32_t)HCD_MEMORY_FIRMWARE_RUNTIME;
        return 1;
    case EFI_MEMORY_UNUSABLE:
        *target_type = (uint32_t)HCD_MEMORY_UNUSABLE;
        return 1;
    case EFI_MEMORY_ACPI_RECLAIM:
        *target_type = (uint32_t)HCD_MEMORY_ACPI_RECLAIMABLE;
        return 1;
    case EFI_MEMORY_ACPI_NVS:
        *target_type = (uint32_t)HCD_MEMORY_ACPI_NVS;
        return 1;
    case EFI_MEMORY_MMIO:
        *target_type = (uint32_t)HCD_MEMORY_MMIO;
        return 1;
    case EFI_MEMORY_PERSISTENT:
        *target_type = (uint32_t)HCD_MEMORY_PERSISTENT;
        return 1;
    case EFI_MEMORY_UNACCEPTED:
        *target_type = (uint32_t)HCD_MEMORY_UNACCEPTED;
        return 1;
    case EFI_MEMORY_MMIO_PORT_SPACE:
    default:
        return 0;
    }
}

static enum preboot_uefi_memory_map_error map_attributes(
    uint64_t source,
    uint32_t source_type,
    uint32_t target_type,
    uint32_t *target_attributes)
{
    uint64_t cache_attributes = source & EFI_MEMORY_CACHE_MASK;
    uint64_t cache_classes = cache_attributes;
    uint32_t mapped = HCD_MEMORY_A_READ | HCD_MEMORY_A_WRITE |
        HCD_MEMORY_A_EXECUTE;

    if ((source & ~EFI_MEMORY_SUPPORTED_MASK) != 0U) {
        return PREBOOT_UEFI_MEMORY_MAP_E_UNSUPPORTED_ATTRIBUTES;
    }
    if ((cache_classes & EFI_MEMORY_UCE) != 0U) {
        cache_classes &= ~EFI_MEMORY_UCE;
        cache_classes |= EFI_MEMORY_UC;
    }
    if ((cache_classes != 0U) &&
        ((cache_classes & (cache_classes - 1U)) != 0U)) {
        return PREBOOT_UEFI_MEMORY_MAP_E_BAD_DESCRIPTOR;
    }
    if ((source & EFI_MEMORY_UC) != 0U ||
        (source & EFI_MEMORY_UCE) != 0U) {
        mapped |= HCD_MEMORY_A_CACHE_UC;
    } else if ((source & EFI_MEMORY_WC) != 0U) {
        mapped |= HCD_MEMORY_A_CACHE_WC;
    } else if ((source & EFI_MEMORY_WT) != 0U) {
        mapped |= HCD_MEMORY_A_CACHE_WT;
    } else if ((source & EFI_MEMORY_WB) != 0U) {
        mapped |= HCD_MEMORY_A_CACHE_WB;
    }
    if ((source & (EFI_MEMORY_WP | EFI_MEMORY_RO)) != 0U) {
        mapped &= ~HCD_MEMORY_A_WRITE;
    }
    if ((source & EFI_MEMORY_RP) != 0U) {
        mapped &= ~HCD_MEMORY_A_READ;
    }
    if ((source & EFI_MEMORY_XP) != 0U) {
        mapped &= ~HCD_MEMORY_A_EXECUTE;
    }
    if ((source & EFI_MEMORY_NV) != 0U ||
        target_type == (uint32_t)HCD_MEMORY_PERSISTENT) {
        mapped |= HCD_MEMORY_A_PERSISTENT;
    }
    if ((source & EFI_MEMORY_RUNTIME) != 0U ||
        source_type == EFI_MEMORY_RUNTIME_SERVICES_CODE ||
        source_type == EFI_MEMORY_RUNTIME_SERVICES_DATA) {
        mapped |= HCD_MEMORY_A_RUNTIME;
    }
    *target_attributes = mapped;
    return PREBOOT_UEFI_MEMORY_MAP_OK;
}

static void write_record(
    uint8_t *buffer,
    uint64_t offset,
    uint64_t base,
    uint64_t length,
    uint32_t type,
    uint32_t attributes)
{
    hcd_store_le64(buffer, offset, base);
    hcd_store_le64(buffer, offset + 8U, length);
    hcd_store_le32(buffer, offset + 16U, type);
    hcd_store_le32(buffer, offset + 20U, attributes);
    hcd_store_le32(buffer, offset + 24U, HCD_ID_UNKNOWN);
    hcd_store_le32(buffer, offset + 28U, HCD_ID_UNKNOWN);
    hcd_store_le64(buffer, offset + 32U, 0U);
}

static void swap_records(uint8_t *buffer, uint64_t left, uint64_t right)
{
    uint64_t index;
    uint8_t temporary;

    for (index = 0U; index < HCD_MEMORY_RECORD_SIZE; ++index) {
        temporary = buffer[left + index];
        buffer[left + index] = buffer[right + index];
        buffer[right + index] = temporary;
    }
}

static void sift_down(
    uint8_t *buffer,
    uint64_t root,
    uint64_t end)
{
    uint64_t child;
    uint64_t larger;

    while (root <= (end / 2U)) {
        child = (root * 2U) + 1U;
        if (child > end) {
            break;
        }
        larger = child;
        if ((child < end) &&
            (hcd_load_le64(buffer, (child + 1U) * HCD_MEMORY_RECORD_SIZE) >
             hcd_load_le64(buffer, child * HCD_MEMORY_RECORD_SIZE))) {
            larger = child + 1U;
        }
        if (hcd_load_le64(buffer, root * HCD_MEMORY_RECORD_SIZE) >=
            hcd_load_le64(buffer, larger * HCD_MEMORY_RECORD_SIZE)) {
            break;
        }
        swap_records(
            buffer, root * HCD_MEMORY_RECORD_SIZE,
            larger * HCD_MEMORY_RECORD_SIZE);
        root = larger;
    }
}

static void sort_records(uint8_t *buffer, uint64_t count)
{
    uint64_t start = count / 2U;
    uint64_t end = count;

    while (start > 0U) {
        --start;
        sift_down(buffer, start, count - 1U);
    }
    while (end > 1U) {
        --end;
        swap_records(buffer, 0U, end * HCD_MEMORY_RECORD_SIZE);
        sift_down(buffer, 0U, end - 1U);
    }
}

static int same_extent_policy(const uint8_t *buffer, uint64_t left, uint64_t right)
{
    return (hcd_load_le32(buffer, left + 16U) ==
            hcd_load_le32(buffer, right + 16U)) &&
        (hcd_load_le32(buffer, left + 20U) ==
         hcd_load_le32(buffer, right + 20U)) &&
        (hcd_load_le32(buffer, left + 24U) ==
         hcd_load_le32(buffer, right + 24U)) &&
        (hcd_load_le32(buffer, left + 28U) ==
         hcd_load_le32(buffer, right + 28U));
}

static enum preboot_uefi_memory_map_error compact_records(
    uint8_t *buffer,
    uint64_t count,
    uint32_t *record_count)
{
    uint64_t read_index = 0U;
    uint64_t write_index = 0U;
    uint64_t source_offset;
    uint64_t destination_offset;
    uint64_t base;
    uint64_t length;
    uint64_t end;
    uint64_t previous_base;
    uint64_t previous_length;
    uint64_t previous_end;
    uint64_t previous_offset;

    for (read_index = 0U; read_index < count; ++read_index) {
        source_offset = read_index * HCD_MEMORY_RECORD_SIZE;
        base = hcd_load_le64(buffer, source_offset);
        length = hcd_load_le64(buffer, source_offset + 8U);
        end = base + length;
        if (read_index > 0U) {
            previous_offset = (write_index - 1U) * HCD_MEMORY_RECORD_SIZE;
            previous_base = hcd_load_le64(buffer, previous_offset);
            previous_length = hcd_load_le64(
                buffer, previous_offset + 8U);
            previous_end = previous_base + previous_length;
            if (base < previous_end) {
                return PREBOOT_UEFI_MEMORY_MAP_E_OVERLAP;
            }
            if ((base == previous_end) &&
                same_extent_policy(
                    buffer, previous_offset, source_offset)) {
                hcd_store_le64(
                    buffer, previous_offset + 8U,
                    end - previous_base);
                continue;
            }
        }
        destination_offset = write_index * HCD_MEMORY_RECORD_SIZE;
        if (destination_offset != source_offset) {
            swap_records(buffer, destination_offset, source_offset);
        }
        ++write_index;
        if (write_index > (uint64_t)HCD_MAX_MEMORY_REGIONS) {
            return PREBOOT_UEFI_MEMORY_MAP_E_TOO_MANY_REGIONS;
        }
    }
    *record_count = (uint32_t)write_index;
    return PREBOOT_UEFI_MEMORY_MAP_OK;
}

static enum preboot_uefi_memory_map_error validate_map_input(
    const struct preboot_uefi_exit_map *map,
    size_t buffer_capacity,
    uint64_t *descriptor_count)
{
    if ((map == (const struct preboot_uefi_exit_map *)0) ||
        (descriptor_count == (uint64_t *)0)) {
        return PREBOOT_UEFI_MEMORY_MAP_E_NULL_INPUT;
    }
    if ((map->buffer == (void *)0) || (map->size == 0U) ||
        (map->size > buffer_capacity)) {
        return PREBOOT_UEFI_MEMORY_MAP_E_BAD_MAP_SIZE;
    }
    if ((map->exit_attempts == 0U) ||
        (map->exit_attempts > PREBOOT_UEFI_EXIT_MAX_ATTEMPTS)) {
        return PREBOOT_UEFI_MEMORY_MAP_E_EBS_NOT_COMPLETE;
    }
    if (map->descriptor_version != PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1) {
        return PREBOOT_UEFI_MEMORY_MAP_E_BAD_VERSION;
    }
    if (map->descriptor_size < EFI_MEMORY_DESCRIPTOR_BASE_SIZE) {
        return PREBOOT_UEFI_MEMORY_MAP_E_BAD_DESCRIPTOR_SIZE;
    }
    if ((map->size % map->descriptor_size) != 0U ||
        (map->size > (size_t)HCD_MAX_TOTAL_SIZE)) {
        return PREBOOT_UEFI_MEMORY_MAP_E_BAD_MAP_SIZE;
    }
    *descriptor_count = (uint64_t)(map->size / map->descriptor_size);
    if (*descriptor_count == 0U) {
        return PREBOOT_UEFI_MEMORY_MAP_E_BAD_MAP_SIZE;
    }
    return PREBOOT_UEFI_MEMORY_MAP_OK;
}

static enum preboot_uefi_memory_map_error convert_descriptor(
    uint8_t *buffer,
    size_t descriptor_size,
    uint64_t index,
    uint64_t *previous_base,
    int *sorted)
{
    uint64_t descriptor_offset;
    uint64_t output_offset;
    uint64_t physical_start;
    uint64_t page_count;
    uint64_t byte_length;
    uint64_t attributes;
    uint32_t source_type;
    uint32_t target_type;
    uint32_t target_attributes;
    uint32_t pad;
    enum preboot_uefi_memory_map_error error;

    descriptor_offset = index * (uint64_t)descriptor_size;
    source_type = hcd_load_le32(buffer, descriptor_offset);
    pad = hcd_load_le32(buffer, descriptor_offset + EFI_DESCRIPTOR_PAD_OFFSET);
    physical_start = hcd_load_le64(
        buffer, descriptor_offset + EFI_DESCRIPTOR_PHYSICAL_START_OFFSET);
    page_count = hcd_load_le64(
        buffer, descriptor_offset + EFI_DESCRIPTOR_PAGE_COUNT_OFFSET);
    attributes = hcd_load_le64(
        buffer, descriptor_offset + EFI_DESCRIPTOR_ATTRIBUTES_OFFSET);
    if ((pad != 0U) || (page_count == 0U) ||
        ((physical_start & (EFI_PAGE_SIZE - 1U)) != 0U) ||
        (page_count > (UINT64_MAX / EFI_PAGE_SIZE)) ||
        !map_type(source_type, &target_type)) {
        return source_type > EFI_MEMORY_UNACCEPTED ||
                source_type == EFI_MEMORY_MMIO_PORT_SPACE ?
            PREBOOT_UEFI_MEMORY_MAP_E_UNSUPPORTED_TYPE :
            PREBOOT_UEFI_MEMORY_MAP_E_BAD_DESCRIPTOR;
    }
    byte_length = page_count * EFI_PAGE_SIZE;
    if (physical_start > (UINT64_MAX - byte_length)) {
        return PREBOOT_UEFI_MEMORY_MAP_E_BAD_DESCRIPTOR;
    }
    error = map_attributes(
        attributes, source_type, target_type, &target_attributes);
    if (error != PREBOOT_UEFI_MEMORY_MAP_OK) {
        return error;
    }
    if (((attributes & EFI_MEMORY_RUNTIME) != 0U) ||
        (source_type == EFI_MEMORY_RUNTIME_SERVICES_CODE) ||
        (source_type == EFI_MEMORY_RUNTIME_SERVICES_DATA)) {
        target_type = (uint32_t)HCD_MEMORY_FIRMWARE_RUNTIME;
    }
    output_offset = index * HCD_MEMORY_RECORD_SIZE;
    write_record(
        buffer, output_offset, physical_start, byte_length,
        target_type, target_attributes);
    if ((index > 0U) && (physical_start < *previous_base)) {
        *sorted = 0;
    }
    *previous_base = physical_start;
    return PREBOOT_UEFI_MEMORY_MAP_OK;
}

static enum preboot_uefi_memory_map_error convert_descriptors(
    uint8_t *buffer,
    size_t descriptor_size,
    uint64_t descriptor_count,
    int *sorted)
{
    uint64_t index;
    uint64_t previous_base = 0U;
    enum preboot_uefi_memory_map_error error;

    for (index = 0U; index < descriptor_count; ++index) {
        error = convert_descriptor(
            buffer, descriptor_size, index, &previous_base, sorted);
        if (error != PREBOOT_UEFI_MEMORY_MAP_OK) {
            return error;
        }
    }
    return PREBOOT_UEFI_MEMORY_MAP_OK;
}

enum preboot_uefi_memory_map_error preboot_uefi_normalize_memory_map(
    const struct preboot_uefi_exit_map *map,
    size_t buffer_capacity,
    uint32_t *record_count)
{
    uint8_t *buffer;
    uint64_t descriptor_count = 0U;
    int sorted = 1;
    enum preboot_uefi_memory_map_error error;

    if (record_count == (uint32_t *)0) {
        return PREBOOT_UEFI_MEMORY_MAP_E_NULL_INPUT;
    }
    *record_count = 0U;
    error = validate_map_input(map, buffer_capacity, &descriptor_count);
    if (error != PREBOOT_UEFI_MEMORY_MAP_OK) {
        return error;
    }
    buffer = (uint8_t *)map->buffer;
    error = convert_descriptors(
        buffer, map->descriptor_size, descriptor_count, &sorted);
    if (error != PREBOOT_UEFI_MEMORY_MAP_OK) {
        return error;
    }
    if (sorted == 0) {
        sort_records(buffer, descriptor_count);
    }
    error = compact_records(buffer, descriptor_count, record_count);
    return error;
}
