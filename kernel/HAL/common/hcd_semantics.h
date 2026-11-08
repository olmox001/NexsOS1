#ifndef NXEC_HCD_SEMANTICS_H
#define NXEC_HCD_SEMANTICS_H

#include <stdint.h>

#include "hcd_parser.h"

enum hcd_error hcd_validate_semantics(
    const uint8_t *buffer,
    const struct hcd_header *header,
    struct hcd_validation_workspace *workspace,
    enum hcd_validation_profile profile,
    struct hcd_validation_error *detail);

#endif
