#ifndef NXEC_HCD_BUILDER_H
#define NXEC_HCD_BUILDER_H

#include <stdint.h>

#include "hcd_parser.h"

#define HCD_BUILDER_DIRECTORY_BYTES \
    ((uint64_t)HCD_MAX_SECTIONS * (uint64_t)HCD_SECTION_ENTRY_SIZE)
#define HCD_BUILDER_PAYLOAD_OFFSET \
    ((uint64_t)HCD_HEADER_SIZE + HCD_BUILDER_DIRECTORY_BYTES)

struct hcd_builder_section {
    uint32_t type;
    uint16_t version;
    uint16_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t entry_size;
    uint32_t entry_count;
    uint32_t alignment;
    uint32_t reserved0;
};

struct hcd_builder {
    uint8_t *buffer;
    uint64_t capacity;
    uint64_t cursor;
    uint32_t architecture;
    uint16_t version_major;
    uint16_t reserved0;
    uint32_t flags;
    uint32_t producer_id;
    uint32_t producer_version;
    uint32_t section_count;
    uint32_t seen_mask;
    uint32_t finalized;
    struct hcd_builder_section sections[HCD_MAX_SECTIONS];
};

_Static_assert(sizeof(struct hcd_builder_section) == HCD_SECTION_ENTRY_SIZE,
               "HCD builder directory entry size");

enum hcd_error hcd_builder_init(
    struct hcd_builder *builder,
    uint8_t *buffer,
    uint64_t capacity,
    uint32_t architecture,
    uint32_t flags,
    uint32_t producer_id,
    uint32_t producer_version,
    struct hcd_validation_error *detail);

enum hcd_error hcd_builder_init_v2(
    struct hcd_builder *builder,
    uint8_t *buffer,
    uint64_t capacity,
    uint32_t architecture,
    uint32_t flags,
    uint32_t producer_id,
    uint32_t producer_version,
    struct hcd_validation_error *detail);

enum hcd_error hcd_builder_reserve_section(
    struct hcd_builder *builder,
    uint32_t type,
    uint16_t version,
    uint16_t flags,
    uint32_t entry_size,
    uint32_t entry_count,
    uint32_t alignment,
    uint8_t **section_data,
    uint64_t *section_size,
    struct hcd_validation_error *detail);

enum hcd_error hcd_builder_finalize(
    struct hcd_builder *builder,
    uint64_t *total_size,
    struct hcd_validation_error *detail);

#endif
