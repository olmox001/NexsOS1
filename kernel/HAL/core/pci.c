#include <hal/core.h>
#include <hal/device_name.h>
#include <hal/device_protocol.h>
#include <hal/diagnostic_protocol.h>
#include <hal/pci.h>

#include <stddef.h>
#include <stdint.h>

#define NXHAL_PCI_CONFIG_ADDRESS_PORT UINT16_C(0x0cf8)
#define NXHAL_PCI_CONFIG_DATA_PORT UINT16_C(0x0cfc)
#define NXHAL_PCI_ENABLE_BIT UINT32_C(0x80000000)
#define NXHAL_PCI_COMMAND_OFFSET UINT8_C(0x04)
#define NXHAL_PCI_CLASS_OFFSET UINT8_C(0x08)
#define NXHAL_PCI_HEADER_OFFSET UINT8_C(0x0c)
#define NXHAL_PCI_BAR0_OFFSET UINT8_C(0x10)
#define NXHAL_PCI_INTERRUPT_OFFSET UINT8_C(0x3c)

struct nxhal_pci_window {
  uint64_t next;
  uint64_t end;
};

static struct nxhal_pci_config_ops pci_config_ops;
static uint32_t pci_config_ops_bound;
static volatile uint32_t pci_config_lock;
static volatile uint32_t pci_bar_probe_lock;
static struct nxhal_pci_window pci_mmio32;
static struct nxhal_pci_window pci_mmio64;

static int pci_offset_valid(uint8_t offset) {
  return offset <= UINT8_C(0xfc) &&
         (((uint32_t)offset & UINT32_C(3)) == 0U);
}

static uint32_t pci_config_address(uint8_t bus, uint8_t device,
                                   uint8_t function, uint8_t offset) {
  return NXHAL_PCI_ENABLE_BIT | ((uint32_t)bus << 16U) |
         ((uint32_t)device << 11U) | ((uint32_t)function << 8U) |
         (uint32_t)offset;
}

static uint32_t pci_port_read(uint8_t bus, uint8_t device, uint8_t function,
                              uint8_t offset) {
  uint64_t irq_flags;
  uint32_t value;

  hal_irq_save(&irq_flags);
  hal_spin_lock(&pci_config_lock);
  hal_outl(NXHAL_PCI_CONFIG_ADDRESS_PORT,
           pci_config_address(bus, device, function, offset));
  value = hal_inl(NXHAL_PCI_CONFIG_DATA_PORT);
  hal_spin_unlock(&pci_config_lock);
  hal_irq_restore(irq_flags);
  return value;
}

static void pci_port_write(uint8_t bus, uint8_t device, uint8_t function,
                           uint8_t offset, uint32_t value) {
  uint64_t irq_flags;

  hal_irq_save(&irq_flags);
  hal_spin_lock(&pci_config_lock);
  hal_outl(NXHAL_PCI_CONFIG_ADDRESS_PORT,
           pci_config_address(bus, device, function, offset));
  hal_outl(NXHAL_PCI_CONFIG_DATA_PORT, value);
  hal_spin_unlock(&pci_config_lock);
  hal_irq_restore(irq_flags);
}

int nxhal_pci_set_config_ops(const struct nxhal_pci_config_ops *ops) {
  uint32_t expected = 0U;

  if (ops == NULL || ops->read32 == NULL || ops->write32 == NULL)
    return -1;
  if (!__atomic_compare_exchange_n(&pci_config_ops_bound, &expected, 1U, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return -1;
  pci_config_ops.read32 = ops->read32;
  pci_config_ops.write32 = ops->write32;
  __atomic_store_n(&pci_config_ops_bound, 2U, __ATOMIC_RELEASE);
  return 0;
}

uint32_t nxhal_pci_config_read(uint8_t bus, uint8_t device, uint8_t function,
                               uint8_t offset) {
  if (device >= NXHAL_PCI_DEVICE_COUNT ||
      function >= NXHAL_PCI_FUNCTION_COUNT || !pci_offset_valid(offset))
    return NXHAL_PCI_CONFIG_INVALID;
  if (__atomic_load_n(&pci_config_ops_bound, __ATOMIC_ACQUIRE) == 2U)
    return pci_config_ops.read32(bus, device, function, offset);
  if (!hal_ioport_available())
    return NXHAL_PCI_CONFIG_INVALID;
  return pci_port_read(bus, device, function, offset);
}

void nxhal_pci_config_write(uint8_t bus, uint8_t device, uint8_t function,
                            uint8_t offset, uint32_t value) {
  if (device >= NXHAL_PCI_DEVICE_COUNT ||
      function >= NXHAL_PCI_FUNCTION_COUNT || !pci_offset_valid(offset))
    return;
  if (__atomic_load_n(&pci_config_ops_bound, __ATOMIC_ACQUIRE) == 2U) {
    pci_config_ops.write32(bus, device, function, offset, value);
    return;
  }
  if (hal_ioport_available())
    pci_port_write(bus, device, function, offset, value);
}

static int pci_bdf_decode(uint32_t bdf, uint8_t *bus, uint8_t *device,
                          uint8_t *function) {
  uint32_t raw_device = (bdf >> 8U) & UINT32_C(0xff);
  uint32_t raw_function = bdf & UINT32_C(0xff);

  if (bus == NULL || device == NULL || function == NULL ||
      raw_device >= NXHAL_PCI_DEVICE_COUNT ||
      raw_function >= NXHAL_PCI_FUNCTION_COUNT)
    return -1;
  *bus = (uint8_t)(bdf >> 16U);
  *device = (uint8_t)raw_device;
  *function = (uint8_t)raw_function;
  return 0;
}

static uint32_t pci_bdf_encode(uint32_t bus, uint32_t device,
                               uint32_t function) {
  return (bus << 16U) | (device << 8U) | function;
}

static int pci_function_present(uint32_t bus, uint32_t device,
                                uint32_t function, uint16_t *vendor,
                                uint16_t *device_id) {
  uint32_t reg0 = nxhal_pci_config_read((uint8_t)bus, (uint8_t)device,
                                        (uint8_t)function, 0U);
  uint16_t found_vendor = (uint16_t)reg0;

  if (found_vendor == NXHAL_PCI_VENDOR_INVALID)
    return 0;
  if (vendor != NULL)
    *vendor = found_vendor;
  if (device_id != NULL)
    *device_id = (uint16_t)(reg0 >> 16U);
  return 1;
}

static int pci_enumerate_device(uint32_t bus, uint32_t device,
                                nxhal_pci_enumerate_fn callback) {
  uint16_t vendor;
  uint16_t device_id;
  uint32_t header;
  uint32_t function;

  if (!pci_function_present(bus, device, 0U, &vendor, &device_id))
    return 0;
  if (callback(pci_bdf_encode(bus, device, 0U), vendor, device_id) != 0)
    return -1;
  header = nxhal_pci_config_read((uint8_t)bus, (uint8_t)device, 0U,
                                 NXHAL_PCI_HEADER_OFFSET);
  if ((header & UINT32_C(0x00800000)) == 0U)
    return 0;
  for (function = 1U; function < NXHAL_PCI_FUNCTION_COUNT; ++function) {
    if (!pci_function_present(bus, device, function, &vendor, &device_id))
      continue;
    if (callback(pci_bdf_encode(bus, device, function), vendor, device_id) != 0)
      return -1;
  }
  return 0;
}

int nxhal_pci_enumerate(nxhal_pci_enumerate_fn callback) {
  uint32_t bus;
  uint32_t device;

  if (callback == NULL)
    return -1;
  for (bus = 0U; bus < NXHAL_PCI_BUS_COUNT; ++bus) {
    for (device = 0U; device < NXHAL_PCI_DEVICE_COUNT; ++device) {
      if (pci_enumerate_device(bus, device, callback) != 0)
        return -1;
    }
  }
  return 0;
}

int nxhal_pci_find_device(uint16_t vendor, uint16_t device_id,
                          uint32_t *out_bdf) {
  uint32_t bus;
  uint32_t device;
  uint32_t function;

  if (out_bdf == NULL || vendor == NXHAL_PCI_VENDOR_INVALID)
    return -1;
  for (bus = 0U; bus < NXHAL_PCI_BUS_COUNT; ++bus) {
    for (device = 0U; device < NXHAL_PCI_DEVICE_COUNT; ++device) {
      for (function = 0U; function < NXHAL_PCI_FUNCTION_COUNT; ++function) {
        uint16_t found_vendor;
        uint16_t found_device;
        if (!pci_function_present(bus, device, function, &found_vendor,
                                  &found_device))
          continue;
        if (found_vendor == vendor && found_device == device_id) {
          *out_bdf = pci_bdf_encode(bus, device, function);
          return 0;
        }
      }
    }
  }
  return -1;
}

uint32_t nxhal_pci_get_class(uint32_t bdf) {
  uint8_t bus;
  uint8_t device;
  uint8_t function;

  if (pci_bdf_decode(bdf, &bus, &device, &function) != 0)
    return NXHAL_PCI_CONFIG_INVALID;
  return nxhal_pci_config_read(bus, device, function, NXHAL_PCI_CLASS_OFFSET);
}

uint8_t nxhal_pci_get_header_type(uint32_t bdf) {
  uint8_t bus;
  uint8_t device;
  uint8_t function;
  uint32_t value;

  if (pci_bdf_decode(bdf, &bus, &device, &function) != 0)
    return UINT8_C(0xff);
  value = nxhal_pci_config_read(bus, device, function, NXHAL_PCI_HEADER_OFFSET);
  return (uint8_t)(value >> 16U);
}

uint8_t nxhal_pci_get_interrupt(uint32_t bdf) {
  uint8_t bus;
  uint8_t device;
  uint8_t function;
  uint32_t value;

  if (pci_bdf_decode(bdf, &bus, &device, &function) != 0)
    return UINT8_C(0xff);
  value = nxhal_pci_config_read(bus, device, function,
                                NXHAL_PCI_INTERRUPT_OFFSET);
  return (uint8_t)value;
}

static uint8_t pci_bar_offset(uint32_t index) {
  return (uint8_t)(NXHAL_PCI_BAR0_OFFSET + (index * UINT32_C(4)));
}

static uint64_t pci_bar_size32(uint32_t mask, uint32_t clear_mask) {
  uint32_t address_mask = mask & clear_mask;

  if (address_mask == 0U)
    return 0U;
  return (uint64_t)(~address_mask) + UINT64_C(1);
}

static uint64_t pci_bar_size64(uint32_t low_mask, uint32_t high_mask) {
  uint64_t mask = ((uint64_t)high_mask << 32U) |
                  (uint64_t)(low_mask & UINT32_C(0xfffffff0));

  if (mask == 0U)
    return 0U;
  return (~mask) + UINT64_C(1);
}

static uint64_t pci_probe_bar_size(uint8_t bus, uint8_t device,
                                   uint8_t function, uint32_t index,
                                   uint32_t original, int is_io, int is_64) {
  uint8_t offset = pci_bar_offset(index);
  uint32_t low_mask;
  uint32_t high_original = 0U;
  uint32_t high_mask = 0U;
  uint64_t irq_flags;

  hal_irq_save(&irq_flags);
  hal_spin_lock(&pci_bar_probe_lock);
  if (is_64 != 0)
    high_original = nxhal_pci_config_read(bus, device, function,
                                          (uint8_t)(offset + 4U));
  nxhal_pci_config_write(bus, device, function, offset, UINT32_MAX);
  if (is_64 != 0)
    nxhal_pci_config_write(bus, device, function, (uint8_t)(offset + 4U),
                           UINT32_MAX);
  low_mask = nxhal_pci_config_read(bus, device, function, offset);
  if (is_64 != 0)
    high_mask = nxhal_pci_config_read(bus, device, function,
                                      (uint8_t)(offset + 4U));
  nxhal_pci_config_write(bus, device, function, offset, original);
  if (is_64 != 0)
    nxhal_pci_config_write(bus, device, function, (uint8_t)(offset + 4U),
                           high_original);
  hal_spin_unlock(&pci_bar_probe_lock);
  hal_irq_restore(irq_flags);
  if (is_64 != 0)
    return pci_bar_size64(low_mask, high_mask);
  return pci_bar_size32(low_mask, is_io != 0 ? UINT32_C(0xfffffffc)
                                              : UINT32_C(0xfffffff0));
}

int nxhal_pci_get_bar(uint32_t bdf, uint32_t bar_index,
                      struct nxhal_pci_bar *out_bar) {
  uint8_t bus;
  uint8_t device;
  uint8_t function;
  uint32_t low;
  uint32_t high = 0U;

  if (out_bar == NULL || bar_index >= NXHAL_PCI_BAR_COUNT ||
      pci_bdf_decode(bdf, &bus, &device, &function) != 0)
    return -1;
  low = nxhal_pci_config_read(bus, device, function, pci_bar_offset(bar_index));
  out_bar->index = bar_index;
  out_bar->is_io = low & UINT32_C(1);
  out_bar->is_64 = out_bar->is_io == 0U &&
                   (low & UINT32_C(6)) == UINT32_C(4) &&
                   bar_index + 1U < NXHAL_PCI_BAR_COUNT;
  out_bar->prefetchable = out_bar->is_io == 0U &&
                          (low & UINT32_C(8)) != 0U;
  if (out_bar->is_64 != 0U)
    high = nxhal_pci_config_read(bus, device, function,
                                 pci_bar_offset(bar_index + 1U));
  out_bar->base = out_bar->is_io != 0U
      ? (uint64_t)(low & UINT32_C(0xfffffffc))
      : ((uint64_t)high << 32U) | (low & UINT32_C(0xfffffff0));
  out_bar->size = pci_probe_bar_size(bus, device, function, bar_index, low,
                                     out_bar->is_io != 0U,
                                     out_bar->is_64 != 0U);
  return 0;
}

static int pci_window_set(struct nxhal_pci_window *window, uint64_t base,
                          uint64_t size) {
  if (window == NULL)
    return -1;
  if (size == 0U) {
    if (base != 0U)
      return -1;
    window->next = 0U;
    window->end = 0U;
    return 0;
  }
  if (base > UINT64_MAX - size)
    return -1;
  window->next = base;
  window->end = base + size;
  return 0;
}

int nxhal_pci_set_mmio_windows(uint64_t m32_base, uint64_t m32_size,
                               uint64_t m64_base, uint64_t m64_size) {
  struct nxhal_pci_window window32;
  struct nxhal_pci_window window64;

  if (pci_window_set(&window32, m32_base, m32_size) != 0 ||
      pci_window_set(&window64, m64_base, m64_size) != 0)
    return -1;
  if (window32.end > UINT64_C(0x100000000))
    return -1;
  pci_mmio32 = window32;
  pci_mmio64 = window64;
  return 0;
}

static int pci_power_of_two(uint64_t value) {
  return value != 0U && (value & (value - UINT64_C(1))) == 0U;
}

static uint64_t pci_window_allocate(struct nxhal_pci_window *window,
                                    uint64_t size) {
  uint64_t aligned;

  if (window == NULL || window->next == 0U || !pci_power_of_two(size) ||
      window->next > UINT64_MAX - (size - UINT64_C(1)))
    return 0U;
  aligned = (window->next + size - UINT64_C(1)) & ~(size - UINT64_C(1));
  if (aligned > window->end || size > window->end - aligned)
    return 0U;
  window->next = aligned + size;
  return aligned;
}

static uint64_t pci_allocate_bar(const struct nxhal_pci_bar *bar) {
  uint64_t address;

  if (bar == NULL || bar->is_io != 0U || bar->size == 0U)
    return 0U;
  if (bar->is_64 != 0U) {
    address = pci_window_allocate(&pci_mmio64, bar->size);
    if (address != 0U)
      return address;
  }
  return pci_window_allocate(&pci_mmio32, bar->size);
}

static int pci_program_bar(uint32_t bdf, const struct nxhal_pci_bar *bar,
                           uint64_t address) {
  uint8_t bus;
  uint8_t device;
  uint8_t function;
  uint8_t offset;
  uint32_t low_flags;

  if (bar == NULL || address == 0U ||
      pci_bdf_decode(bdf, &bus, &device, &function) != 0)
    return -1;
  offset = pci_bar_offset(bar->index);
  low_flags = nxhal_pci_config_read(bus, device, function, offset) &
              UINT32_C(0x0000000f);
  nxhal_pci_config_write(bus, device, function, offset,
                         (uint32_t)address | low_flags);
  if (bar->is_64 != 0U)
    nxhal_pci_config_write(bus, device, function, (uint8_t)(offset + 4U),
                           (uint32_t)(address >> 32U));
  return 0;
}

static int pci_prepare_primary_bar(uint32_t bdf, struct nxhal_pci_bar *bar) {
  uint64_t address;

  if (nxhal_pci_get_bar(bdf, 0U, bar) != 0)
    return -1;
  if (bar->is_io != 0U || bar->base != 0U || bar->size == 0U)
    return 0;
  address = pci_allocate_bar(bar);
  if (address == 0U || pci_program_bar(bdf, bar, address) != 0)
    return -1;
  bar->base = address;
  return 0;
}

static void pci_enable_function(uint32_t bdf) {
  uint8_t bus;
  uint8_t device;
  uint8_t function;
  uint32_t command;

  if (pci_bdf_decode(bdf, &bus, &device, &function) != 0)
    return;
  command = nxhal_pci_config_read(bus, device, function,
                                  NXHAL_PCI_COMMAND_OFFSET);
  nxhal_pci_config_write(bus, device, function, NXHAL_PCI_COMMAND_OFFSET,
                         command | UINT32_C(7));
}

static void pci_fill_device(struct nxhal_device_desc *device, uint32_t bdf,
                            uint16_t vendor, uint16_t device_id,
                            const struct nxhal_pci_bar *bar) {
  uint32_t class_word = nxhal_pci_get_class(bdf);

  device->bus_type = NXHAL_BUS_PCI;
  device->base = bar != NULL ? (uintptr_t)bar->base : 0U;
  device->irq = 0U;
  device->vendor_id = vendor;
  device->device_id = device_id;
  device->class_code = (uint8_t)(class_word >> 24U);
  device->subclass = (uint8_t)(class_word >> 16U);
  device->prog_if = (uint8_t)(class_word >> 8U);
  device->header_type = nxhal_pci_get_header_type(bdf);
  device->pci_bdf = bdf;
}

static int pci_register_one(uint32_t bdf, uint16_t vendor, uint16_t device_id) {
  struct nxhal_pci_bar bar;
  struct nxhal_device_desc device = {0};
  int bar_status = pci_prepare_primary_bar(bdf, &bar);

  if (bar_status != 0)
    return -1;
  pci_enable_function(bdf);
  pci_fill_device(&device, bdf, vendor, device_id, &bar);
  if (vendor == UINT16_C(0x1af4))
    nxhal_device_name_virtio(device.name, sizeof(device.name), device_id);
  else
    nxhal_device_name_pci(device.name, sizeof(device.name), vendor, device_id);
  if (bar.is_io == 0U && bar.base != 0U && bar.size != 0U &&
      hal_vmm_map_device(bar.base, bar.size) != 0)
    return -1;
  nxhal_device_publish(&device);
  return 0;
}

int nxhal_pci_scan_and_register(void) {
  nxhal_diag_info("%s", "PCI: scanning config space in NX-HAL...\n");
  return nxhal_pci_enumerate(pci_register_one);
}
