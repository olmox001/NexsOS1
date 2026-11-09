#include <hal/diagnostic_protocol.h>
#include <hal/config.h>
#include <hal/device_protocol.h>
#include <hal/memory_protocol.h>
#include <hal/pci.h>
#include <hal/core.h>
#include <drivers/virtio.h>

static struct virtio_device *virtio_devices;
static uint32_t virtio_capacity;
static uint32_t virtio_dev_count;

/* Translation helpers (reuse logic from original) */
static uint32_t translate_legacy(uint32_t offset) {
  switch (offset) {
  case VIRTIO_MMIO_DEVICE_FEATURES:
    return 0x00;
  case VIRTIO_MMIO_DRIVER_FEATURES:
    return 0x04;
  case VIRTIO_MMIO_QUEUE_PFN:
    return 0x08;
  case VIRTIO_MMIO_QUEUE_NUM_MAX:
    return 0x0C;
  case VIRTIO_MMIO_QUEUE_NUM:
    return 0x0C;
  case VIRTIO_MMIO_QUEUE_SEL:
    return 0x0E;
  case VIRTIO_MMIO_QUEUE_NOTIFY:
    return 0x10;
  case VIRTIO_MMIO_STATUS:
    return 0x12;
  case VIRTIO_MMIO_INTERRUPT_STATUS:
  case VIRTIO_MMIO_INTERRUPT_ACK:
    return 0x13;
  default:
    return 0xFFFFFFFF;
  }
}

static uint32_t virtio_mmio_read32(uint64_t address) {
  uint32_t value = 0U;

  if (hal_mmio_read32(address, &value) != 0)
    nxhal_diag_error("VirtIO: invalid MMIO read at 0x%lx\n", address);
  return value;
}

static uint32_t virtio_mmio_read16(uint64_t address) {
  uint16_t value = 0U;

  if (hal_mmio_read16(address, &value) != 0)
    nxhal_diag_error("VirtIO: invalid MMIO16 read at 0x%lx\n", address);
  return value;
}

static uint32_t virtio_mmio_read8(uint64_t address) {
  uint8_t value = 0U;

  if (hal_mmio_read8(address, &value) != 0)
    nxhal_diag_error("VirtIO: invalid MMIO8 read at 0x%lx\n", address);
  return value;
}

static void virtio_mmio_write32(uint64_t address, uint32_t value) {
  if (hal_mmio_write32(address, value) != 0)
    nxhal_diag_error("VirtIO: invalid MMIO write at 0x%lx\n", address);
}

static void virtio_mmio_write16(uint64_t address, uint16_t value) {
  if (hal_mmio_write16(address, value) != 0)
    nxhal_diag_error("VirtIO: invalid MMIO16 write at 0x%lx\n", address);
}

static void virtio_mmio_write8(uint64_t address, uint8_t value) {
  if (hal_mmio_write8(address, value) != 0)
    nxhal_diag_error("VirtIO: invalid MMIO8 write at 0x%lx\n", address);
}

static uint32_t modern_interrupt_read(struct virtio_device *dev) {
  if (dev == NULL || dev->isr_base == 0U)
    return 0U;
  return virtio_mmio_read8((uint64_t)dev->isr_base);
}

static uint32_t modern_read32(struct virtio_device *dev, uint32_t offset) {
  uint64_t base;

  if (dev == NULL)
    return 0U;
  if (offset == VIRTIO_MMIO_MAGIC_VALUE)
    return UINT32_C(0x74726976);
  if (offset == VIRTIO_MMIO_VERSION)
    return UINT32_C(2);
  if (offset == VIRTIO_MMIO_INTERRUPT_STATUS ||
      offset == VIRTIO_MMIO_INTERRUPT_ACK)
    return modern_interrupt_read(dev);
  base = (uint64_t)dev->base;
  if (offset == VIRTIO_MMIO_DEVICE_FEATURES)
    return virtio_mmio_read32(base + UINT64_C(0x04));
  if (offset == VIRTIO_MMIO_DRIVER_FEATURES)
    return virtio_mmio_read32(base + UINT64_C(0x0c));
  if (offset == VIRTIO_MMIO_QUEUE_SEL)
    return virtio_mmio_read16(base + UINT64_C(0x16));
  if (offset == VIRTIO_MMIO_QUEUE_NUM_MAX || offset == VIRTIO_MMIO_QUEUE_NUM)
    return virtio_mmio_read16(base + UINT64_C(0x18));
  if (offset == VIRTIO_MMIO_STATUS)
    return virtio_mmio_read8(base + UINT64_C(0x14));
  return 0U;
}

static void modern_write32(struct virtio_device *dev, uint32_t offset,
                           uint32_t value) {
  uint64_t base;

  if (dev == NULL)
    return;
  if (offset == VIRTIO_MMIO_INTERRUPT_ACK) {
    (void)modern_interrupt_read(dev);
    return;
  }
  base = (uint64_t)dev->base;
  if (offset == VIRTIO_MMIO_DRIVER_FEATURES)
    virtio_mmio_write32(base + UINT64_C(0x0c), value);
  else if (offset == VIRTIO_MMIO_QUEUE_SEL)
    virtio_mmio_write16(base + UINT64_C(0x16), (uint16_t)value);
  else if (offset == VIRTIO_MMIO_QUEUE_NUM)
    virtio_mmio_write16(base + UINT64_C(0x18), (uint16_t)value);
  else if (offset == VIRTIO_MMIO_STATUS)
    virtio_mmio_write8(base + UINT64_C(0x14), (uint8_t)value);
}

static int modern_notify_address(struct virtio_device *dev, uint32_t queue_idx,
                                 uint64_t *out_address) {
  uint16_t notify_offset = 0U;
  uint64_t delta;

  if (dev == NULL || out_address == NULL || dev->notify_base == 0U)
    return -1;
  modern_write32(dev, VIRTIO_MMIO_QUEUE_SEL, queue_idx);
  if (hal_mmio_read16((uint64_t)dev->base + UINT64_C(0x1e),
                      &notify_offset) != 0)
    return -1;
  if (__builtin_mul_overflow((uint64_t)notify_offset,
                             (uint64_t)dev->notify_off_multiplier, &delta) ||
      (uint64_t)dev->notify_base > UINT64_MAX - delta)
    return -1;
  *out_address = (uint64_t)dev->notify_base + delta;
  return 0;
}

static void modern_notify(struct virtio_device *dev, uint32_t queue_idx) {
  uint64_t address;

  if (modern_notify_address(dev, queue_idx, &address) != 0 ||
      hal_mmio_write16(address, (uint16_t)queue_idx) != 0)
    nxhal_diag_error("%s", "VirtIO: notify write failed\n");
}

static uint32_t legacy_read32(struct virtio_device *dev, uint32_t offset) {
  uint32_t pci_offset = translate_legacy(offset);
  uint16_t port;

  if (offset == VIRTIO_MMIO_MAGIC_VALUE)
    return UINT32_C(0x74726976);
  if (offset == VIRTIO_MMIO_VERSION)
    return UINT32_C(1);
  if (pci_offset == UINT32_MAX || dev->base > UINT16_MAX ||
      pci_offset > (uint32_t)UINT16_MAX - (uint32_t)(uint16_t)dev->base)
    return 0U;
  port = (uint16_t)dev->base + (uint16_t)pci_offset;
  if (pci_offset == UINT32_C(0x12) || pci_offset == UINT32_C(0x13))
    return hal_inb(port);
  if (pci_offset == UINT32_C(0x0c) || pci_offset == UINT32_C(0x0e) ||
      pci_offset == UINT32_C(0x10))
    return hal_inw(port);
  return hal_inl(port);
}

static void legacy_write32(struct virtio_device *dev, uint32_t offset,
                           uint32_t value) {
  uint32_t pci_offset = translate_legacy(offset);
  uint16_t port;

  if (pci_offset == UINT32_MAX || dev->base > UINT16_MAX ||
      pci_offset > (uint32_t)UINT16_MAX - (uint32_t)(uint16_t)dev->base)
    return;
  port = (uint16_t)dev->base + (uint16_t)pci_offset;
  if (pci_offset == UINT32_C(0x12) || pci_offset == UINT32_C(0x13))
    hal_outb(port, (uint8_t)value);
  else if (pci_offset == UINT32_C(0x0c) || pci_offset == UINT32_C(0x0e) ||
           pci_offset == UINT32_C(0x10))
    hal_outw(port, (uint16_t)value);
  else
    hal_outl(port, value);
}

static void legacy_notify(struct virtio_device *dev, uint32_t queue_idx) {
  if (dev->base <= UINT16_MAX - UINT16_C(0x10))
    hal_outw((uint16_t)dev->base + UINT16_C(0x10), (uint16_t)queue_idx);
}

static const struct virtio_transport_ops modern_ops = {
    modern_read32, modern_write32, modern_notify};
static const struct virtio_transport_ops legacy_ops = {
    legacy_read32, legacy_write32, legacy_notify};

static int virtio_storage_prepare(uint32_t device_count) {
  size_t bytes;
  size_t pages;

  if (device_count == 0U) {
    virtio_devices = NULL;
    virtio_capacity = 0U;
    return 0;
  }
  if (__builtin_mul_overflow((size_t)device_count,
                             sizeof(struct virtio_device), &bytes))
    return -1;
  if (bytes > SIZE_MAX - ((size_t)HAL_PAGE_SIZE - 1U))
    return -1;
  pages = (bytes + (size_t)HAL_PAGE_SIZE - 1U) / (size_t)HAL_PAGE_SIZE;
  virtio_devices = (struct virtio_device *)nxhal_memory_alloc_pages(pages);
  if (virtio_devices == NULL)
    return -1;
  {
    uint32_t index;
    for (index = 0U; index < device_count; ++index)
      virtio_devices[index] = (struct virtio_device){0};
  }
  virtio_capacity = device_count;
  return 0;
}

static int virtio_bar_for_device(const struct nxhal_device_desc *hdev,
                                 struct nxhal_pci_bar *bar, int *modern) {
  struct nxhal_pci_bar bar0;
  struct nxhal_pci_bar bar4;

  if (hdev == NULL || bar == NULL || modern == NULL)
    return -1;
  if (nxhal_pci_get_bar(hdev->pci_bdf, 0U, &bar0) != 0 ||
      nxhal_pci_get_bar(hdev->pci_bdf, 4U, &bar4) != 0)
    return -1;
  if (bar4.is_io == 0U && bar4.base != 0U &&
      (uintptr_t)bar4.base == hdev->base) {
    *bar = bar4;
    *modern = 1;
    return 0;
  }
  *bar = bar0;
  *modern = 0;
  return 0;
}

static int virtio_cap_error(uint32_t bdf, uint8_t cap_ptr,
                            const char *reason) {
  nxhal_diag_error("VirtIO: BDF 0x%x cap 0x%x rejected: %s\n",
                   bdf, (unsigned)cap_ptr, reason);
  return -1;
}

static int virtio_cap_region(uint32_t bdf, uint8_t cap_ptr,
                             uint32_t cap_header, struct nxhal_pci_bar *bar,
                             uint32_t *out_offset, uint32_t *out_length) {
  uint8_t bus = (uint8_t)(bdf >> 16U);
  uint8_t device = (uint8_t)(bdf >> 8U);
  uint8_t function = (uint8_t)bdf;
  uint8_t type = (uint8_t)(cap_header >> 24U);
  uint8_t cap_length = (uint8_t)(cap_header >> 16U);
  uint8_t bar_index;

  if (bar == NULL || out_offset == NULL || out_length == NULL)
    return virtio_cap_error(bdf, cap_ptr, "null output");
  if (cap_length < UINT8_C(16) ||
      (type == UINT8_C(2) && cap_length < UINT8_C(20)) ||
      (uint32_t)cap_ptr + UINT32_C(12) > UINT32_C(0xfc))
    return virtio_cap_error(bdf, cap_ptr, "invalid capability length");
  bar_index = (uint8_t)nxhal_pci_config_read(
      bus, device, function, (uint8_t)(cap_ptr + 4U));
  *out_offset = nxhal_pci_config_read(
      bus, device, function, (uint8_t)(cap_ptr + 8U));
  *out_length = nxhal_pci_config_read(
      bus, device, function, (uint8_t)(cap_ptr + 12U));
  if (nxhal_pci_get_bar(bdf, bar_index, bar) != 0)
    return virtio_cap_error(bdf, cap_ptr, "BAR lookup failed");
  if (*out_length == 0U || bar->is_io != 0U ||
      *out_offset > bar->size ||
      (uint64_t)*out_length > bar->size - *out_offset ||
      bar->base > UINT64_MAX - (uint64_t)*out_offset) {
    nxhal_diag_error(
        "VirtIO: BDF 0x%x cap 0x%x type=%u BAR%u base=0x%lx size=0x%lx "
        "offset=0x%x length=0x%x invalid\n",
        bdf, (unsigned)cap_ptr, (unsigned)type, (unsigned)bar_index,
        bar->base, bar->size, *out_offset, *out_length);
    return -1;
  }
  if (hal_vmm_map_device(bar->base, bar->size) != 0)
    return virtio_cap_error(bdf, cap_ptr, "BAR mapping failed");
  return 0;
}

static int virtio_apply_pci_cap(struct virtio_device *vdev, uint32_t bdf,
                                uint8_t cap_ptr, uint32_t cap_header) {
  struct nxhal_pci_bar bar;
  uint8_t bus = (uint8_t)(bdf >> 16U);
  uint8_t device = (uint8_t)(bdf >> 8U);
  uint8_t function = (uint8_t)bdf;
  uint8_t type = (uint8_t)(cap_header >> 24U);
  uint32_t offset;
  uint32_t region_length;
  uint64_t address;

  if (type < UINT8_C(1) || type > UINT8_C(4))
    return 0;
  if (virtio_cap_region(bdf, cap_ptr, cap_header, &bar, &offset,
                        &region_length) != 0)
    return -1;
  address = bar.base + offset;
  if (type == UINT8_C(1))
    vdev->base = (uintptr_t)address;
  else if (type == UINT8_C(2)) {
    if ((uint32_t)cap_ptr + UINT32_C(16) > UINT32_C(0xfc))
      return virtio_cap_error(bdf, cap_ptr, "notify multiplier out of range");
    vdev->notify_base = (uintptr_t)address;
    vdev->notify_off_multiplier = nxhal_pci_config_read(
        bus, device, function, (uint8_t)(cap_ptr + 16U));
  } else if (type == UINT8_C(3))
    vdev->isr_base = (uintptr_t)address;
  else
    vdev->priv = (void *)(uintptr_t)address;
  return 0;
}

static int virtio_scan_pci_caps(struct virtio_device *vdev, uint32_t bdf) {
  uint8_t bus = (uint8_t)(bdf >> 16U);
  uint8_t device = (uint8_t)(bdf >> 8U);
  uint8_t function = (uint8_t)bdf;
  uint8_t cap_ptr = (uint8_t)nxhal_pci_config_read(
      bus, device, function, UINT8_C(0x34));
  uint32_t step;

  for (step = 0U;
       step < NXHAL_PCI_CAPABILITY_HEADER_COUNT &&
       cap_ptr != 0U && cap_ptr != UINT8_C(0xff);
       ++step) {
    uint32_t header;
    uint8_t next;
    if (cap_ptr < UINT8_C(0x40) || cap_ptr > UINT8_C(0xf4) ||
        (cap_ptr & UINT8_C(3)) != 0U)
      return -1;
    header = nxhal_pci_config_read(bus, device, function, cap_ptr);
    next = (uint8_t)(header >> 8U);
    if ((uint8_t)header == UINT8_C(0x09) &&
        virtio_apply_pci_cap(vdev, bdf, cap_ptr, header) != 0)
      return -1;
    if (next == cap_ptr)
      return -1;
    cap_ptr = next;
  }
  return cap_ptr == 0U || cap_ptr == UINT8_C(0xff) ? 0 : -1;
}

static int virtio_pci_init_device(const struct nxhal_device_desc *hdev) {
  struct nxhal_pci_bar transport_bar;
  struct virtio_device *vdev;
  int modern;

  if (hdev == NULL || virtio_dev_count >= virtio_capacity)
    return -1;
  if (virtio_bar_for_device(hdev, &transport_bar, &modern) != 0)
    return -1;
  vdev = &virtio_devices[virtio_dev_count];
  *vdev = (struct virtio_device){0};
  vdev->base = hdev->base;
  vdev->irq = hdev->irq;
  vdev->device_id = hdev->device_id;
  vdev->is_legacy = modern == 0;
  if (modern != 0) {
    if (virtio_scan_pci_caps(vdev, hdev->pci_bdf) != 0)
      return -1;
    if (vdev->base == 0U || vdev->notify_base == 0U ||
        vdev->isr_base == 0U) {
      nxhal_diag_error(
          "VirtIO: BDF 0x%x incomplete caps common=0x%lx notify=0x%lx "
          "isr=0x%lx device=0x%lx multiplier=%u\n",
          hdev->pci_bdf, (uint64_t)vdev->base,
          (uint64_t)vdev->notify_base, (uint64_t)vdev->isr_base,
          (uint64_t)(uintptr_t)vdev->priv, vdev->notify_off_multiplier);
      return -1;
    }
    vdev->ops = &modern_ops;
  } else {
    if (transport_bar.is_io == 0U)
      return -1;
    vdev->ops = &legacy_ops;
  }
  ++virtio_dev_count;
  return 0;
}

void arch_virtio_scan(void) {
  int catalog_count = nxhal_device_get_count();
  int index;

  virtio_dev_count = 0U;
  if (catalog_count < 0 ||
      virtio_storage_prepare((uint32_t)catalog_count) != 0) {
    nxhal_diag_error("%s", "VirtIO: storage allocation failed\n");
    return;
  }
  for (index = 0; index < catalog_count; ++index) {
    struct nxhal_device_desc hdev;
    if (nxhal_device_get(index, &hdev) != 0)
      continue;
    if (hdev.vendor_id != UINT16_C(0x1af4))
      continue;
    if (virtio_pci_init_device(&hdev) != 0) {
      nxhal_diag_error("VirtIO: PCI init failed for BDF 0x%x\n",
                       hdev.pci_bdf);
      return;
    }
  }
}

/* Device-config access (GFX-DYN-01 F7).  On a modern PCI device the device-
 * specific config region was captured into dev->priv (cap type 4) during probe;
 * read it directly.  Otherwise (MMIO-on-amd64 fallback) config is at
 * base+VIRTIO_MMIO_CONFIG. */
uint32_t virtio_config_read32(virtio_handle_t dev, uint32_t offset) {
  uint64_t base;

  if (dev == NULL)
    return 0U;
  base = dev->priv != NULL ? (uint64_t)(uintptr_t)dev->priv
                           : (uint64_t)dev->base + VIRTIO_MMIO_CONFIG;
  return virtio_mmio_read32(base + offset);
}

void virtio_config_write32(virtio_handle_t dev, uint32_t offset, uint32_t val) {
  uint64_t base;

  if (dev == NULL)
    return;
  base = dev->priv != NULL ? (uint64_t)(uintptr_t)dev->priv
                           : (uint64_t)dev->base + VIRTIO_MMIO_CONFIG;
  virtio_mmio_write32(base + offset, val);
}

int arch_virtio_get_count(uint32_t device_id) {
  uint32_t index;
  int count = 0;

  for (index = 0U; index < virtio_dev_count; ++index) {
    if (virtio_devices[index].device_id == device_id)
      ++count;
  }
  return count;
}

int arch_virtio_get_device(uint32_t device_id, int index,
                           virtio_handle_t *out_dev, uint32_t *out_irq) {
  uint32_t cursor;
  int current = 0;

  for (cursor = 0U; cursor < virtio_dev_count; ++cursor) {
    if (virtio_devices[cursor].device_id != device_id)
      continue;
    if (current == index) {
      if (out_dev != NULL)
        *out_dev = &virtio_devices[cursor];
      if (out_irq != NULL)
        *out_irq = virtio_devices[cursor].irq;
      return 0;
    }
    ++current;
  }
  return -1;
}

uint32_t virtio_queue_set_size(virtio_handle_t dev, uint32_t queue_idx,
                               uint32_t requested_size) {
  if (!dev || requested_size == 0)
    return 0;

  virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_SEL, queue_idx);
  const uint32_t qmax =
      virtio_read_reg(dev, VIRTIO_MMIO_QUEUE_NUM_MAX);
  if (qmax == 0)
    return 0;

  if (dev->is_legacy) {
    /* Legacy PCI exposes QueueSize at 0x0C as READ-ONLY. There is no
     * writable QueueNum register in this transport. The device always derives
     * the avail-ring offset from its full QueueSize, so the caller must lay
     * out the ring using exactly qmax; writing a smaller value to 0x0C is
     * ignored and silently corrupts the queue layout. */
    nxhal_diag_info("VirtIO: Legacy PCI queue %u fixed size=%u (requested=%u)\n",
            queue_idx, qmax, requested_size);
    return qmax;
  }

  const uint32_t qsize =
      requested_size < qmax ? requested_size : qmax;
  virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_NUM, qsize);
  return qsize;
}

void virtio_setup_queue(virtio_handle_t dev, uint32_t queue_idx,
                        uint64_t desc_addr, uint64_t avail_addr,
                        uint64_t used_addr) {
  if (dev->ops == &modern_ops) {
    modern_write32(dev, VIRTIO_MMIO_QUEUE_SEL, queue_idx);
    /* Modern registers: desc(0x20), avail(0x28), used(0x30) */
    virtio_mmio_write32((uint64_t)dev->base + UINT64_C(0x20),
                        (uint32_t)desc_addr);
    virtio_mmio_write32((uint64_t)dev->base + UINT64_C(0x24),
                        (uint32_t)(desc_addr >> 32U));
    virtio_mmio_write32((uint64_t)dev->base + UINT64_C(0x28),
                        (uint32_t)avail_addr);
    virtio_mmio_write32((uint64_t)dev->base + UINT64_C(0x2c),
                        (uint32_t)(avail_addr >> 32U));
    virtio_mmio_write32((uint64_t)dev->base + UINT64_C(0x30),
                        (uint32_t)used_addr);
    virtio_mmio_write32((uint64_t)dev->base + UINT64_C(0x34),
                        (uint32_t)(used_addr >> 32U));
    virtio_mmio_write16((uint64_t)dev->base + UINT64_C(0x1c), UINT16_C(1));
  } else {
    uint32_t pfn = (uint32_t)(desc_addr >> 12);
    nxhal_diag_info("VirtIO: Setting Legacy PFN 0x%x for queue %u at 0x%lx\n", pfn,
            queue_idx, dev->base);
    if (dev->base > UINT16_MAX - UINT16_C(0x0e))
      return;
    hal_outw((uint16_t)dev->base + UINT16_C(0x0e), (uint16_t)queue_idx);
    hal_outl((uint16_t)dev->base + UINT16_C(0x08), pfn);
  }
}
