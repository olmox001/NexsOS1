#ifndef _DRIVERS_PCI_H
#define _DRIVERS_PCI_H

#include <kernel/types.h>

/* Arch-pluggable config-space backend. amd64 uses the built-in CF8/CFC port I/O
 * (default when no ops are set); aarch64 registers ECAM MMIO ops. This is what
 * makes pci.c arch-neutral so the same PCI providers (xHCI/VGA/...) enumerate
 * on both architectures. */
struct pci_config_ops {
    uint32_t (*read32)(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
    void     (*write32)(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value);
};
void pci_set_config_ops(struct pci_config_ops *ops);

/* Platform MMIO windows used to assign unprogrammed BARs (bare-metal aarch64
 * has no firmware PCI resource assignment). Pass 32-bit and 64-bit windows
 * from the host bridge ranges. amd64 leaves these unset (firmware assigns). */
void pci_set_mmio_windows(uint64_t m32_base, uint64_t m32_size,
                          uint64_t m64_base, uint64_t m64_size);

uint32_t pci_config_read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
void pci_config_write(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value);
int pci_find_device(uint16_t vendor, uint16_t device_id);
uint32_t pci_get_bar(int bdf, int bar_index);
uint32_t pci_get_bar_size(int bdf, int bar_index);
uint8_t pci_get_interrupt(int bdf);
uint32_t pci_get_class(int bdf);       /* raw config word at offset 0x08 */
uint8_t pci_get_header_type(int bdf);  /* header-type byte (offset 0x0E) */
void pci_enumerate(void (*callback)(int bdf, uint16_t vendor, uint16_t device_id));
void pci_scan_and_register(void);

#endif
