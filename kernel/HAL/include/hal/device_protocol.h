#ifndef NXHAL_DEVICE_PROTOCOL_H
#define NXHAL_DEVICE_PROTOCOL_H

#include <stdint.h>

#define NXHAL_DEVICE_PROTOCOL_ABI_VERSION UINT32_C(2)
#define NXHAL_DEVICE_NAME_CAPACITY 32U

enum nxhal_bus_type {
  NXHAL_BUS_PLATFORM = 0,
  NXHAL_BUS_PCI = 1,
  NXHAL_BUS_VIRTIO_MMIO = 2
};

struct nxhal_device_desc {
  char name[NXHAL_DEVICE_NAME_CAPACITY];
  uint32_t bus_type;
  uintptr_t base;
  uint32_t irq;
  uint16_t vendor_id;
  uint16_t device_id;
  uint8_t class_code;
  uint8_t subclass;
  uint8_t prog_if;
  uint8_t header_type;
  uint32_t pci_bdf;
};

enum nxhal_device_bind_status {
  NXHAL_DEVICE_BIND_OK = 0,
  NXHAL_DEVICE_BIND_E_NULL = -1,
  NXHAL_DEVICE_BIND_E_VERSION = -2,
  NXHAL_DEVICE_BIND_E_SIZE = -3,
  NXHAL_DEVICE_BIND_E_RESERVED = -4,
  NXHAL_DEVICE_BIND_E_MISSING = -5,
  NXHAL_DEVICE_BIND_E_STATE = -6
};

struct nxhal_device_protocol_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t reserved0;
  void (*publish)(const struct nxhal_device_desc *device);
  int (*get_count)(void);
  int (*get)(int index, struct nxhal_device_desc *device);
};

int nxhal_device_bind(const struct nxhal_device_protocol_v1 *protocol);
int nxhal_device_is_bound(void);
void nxhal_device_publish(const struct nxhal_device_desc *device);
int nxhal_device_get_count(void);
int nxhal_device_get(int index, struct nxhal_device_desc *device);

#endif /* NXHAL_DEVICE_PROTOCOL_H */
