#include "uefi_exit.h"

#include <stddef.h>
#include <stdint.h>

static void clear_result(struct preboot_uefi_exit_map *result)
{
    result->buffer = (void *)0;
    result->size = 0U;
    result->map_key = 0U;
    result->descriptor_size = 0U;
    result->descriptor_version = 0U;
    result->exit_attempts = 0U;
    result->buffer_growths = 0U;
}

static int map_is_valid(
    size_t map_size,
    size_t capacity,
    size_t descriptor_size,
    uint32_t descriptor_version)
{
    if (map_size == 0U || map_size > capacity ||
        descriptor_version != PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1 ||
        descriptor_size < PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1_SIZE ||
        (map_size % descriptor_size) != 0U) {
        return 0;
    }
    return 1;
}

static int minimum_capacity(size_t required, size_t descriptor_size, size_t *minimum)
{
    size_t slack_descriptor_size = descriptor_size;
    size_t slack;

    if (slack_descriptor_size < PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1_SIZE) {
        slack_descriptor_size = PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1_SIZE;
    }
    if (slack_descriptor_size >
        (SIZE_MAX / (size_t)PREBOOT_UEFI_MAP_SLACK_DESCRIPTORS)) {
        return 0;
    }
    slack = slack_descriptor_size * (size_t)PREBOOT_UEFI_MAP_SLACK_DESCRIPTORS;
    if (required > (SIZE_MAX - slack)) {
        return 0;
    }
    *minimum = required + slack;
    return 1;
}

enum preboot_uefi_exit_error preboot_uefi_exit_boot_services(
    const struct preboot_uefi_exit_services *services,
    void *context,
    uintptr_t image_handle,
    void *initial_buffer,
    size_t initial_capacity,
    struct preboot_uefi_exit_map *result)
{
    void *buffer = initial_buffer;
    size_t capacity = initial_capacity;
    uint32_t attempt;
    uint32_t growths = 0U;

    if (result == (struct preboot_uefi_exit_map *)0) {
        return PREBOOT_UEFI_EXIT_E_NULL_INPUT;
    }
    clear_result(result);
    if (services == (const struct preboot_uefi_exit_services *)0 ||
        services->get_memory_map == 0 ||
        services->allocate_map_buffer == 0 ||
        services->exit_boot_services == 0 ||
        (initial_buffer == (void *)0 && initial_capacity != 0U)) {
        return PREBOOT_UEFI_EXIT_E_NULL_INPUT;
    }

    attempt = 0U;
    while (attempt < PREBOOT_UEFI_EXIT_MAX_ATTEMPTS) {
        size_t map_size = capacity;
        size_t descriptor_size = 0U;
        uint32_t descriptor_version = 0U;
        uint64_t map_key = 0U;
        enum preboot_uefi_status status = services->get_memory_map(
            context,
            buffer,
            &map_size,
            &map_key,
            &descriptor_size,
            &descriptor_version);

        if (status == PREBOOT_UEFI_STATUS_BUFFER_TOO_SMALL) {
            size_t required_capacity = 0U;
            size_t new_capacity = 0U;
            void *new_buffer = (void *)0;

            if (growths >= PREBOOT_UEFI_MAP_MAX_GROWTHS ||
                map_size <= capacity ||
                !minimum_capacity(map_size, descriptor_size, &required_capacity)) {
                return PREBOOT_UEFI_EXIT_E_MAP_CAPACITY;
            }
            status = services->allocate_map_buffer(
                context, required_capacity, &new_buffer, &new_capacity);
            if (status != PREBOOT_UEFI_STATUS_SUCCESS ||
                new_buffer == (void *)0 || new_capacity < required_capacity ||
                new_capacity <= capacity) {
                return PREBOOT_UEFI_EXIT_E_ALLOC_FAILED;
            }
            buffer = new_buffer;
            capacity = new_capacity;
            ++growths;
            continue;
        }
        if (status != PREBOOT_UEFI_STATUS_SUCCESS) {
            return PREBOOT_UEFI_EXIT_E_GET_MAP;
        }
        if (!map_is_valid(
                map_size, capacity, descriptor_size, descriptor_version)) {
            return PREBOOT_UEFI_EXIT_E_BAD_MAP;
        }

        status = services->exit_boot_services(context, image_handle, map_key);
        ++attempt;
        if (status == PREBOOT_UEFI_STATUS_SUCCESS) {
            result->buffer = buffer;
            result->size = map_size;
            result->map_key = map_key;
            result->descriptor_size = descriptor_size;
            result->descriptor_version = descriptor_version;
            result->exit_attempts = attempt;
            result->buffer_growths = growths;
            return PREBOOT_UEFI_EXIT_OK;
        }
        if (status != PREBOOT_UEFI_STATUS_INVALID_PARAMETER) {
            return PREBOOT_UEFI_EXIT_E_EXIT_FAILED;
        }
        if (attempt >= PREBOOT_UEFI_EXIT_MAX_ATTEMPTS) {
            return PREBOOT_UEFI_EXIT_E_RETRIES_EXHAUSTED;
        }
    }
    return PREBOOT_UEFI_EXIT_E_RETRIES_EXHAUSTED;
}
