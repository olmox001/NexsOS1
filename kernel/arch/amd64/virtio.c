#include <drivers/pci.h>
#include <drivers/virtio.h>
#include <kernel/arch.h>
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
  case VIRTIO_MMIO_INTERRUPT_ACK:
    return 0xFFFFFFFF; /* Handle as NOP */
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
  uint32_t mod_off = translate_modern(offset);
  if (mod_off == 0xFFFFFFFF)
    return 0;
  return *(volatile uint32_t *)(dev->base + mod_off);
}

static void modern_write32(struct virtio_device *dev, uint32_t offset,
                           uint32_t val) {
  uint32_t mod_off = translate_modern(offset);
  if (mod_off != 0xFFFFFFFF) {
    *(volatile uint32_t *)(dev->base + mod_off) = val;
  }
}

static void modern_notify(struct virtio_device *dev, uint32_t queue_idx) {
  uintptr_t notify_base = (uintptr_t)dev->priv;
  if (notify_base == 0)
    notify_base = dev->base + 0x3000; /* Fallback for QEMU default */
  /* Simplified notify for now */
  *(volatile uint16_t *)notify_base = (uint16_t)queue_idx;
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
    return inb(port);
  if (pci_off == 0x0C || pci_off == 0x0E || pci_off == 0x10)
    return inw(port);
  return inl(port);
}

static void legacy_write32(struct virtio_device *dev, uint32_t offset,
                           uint32_t val) {
  uint32_t pci_off = translate_legacy(offset);
  if (pci_off == 0xFFFFFFFF)
    return;
  uint16_t port = (uint16_t)dev->base + pci_off;
  if (pci_off == 0x12 || pci_off == 0x13)
    outb(port, (uint8_t)val);
  else if (pci_off == 0x0C || pci_off == 0x0E || pci_off == 0x10)
    outw(port, (uint16_t)val);
  else
    outl(port, val);
}

static void legacy_notify(struct virtio_device *dev, uint32_t queue_idx) {
  outw((uint16_t)dev->base + 0x10, (uint16_t)queue_idx);
}

static const struct virtio_transport_ops modern_ops = {
    modern_read32, modern_write32, modern_notify};
static const struct virtio_transport_ops legacy_ops = {
    legacy_read32, legacy_write32, legacy_notify};

/* PCI Discovery Callback */
static void virtio_pci_callback(int bdf, uint16_t vendor, uint16_t device_id) {
  if (vendor != 0x1AF4 || virtio_dev_count >= MAX_VIRTIO_DEVS)
    return;

  struct virtio_device *vdev = &virtio_devices[virtio_dev_count];
  uint32_t bar0 = pci_get_bar(bdf, 0);

  if (bar0 & 1) { /* Legacy Port I/O */
    vdev->base = bar0 & ~3;
    vdev->ops = &legacy_ops;
    /* Subsystem Device ID at 0x2C */
    uint32_t sub_id =
        pci_config_read((bdf >> 16) & 0xFF, (bdf >> 8) & 0xFF, bdf & 0x7, 0x2C);
    vdev->device_id = sub_id >> 16;
    pr_info("VirtIO: Found Legacy device at I/O 0x%lx, ID %d\n", vdev->base,
            vdev->device_id);
  } else {                                  /* Modern MMIO (Simplified) */
    uint32_t bar_val = pci_get_bar(bdf, 4); /* Try BAR4 for modern? Or BAR0? */
    if (bar_val == 0)
      bar_val = pci_get_bar(bdf, 0);

    if (bar_val == 0) {
      pr_warn("VirtIO: Skipping modern device %04x with no BAR assigned\n",
              device_id);
      return;
    }

    vdev->base = bar_val & ~0xF;
    vdev->ops = &modern_ops;
    vdev->device_id = (device_id >= 0x1040) ? (device_id - 0x1040) : device_id;
    pr_info("VirtIO: Found Modern device at MMIO 0x%lx, ID %d\n", vdev->base,
            vdev->device_id);
  }

  vdev->irq = 32 + pci_get_interrupt(bdf);

  /* Enable PCI Bus Master and IO/Mem space */
  uint32_t cmd =
      pci_config_read((bdf >> 16) & 0xFF, (bdf >> 8) & 0xFF, bdf & 0x7, 0x04);
  pci_config_write((bdf >> 16) & 0xFF, (bdf >> 8) & 0xFF, bdf & 0x7, 0x04,
                   cmd | 0x7);

  virtio_dev_count++;
}

void arch_virtio_scan(void) {
  virtio_dev_count = 0;
  pci_enumerate(virtio_pci_callback);
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
    *(volatile uint32_t *)(dev->base + 0x20) = (uint32_t)desc_addr;
    *(volatile uint32_t *)(dev->base + 0x24) = (uint32_t)(desc_addr >> 32);
    *(volatile uint32_t *)(dev->base + 0x28) = (uint32_t)avail_addr;
    *(volatile uint32_t *)(dev->base + 0x2C) = (uint32_t)(avail_addr >> 32);
    *(volatile uint32_t *)(dev->base + 0x30) = (uint32_t)used_addr;
    *(volatile uint32_t *)(dev->base + 0x34) = (uint32_t)(used_addr >> 32);
    /* Queue enable (0x1C) */
    *(volatile uint32_t *)(dev->base + 0x1C) = 1;
  } else {
    uint32_t pfn = (uint32_t)(desc_addr >> 12);
    pr_info("VirtIO: Setting Legacy PFN 0x%x for queue %u at 0x%lx\n", pfn,
            queue_idx, dev->base);
    outw((uint16_t)dev->base + 0x0E, (uint16_t)queue_idx);
    outl((uint16_t)dev->base + 0x08, pfn);
  }
}
