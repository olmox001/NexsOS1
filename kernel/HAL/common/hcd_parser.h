#ifndef NXEC_HCD_PARSER_H
#define NXEC_HCD_PARSER_H

#include <stdint.h>

#include <stddef.h>

#include "hcd_wire.h"

enum hcd_validation_profile {
    HCD_PROFILE_CORE = 1,
    HCD_PROFILE_RECOVERY = 2
};

struct hcd_section_view {
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

struct hcd_validation_workspace {
    uint32_t section_count;
    uint32_t seen_mask;
    struct hcd_section_view sections[HCD_MAX_SECTIONS];
    uint64_t cpu_seen[HCD_MAX_CPUS / 64U];
};

struct hcd_validation_error {
    uint32_t code;
    uint32_t section_type;
    uint32_t record_index;
    uint32_t reserved0;
    uint64_t offset;
};

struct hcd_validated_view {
    const uint8_t *buffer;
    uint64_t size;
    struct hcd_header header;
    const struct hcd_validation_workspace *workspace;
};

_Static_assert(sizeof(struct hcd_section_view) == 40U,
               "HCD section view size changed");
_Static_assert(sizeof(struct hcd_validation_workspace) <= ((size_t)12U * (size_t)1024U),
               "HCD validation workspace exceeds its fixed budget");

enum hcd_error hcd_validate(
    const uint8_t *buffer,
    uint64_t available_size,
    enum hcd_validation_profile profile,
    struct hcd_validation_workspace *workspace,
    struct hcd_validation_error *detail,
    struct hcd_validated_view *view);

enum hcd_error hcd_find_section(
    const struct hcd_validated_view *view,
    uint32_t section_type,
    struct hcd_section_view *section);

#endif
