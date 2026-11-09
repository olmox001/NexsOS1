/*
 * AArch64 VirtIO transport consumer.
 *
 * Hardware discovery belongs to the provider bus scan.  This file consumes the
 * canonical HAL device catalog and implements the VirtIO MMIO transport only.
 */
#include <drivers/virtio.h>
#include <hal/config.h>
#include <hal/core.h>
#include <hal/device_protocol.h>
#include <hal/diagnostic_protocol.h>
#include <hal/memory_protocol.h>

#include <stddef.h>
#include <stdint.h>

static struct virtio_device *virtio_devices;
static uint32_t virtio_capacity;
static uint32_t virtio_dev_count;

static uint32_t mmio_read32(struct virtio_device *dev, uint32_t offset) {
  uint32_t value = 0U;

  if (dev == NULL ||
      hal_mmio_read32((uint64_t)dev->base + offset, &value) != 0)
    nxhal_diag_error("%s", "VirtIO-MMIO: read rejected\n");
  return value;
}

static void mmio_write32(struct virtio_device *dev, uint32_t offset,
                         uint32_t value) {
  if (dev == NULL ||
      hal_mmio_write32((uint64_t)dev->base + offset, value) != 0)
    nxhal_diag_error("%s", "VirtIO-MMIO: write rejected\n");
}

static void mmio_notify(struct virtio_device *dev, uint32_t queue_idx) {
  mmio_write32(dev, VIRTIO_MMIO_QUEUE_NOTIFY, queue_idx);
}

static const struct virtio_transport_ops mmio_ops = {
    .read32 = mmio_read32,
    .write32 = mmio_write32,
    .notify = mmio_notify,
};

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
  virtio_capacity = device_count;
  return 0;
}

static int virtio_catalog_add(const struct nxhal_device_desc *source) {
  struct virtio_device *target;

  if (source == NULL || virtio_dev_count >= virtio_capacity)
    return -1;
  if (source->bus_type != NXHAL_BUS_VIRTIO_MMIO ||
      source->vendor_id != UINT16_C(0x1af4))
    return 0;
  target = &virtio_devices[virtio_dev_count];
  *target = (struct virtio_device){0};
  target->base = source->base;
  target->irq = source->irq;
  target->device_id = source->device_id;
  target->is_legacy = mmio_read32(target, VIRTIO_MMIO_VERSION) == 1U;
  target->ops = &mmio_ops;
  ++virtio_dev_count;
  return 0;
}

void arch_virtio_scan(void) {
  int catalog_count = nxhal_device_get_count();
  int index;

  virtio_dev_count = 0U;
  if (catalog_count < 0 ||
      virtio_storage_prepare((uint32_t)catalog_count) != 0) {
    nxhal_diag_error("%s", "VirtIO-MMIO: storage allocation failed\n");
    return;
  }
  for (index = 0; index < catalog_count; ++index) {
    struct nxhal_device_desc source;
    if (nxhal_device_get(index, &source) != 0)
      continue;
    if (virtio_catalog_add(&source) != 0) {
      nxhal_diag_error("%s", "VirtIO-MMIO: catalog import failed\n");
      return;
    }
  }
}

uint32_t virtio_queue_set_size(virtio_handle_t dev, uint32_t queue_idx,
                               uint32_t requested_size) {
  uint32_t qmax;
  uint32_t qsize;

  if (dev == NULL || requested_size == 0U)
    return 0U;
  virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_SEL, queue_idx);
  qmax = virtio_read_reg(dev, VIRTIO_MMIO_QUEUE_NUM_MAX);
  if (qmax == 0U)
    return 0U;
  qsize = requested_size < qmax ? requested_size : qmax;
  virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_NUM, qsize);
  return qsize;
}

void virtio_setup_queue(virtio_handle_t dev, uint32_t queue_idx,
                        uint64_t desc_addr, uint64_t avail_addr,
                        uint64_t used_addr) {
  (void)avail_addr;
  (void)used_addr;
  if (dev == NULL || (desc_addr % (uint64_t)HAL_PAGE_SIZE) != 0U)
    return;
  virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_SEL, queue_idx);
  mmio_write32(dev, UINT32_C(0x028), (uint32_t)HAL_PAGE_SIZE);
  virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_PFN,
                   (uint32_t)(desc_addr / (uint64_t)HAL_PAGE_SIZE));
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

uint32_t virtio_config_read32(virtio_handle_t dev, uint32_t offset) {
  uint32_t value = 0U;

  if (dev == NULL ||
      hal_mmio_read32((uint64_t)dev->base + VIRTIO_MMIO_CONFIG + offset,
                      &value) != 0)
    nxhal_diag_error("%s", "VirtIO-MMIO: config read rejected\n");
  return value;
}

void virtio_config_write32(virtio_handle_t dev, uint32_t offset,
                           uint32_t value) {
  if (dev == NULL ||
      hal_mmio_write32((uint64_t)dev->base + VIRTIO_MMIO_CONFIG + offset,
                       value) != 0)
    nxhal_diag_error("%s", "VirtIO-MMIO: config write rejected\n");
}

int arch_virtio_get_device(uint32_t device_id, int index,
                           virtio_handle_t *out_dev, uint32_t *out_irq) {
  uint32_t cursor;
  int current = 0;

  if (index < 0)
    return -1;
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
