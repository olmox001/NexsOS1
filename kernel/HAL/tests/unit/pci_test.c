#include <hal/device_protocol.h>
#include <hal/pci.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static uint32_t bar0_value = UINT32_C(0x80000000);
static uint32_t bar4_value = UINT32_C(0x00002004);
static uint32_t bar5_value = UINT32_C(0x00000001);
static int bar0_probe;
static int bar4_probe;
static int bar5_probe;
static unsigned write_calls;
static unsigned enumerate_calls;
static unsigned publish_calls;

static uint32_t backend_read(uint8_t bus, uint8_t dev, uint8_t func,
                             uint8_t offset) {
  if (bus != 0U || dev != 1U || func != 0U)
    return NXHAL_PCI_CONFIG_INVALID;
  if (offset == 0U)
    return UINT32_C(0xabcd1234);
  if (offset == UINT8_C(0x08))
    return UINT32_C(0x01060100);
  if (offset == UINT8_C(0x0c))
    return 0U;
  if (offset == UINT8_C(0x3c))
    return UINT32_C(11);
  if (offset == UINT8_C(0x10))
    return bar0_probe != 0 ? UINT32_C(0xfffff000) : bar0_value;
  if (offset == UINT8_C(0x20))
    return bar4_probe != 0 ? UINT32_C(0xffffe004) : bar4_value;
  if (offset == UINT8_C(0x24))
    return bar5_probe != 0 ? UINT32_MAX : bar5_value;
  return 0U;
}

static void backend_write(uint8_t bus, uint8_t dev, uint8_t func,
                          uint8_t offset, uint32_t value) {
  if (bus != 0U || dev != 1U || func != 0U)
    return;
  ++write_calls;
  if (offset == UINT8_C(0x10)) {
    bar0_probe = value == UINT32_MAX;
    if (bar0_probe == 0)
      bar0_value = value;
  } else if (offset == UINT8_C(0x20)) {
    bar4_probe = value == UINT32_MAX;
    if (bar4_probe == 0)
      bar4_value = value;
  } else if (offset == UINT8_C(0x24)) {
    bar5_probe = value == UINT32_MAX;
    if (bar5_probe == 0)
      bar5_value = value;
  }
}

int hal_ioport_available(void) { return 0; }
void hal_irq_save(uint64_t *flags) { *flags = 0U; }
void hal_irq_restore(uint64_t flags) { (void)flags; }
void hal_spin_lock(volatile uint32_t *lock) { (void)lock; }
void hal_spin_unlock(volatile uint32_t *lock) { (void)lock; }
void hal_outl(uint16_t port, uint32_t value) { (void)port; (void)value; }
uint32_t hal_inl(uint16_t port) { (void)port; return 0U; }
int hal_vmm_map_device(uint64_t base, uint64_t size) {
  return base != 0U && size != 0U ? 0 : -1;
}
int nxhal_diag_info(const char *fmt, ...) { (void)fmt; return 0; }
void nxhal_device_name_virtio(char *dst, size_t cap, uint32_t id) {
  (void)id;
  if (dst != NULL && cap != 0U)
    dst[0] = '\0';
}
void nxhal_device_name_pci(char *dst, size_t cap, uint16_t vendor,
                           uint16_t device) {
  (void)vendor;
  (void)device;
  if (dst != NULL && cap != 0U)
    dst[0] = '\0';
}
void nxhal_device_publish(const struct nxhal_device_desc *device) {
  if (device != NULL)
    ++publish_calls;
}

static int enum_callback(uint32_t bdf, uint16_t vendor, uint16_t device) {
  if (bdf == UINT32_C(0x000100) && vendor == UINT16_C(0x1234) &&
      device == UINT16_C(0xabcd))
    ++enumerate_calls;
  return 0;
}

int main(void) {
  const struct nxhal_pci_config_ops ops = {backend_read, backend_write};
  struct nxhal_pci_bar bar;
  uint32_t bdf = 0U;

  if (nxhal_pci_set_config_ops(NULL) == 0)
    return 1;
  if (nxhal_pci_set_config_ops(&ops) != 0)
    return 2;
  if (nxhal_pci_set_config_ops(&ops) == 0)
    return 3;

  if (nxhal_pci_config_read(0U, 1U, 0U, 0U) != UINT32_C(0xabcd1234))
    return 4;
  if (nxhal_pci_config_read(0U, 32U, 0U, 0U) != NXHAL_PCI_CONFIG_INVALID)
    return 5;
  nxhal_pci_config_write(0U, 1U, 0U, UINT8_C(0x04), UINT32_C(7));
  nxhal_pci_config_write(0U, 32U, 0U, 0U, UINT32_C(1));
  if (write_calls != 1U)
    return 6;

  if (nxhal_pci_get_class(UINT32_C(0x000100)) != UINT32_C(0x01060100))
    return 7;
  if (nxhal_pci_get_header_type(UINT32_C(0x000100)) != 0U)
    return 8;
  if (nxhal_pci_get_interrupt(UINT32_C(0x000100)) != UINT8_C(11))
    return 9;

  if (nxhal_pci_get_bar(UINT32_C(0x000100), 0U, &bar) != 0 ||
      bar.base != UINT64_C(0x80000000) || bar.size != UINT64_C(0x1000) ||
      bar.is_io != 0U || bar.is_64 != 0U)
    return 10;
  if (nxhal_pci_get_bar(UINT32_C(0x000100), 4U, &bar) != 0 ||
      bar.base != UINT64_C(0x0000000100002000) ||
      bar.size != UINT64_C(0x2000) || bar.is_64 == 0U)
    return 11;

  if (nxhal_pci_find_device(UINT16_C(0x1234), UINT16_C(0xabcd), &bdf) != 0 ||
      bdf != UINT32_C(0x000100))
    return 12;
  if (nxhal_pci_find_device(UINT16_C(0xbeef), UINT16_C(1), &bdf) == 0)
    return 13;

  if (nxhal_pci_enumerate(enum_callback) != 0 || enumerate_calls != 1U)
    return 14;
  if (nxhal_pci_enumerate(NULL) == 0)
    return 15;

  if (nxhal_pci_set_mmio_windows(UINT64_C(0x10000000), UINT64_C(0x100000),
                                 UINT64_C(0x100000000), UINT64_C(0x200000)) != 0)
    return 16;
  if (nxhal_pci_set_mmio_windows(UINT64_MAX, 2U, 0U, 0U) == 0)
    return 17;
  if (nxhal_pci_scan_and_register() != 0 || publish_calls != 1U)
    return 18;

  return 0;
}
