#ifndef NXHAL_BOOT_MEMORY_RECORD_H
#define NXHAL_BOOT_MEMORY_RECORD_H

#include <hal/boot_memory.h>

#include <stdint.h>

#define NXHAL_BOOT_MEMORY_WIRE_RECORD_SIZE UINT64_C(40)

enum nxhal_boot_memory_status nxhal_boot_memory_decode_hcd_record(
    const uint8_t *record, struct nxhal_boot_memory_region *out);

#endif /* NXHAL_BOOT_MEMORY_RECORD_H */
