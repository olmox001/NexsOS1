#ifndef NXEC_PREBOOT_MULTIBOOT2_INFO_H
#define NXEC_PREBOOT_MULTIBOOT2_INFO_H

#include <stddef.h>
#include <stdint.h>

enum preboot_multiboot2_info_error {
    PREBOOT_MULTIBOOT2_INFO_OK = 0,
    PREBOOT_MULTIBOOT2_INFO_E_NULL_INPUT = 1,
    PREBOOT_MULTIBOOT2_INFO_E_BAD_INFO = 2,
    PREBOOT_MULTIBOOT2_INFO_E_BAD_TAG = 3
};

struct preboot_multiboot2_info_iterator {
    const uint8_t *buffer;
    uint64_t total_size;
    uint64_t offset;
    uint32_t finished;
};

struct preboot_multiboot2_tag {
    const uint8_t *buffer;
    size_t size;
    uint32_t type;
};

enum preboot_multiboot2_info_error preboot_multiboot2_info_iterator_init(
    const void *buffer,
    size_t capacity,
    struct preboot_multiboot2_info_iterator *iterator);

enum preboot_multiboot2_info_error preboot_multiboot2_info_iterator_next(
    struct preboot_multiboot2_info_iterator *iterator,
    struct preboot_multiboot2_tag *tag,
    uint32_t *has_tag);

#endif
