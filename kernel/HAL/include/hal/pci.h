#ifndef NXHAL_PCI_H
#define NXHAL_PCI_H

#include <stdint.h>

#define NXHAL_PCI_BUS_COUNT UINT32_C(256)
#define NXHAL_PCI_DEVICE_COUNT UINT32_C(32)
#define NXHAL_PCI_FUNCTION_COUNT UINT32_C(8)
#define NXHAL_PCI_BAR_COUNT UINT32_C(6)
/* Conventional PCI capability headers occupy aligned dwords in 0x40..0xfc. */
#define NXHAL_PCI_CAPABILITY_HEADER_COUNT UINT32_C(48)

#define NXHAL_PCI_VENDOR_INVALID UINT16_C(0xffff)
#define NXHAL_PCI_CONFIG_INVALID UINT32_C(0xffffffff)

struct nxhal_pci_config_ops {
  uint32_t (*read32)(uint8_t bus, uint8_t device, uint8_t function,
                     uint8_t offset);
  void (*write32)(uint8_t bus, uint8_t device, uint8_t function,
                  uint8_t offset, uint32_t value);
};

struct nxhal_pci_bar {
  uint64_t base;
  uint64_t size;
  uint32_t index;
  uint32_t is_io;
  uint32_t is_64;
  uint32_t prefetchable;
};

typedef int (*nxhal_pci_enumerate_fn)(uint32_t bdf, uint16_t vendor,
                                      uint16_t device_id);

int nxhal_pci_set_config_ops(const struct nxhal_pci_config_ops *ops);
int nxhal_pci_set_mmio_windows(uint64_t m32_base, uint64_t m32_size,
                               uint64_t m64_base, uint64_t m64_size);

uint32_t nxhal_pci_config_read(uint8_t bus, uint8_t device, uint8_t function,
                               uint8_t offset);
void nxhal_pci_config_write(uint8_t bus, uint8_t device, uint8_t function,
                            uint8_t offset, uint32_t value);

int nxhal_pci_find_device(uint16_t vendor, uint16_t device_id,
                          uint32_t *out_bdf);
int nxhal_pci_get_bar(uint32_t bdf, uint32_t bar_index,
                      struct nxhal_pci_bar *out_bar);
uint8_t nxhal_pci_get_interrupt(uint32_t bdf);
uint32_t nxhal_pci_get_class(uint32_t bdf);
uint8_t nxhal_pci_get_header_type(uint32_t bdf);
int nxhal_pci_enumerate(nxhal_pci_enumerate_fn callback);
int nxhal_pci_scan_and_register(void);

#endif /* NXHAL_PCI_H */
