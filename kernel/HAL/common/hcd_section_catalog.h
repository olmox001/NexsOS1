#ifndef NXEC_HCD_SECTION_CATALOG_H
#define NXEC_HCD_SECTION_CATALOG_H

#include <stdint.h>

uint32_t hcd_section_expected_entry_size(uint32_t type);
uint32_t hcd_section_expected_entry_size_for_version(
    uint32_t type,
    uint16_t version_major);
int hcd_section_is_array(uint32_t type);
int hcd_section_is_per_cpu(uint32_t type);
int hcd_section_is_external(uint32_t type);

#endif
