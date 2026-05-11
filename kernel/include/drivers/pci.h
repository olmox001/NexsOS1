#ifndef _DRIVERS_PCI_H
#define _DRIVERS_PCI_H

#include <kernel/types.h>

uint32_t pci_config_read(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset);
void pci_config_write(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value);
int pci_find_device(uint16_t vendor, uint16_t device_id);
uint32_t pci_get_bar(int bdf, int bar_index);
uint8_t pci_get_interrupt(int bdf);
void pci_enumerate(void (*callback)(int bdf, uint16_t vendor, uint16_t device_id));

#endif
