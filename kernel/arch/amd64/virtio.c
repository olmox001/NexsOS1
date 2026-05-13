#include <drivers/pci.h>
#include <drivers/virtio.h>
#include <kernel/hal.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

#define MAX_VIRTIO_DEVS 8

static struct virtio_device virtio_devices[MAX_VIRTIO_DEVS];
static int virtio_dev_count = 0;

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

static uint32_t translate_modern(uint32_t offset) {
  switch (offset) {
  case VIRTIO_MMIO_DEVICE_FEATURES:
    return 0x04;
  case VIRTIO_MMIO_DRIVER_FEATURES:
    return 0x0C;
  case VIRTIO_MMIO_QUEUE_SEL:
    return 0x16;
  case VIRTIO_MMIO_QUEUE_NUM_MAX:
    return 0x18;
  case VIRTIO_MMIO_QUEUE_NUM:
    return 0x18;
  case VIRTIO_MMIO_STATUS:
    return 0x14;
  case VIRTIO_MMIO_QUEUE_PFN:
    return 0x20; /* queue_desc_lo */
  case VIRTIO_MMIO_INTERRUPT_STATUS:
    return 0x60; /* Standard MMIO offset, might need adjustment for PCI */
  case VIRTIO_MMIO_INTERRUPT_ACK:
    return 0x64;
  default:
    return 0xFFFFFFFF;
  }
}

/* Modern (MMIO) Implementation */
static uint32_t modern_read32(struct virtio_device *dev, uint32_t offset) {
  if (offset == VIRTIO_MMIO_MAGIC_VALUE)
    return 0x74726976;
  if (offset == VIRTIO_MMIO_VERSION)
    return 2;
  
  if (offset == VIRTIO_MMIO_INTERRUPT_STATUS || offset == VIRTIO_MMIO_INTERRUPT_ACK) {
    if (dev->isr_base) {
      return hal_read8(dev->isr_base);
    }
    return 0;
  }

  uint32_t mod_off = translate_modern(offset);
  if (mod_off == 0xFFFFFFFF)
    return 0;
  return hal_read32(dev->base + mod_off);
}

static void modern_write32(struct virtio_device *dev, uint32_t offset,
                           uint32_t val) {
  if (offset == VIRTIO_MMIO_INTERRUPT_ACK) {
    if (dev->isr_base) {
      hal_read8(dev->isr_base); /* Reading ISR status acknowledges on PCI */
    }
    return;
  }

  uint32_t mod_off = translate_modern(offset);
  if (mod_off != 0xFFFFFFFF) {
    hal_write32(dev->base + mod_off, val);
  }
}

static void modern_notify(struct virtio_device *dev, uint32_t queue_idx) {
  if (dev->notify_base) {
    hal_write16(dev->notify_base, (uint16_t)queue_idx);
  } else {
    /* Fallback if no notify capability found */
    hal_write16(dev->base + 0x3000, (uint16_t)queue_idx);
  }
}

/* Legacy (Port I/O) Implementation */
static uint32_t legacy_read32(struct virtio_device *dev, uint32_t offset) {
  if (offset == VIRTIO_MMIO_MAGIC_VALUE)
    return 0x74726976;
  if (offset == VIRTIO_MMIO_VERSION)
    return 1;
  uint32_t pci_off = translate_legacy(offset);
  if (pci_off == 0xFFFFFFFF)
    return 0;
  uint16_t port = (uint16_t)dev->base + pci_off;
  if (pci_off == 0x12 || pci_off == 0x13)
    return hal_read8(port);
  if (pci_off == 0x0C || pci_off == 0x0E || pci_off == 0x10)
    return hal_read16(port);
  return hal_read32(port);
}

static void legacy_write32(struct virtio_device *dev, uint32_t offset,
                           uint32_t val) {
  uint32_t pci_off = translate_legacy(offset);
  if (pci_off == 0xFFFFFFFF)
    return;
  uint16_t port = (uint16_t)dev->base + pci_off;
  if (pci_off == 0x12 || pci_off == 0x13)
    hal_write8(port, (uint8_t)val);
  else if (pci_off == 0x0C || pci_off == 0x0E || pci_off == 0x10)
    hal_write16(port, (uint16_t)val);
  else
    hal_write32(port, val);
}

static void legacy_notify(struct virtio_device *dev, uint32_t queue_idx) {
  hal_write16((uint16_t)dev->base + 0x10, (uint16_t)queue_idx);
}

static const struct virtio_transport_ops modern_ops = {
    modern_read32, modern_write32, modern_notify};
static const struct virtio_transport_ops legacy_ops = {
    legacy_read32, legacy_write32, legacy_notify};

/* PCI Discovery Callback */
static void virtio_pci_init_device(struct hal_device *hdev) {
  if (virtio_dev_count >= MAX_VIRTIO_DEVS)
    return;

  struct virtio_device *vdev = &virtio_devices[virtio_dev_count];
  memset(vdev, 0, sizeof(struct virtio_device));

  /* Store HAL handle and basic info */
  vdev->hal_dev.base = hdev->base;
  vdev->hal_dev.irq = hdev->irq;
  vdev->hal_dev.bus_type = hdev->bus_type;
  vdev->hal_dev.io_type = (hdev->base < 0x10000) ? HAL_RES_PORT : HAL_RES_MMIO;

  vdev->base = hdev->base;
  vdev->irq = hdev->irq;
  vdev->device_id = hdev->device_id;
  vdev->is_legacy = (hdev->vendor_id == 0x1AF4 && hdev->device_id < 0x1000);

  int bdf = hdev->pci_bdf;
  uint8_t bus = (bdf >> 16) & 0xFF;
  uint8_t dev_idx = (bdf >> 8) & 0xFF;
  uint8_t func = bdf & 0x7;

  /* Scans Capabilities for Modern Device */
  bool is_modern = (hdev->vendor_id == 0x1AF4 && hdev->device_id >= 0x1); // Simplified check since IDs are already translated

  if (is_modern && vdev->hal_dev.io_type == HAL_RES_MMIO) {
    uint8_t cap_ptr = pci_config_read(bus, dev_idx, func, 0x34) & 0xFF;
    while (cap_ptr != 0 && cap_ptr != 0xFF) {
      uint32_t cap_header = pci_config_read(bus, dev_idx, func, cap_ptr);
      uint8_t cap_id = cap_header & 0xFF;
      uint8_t next_cap = (cap_header >> 8) & 0xFF;

      if (cap_id == 0x09) { /* Vendor Specific (VirtIO) */
        uint8_t type = (cap_header >> 24) & 0xFF;
        uint8_t bar = pci_config_read(bus, dev_idx, func, cap_ptr + 4) & 0xFF;
        uint32_t offset = pci_config_read(bus, dev_idx, func, cap_ptr + 8);

        uintptr_t bar_addr = pci_get_bar(bdf, bar) & ~0xF;

        if (type == 1) { /* Common Config */
          vdev->base = bar_addr + offset;
          vdev->hal_dev.base = vdev->base;
        } else if (type == 2) { /* Notifications */
          vdev->notify_base = bar_addr + offset;
        } else if (type == 3) { /* ISR Status */
          vdev->isr_base = bar_addr + offset;
        } else if (type == 4) { /* Device Specific */
          vdev->priv = (void *)(bar_addr + offset);
        }
      }
      cap_ptr = next_cap;
    }
    vdev->ops = &modern_ops;
    pr_info("VirtIO: Found Modern device (PCI) at Common=0x%lx, ISR=0x%lx, ID %d\n",
            vdev->base, vdev->isr_base, vdev->device_id);
  } else {
    vdev->ops = &legacy_ops;
    vdev->is_legacy = true;
    pr_info("VirtIO: Found Legacy device (PCI) at 0x%lx, ID %d\n", vdev->base,
            vdev->device_id);
  }

  virtio_dev_count++;
}

void arch_virtio_scan(void) {
  virtio_dev_count = 0;
  int count = hal_device_get_count();
  for (int i = 0; i < count; i++) {
    struct hal_device *hdev = hal_device_get(i);
    if (hdev && hdev->vendor_id == 0x1AF4) {
      virtio_pci_init_device(hdev);
    }
  }
}

int arch_virtio_get_count(uint32_t device_id) {
  int count = 0;
  for (int i = 0; i < virtio_dev_count; i++) {
    if (virtio_devices[i].device_id == device_id)
      count++;
  }
  return count;
}

int arch_virtio_get_device(uint32_t device_id, int index,
                           virtio_handle_t *out_dev, uint32_t *out_irq) {
  int current = 0;
  for (int i = 0; i < virtio_dev_count; i++) {
    if (virtio_devices[i].device_id == device_id) {
      if (current == index) {
        if (out_dev)
          *out_dev = &virtio_devices[i];
        if (out_irq)
          *out_irq = virtio_devices[i].irq;
        return 0;
      }
      current++;
    }
  }
  return -1;
}

void virtio_setup_queue(virtio_handle_t dev, uint32_t queue_idx,
                        uint64_t desc_addr, uint64_t avail_addr,
                        uint64_t used_addr) {
  if (dev->ops == &modern_ops) {
    modern_write32(dev, VIRTIO_MMIO_QUEUE_SEL, queue_idx);
    /* Modern registers: desc(0x20), avail(0x28), used(0x30) */
    hal_write32(dev->base + 0x20, (uint32_t)desc_addr);
    hal_write32(dev->base + 0x24, (uint32_t)(desc_addr >> 32));
    hal_write32(dev->base + 0x28, (uint32_t)avail_addr);
    hal_write32(dev->base + 0x2C, (uint32_t)(avail_addr >> 32));
    hal_write32(dev->base + 0x30, (uint32_t)used_addr);
    hal_write32(dev->base + 0x34, (uint32_t)(used_addr >> 32));
    /* Queue enable (0x1C) */
    hal_write32(dev->base + 0x1C, 1);
  } else {
    uint32_t pfn = (uint32_t)(desc_addr >> 12);
    pr_info("VirtIO: Setting Legacy PFN 0x%x for queue %u at 0x%lx\n", pfn,
            queue_idx, dev->base);
    hal_write16((uint16_t)dev->base + 0x0E, (uint16_t)queue_idx);
    hal_write32((uint16_t)dev->base + 0x08, pfn);
  }
}
