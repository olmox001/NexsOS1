#ifndef NXHAL_DEVICE_NAME_H
#define NXHAL_DEVICE_NAME_H

#include <stddef.h>
#include <stdint.h>

void nxhal_device_name_virtio(char *dst, size_t capacity, uint32_t device_id);
void nxhal_device_name_pci(char *dst, size_t capacity,
                           uint16_t vendor_id, uint16_t device_id);

#endif /* NXHAL_DEVICE_NAME_H */
