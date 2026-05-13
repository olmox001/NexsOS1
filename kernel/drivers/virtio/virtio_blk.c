/*
 * kernel/drivers/virtio/virtio_blk.c
 * VirtIO Block Device Driver
 */
#include <drivers/virtio.h>
#include <drivers/virtio_blk.h>
#include <kernel/arch.h>
#include <kernel/irq.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

static virtio_handle_t virtio_blk_dev = NULL;
static uint32_t virtio_blk_qsize = 0;

/* Vring structures */
static struct vring_desc *desc;
static struct vring_avail *avail;
static struct vring_used *used;

/*
 * Initialize VirtIO Block Device
 */
void virtio_blk_init(void) {
  pr_info("%s", "VirtIO: Probing for block device...\n");

  virtio_handle_t dev = NULL;
  uint32_t irq = 0;

  if (arch_virtio_get_device(VIRTIO_DEV_BLOCK, 0, &dev, &irq) == 0) {
    pr_info("VirtIO: Found Block Device (IRQ %u)\n", irq);
    virtio_blk_dev = dev;
  } else {
    pr_info("%s", "VirtIO: No block device found\n");
    return;
  }

  /* === Common initialization (Legacy) === */
  uint32_t version = virtio_read_reg(dev, VIRTIO_MMIO_VERSION);
  pr_info("VirtIO: Version %d\n", version);

  /* Reset device */
  virtio_write_reg(dev, VIRTIO_MMIO_STATUS, 0);

  /* Acknowledge + Driver */
  uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
  virtio_write_reg(dev, VIRTIO_MMIO_STATUS, status);

  /* Feature negotiation */
  uint32_t features = virtio_read_reg(dev, VIRTIO_MMIO_DEVICE_FEATURES);
  virtio_write_reg(dev, VIRTIO_MMIO_DRIVER_FEATURES, features);

  if (version >= 2) {
    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_write_reg(dev, VIRTIO_MMIO_STATUS, status);
  }

  /* Queue 0 setup */
  virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_SEL, 0);
  uint32_t qmax = virtio_read_reg(dev, VIRTIO_MMIO_QUEUE_NUM_MAX);
  uint32_t qsize = (qmax > 16) ? 16 : qmax;
  virtio_blk_qsize = qsize;
  if (virtio_blk_qsize == 0) {
    pr_err("%s", "VirtIO-Blk: Invalid queue size (0)!\n");
    return;
  }
  virtio_write_reg(dev, VIRTIO_MMIO_QUEUE_NUM, qsize);

  void *qmem = pmm_alloc_pages(2);
  if (!qmem) {
    pr_err("%s", "VirtIO: Failed to allocate queue memory\n");
    return;
  }
  memset(qmem, 0, 8192);

  desc = (struct vring_desc *)qmem;
  avail = (struct vring_avail *)((uint8_t *)qmem + qsize * 16);
  used = (struct vring_used *)((uint8_t *)qmem + 4096);

  /* Unified HAL API handles Legacy/Modern address registration */
  virtio_setup_queue(dev, 0, (uint64_t)desc, (uint64_t)avail, (uint64_t)used);

  /* Driver OK */
  status |= VIRTIO_STATUS_DRIVER_OK;
  virtio_write_reg(dev, VIRTIO_MMIO_STATUS, status);

  pr_info("%s", "VirtIO: Block Device Initialized successfully\n");
}

/*
 * virtio_blk_read: Read sectors from disk
 * Synchronous implementation (busy-wait)
 */
int virtio_blk_read(void *buf, uint64_t sector, uint32_t count) {
  if (!virtio_blk_dev || !desc || !avail || !used)
    return -1;

  static struct virtio_blk_req req;
  static uint8_t status;

  req.type = VIRTIO_BLK_T_IN;
  req.reserved = 0;
  req.sector = sector;

  /* Header (Read-only for device) */
  desc[0].addr = (uint64_t)&req;
  desc[0].len = sizeof(struct virtio_blk_req);
  desc[0].flags = VRING_DESC_F_NEXT;
  desc[0].next = 1;

  /* Buffer (Write-only for device) */
  desc[1].addr = (uint64_t)buf;
  desc[1].len = count * 512;
  desc[1].flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;
  desc[1].next = 2;

  /* Status (Write-only for device) */
  desc[2].addr = (uint64_t)&status;
  desc[2].len = 1;
  desc[2].flags = VRING_DESC_F_WRITE;
  desc[2].next = 0;

  /* Put request in Available Ring */
  uint16_t idx = avail->idx % virtio_blk_qsize;
  avail->ring[idx] = 0;

  hal_mb();
  avail->idx++;
  hal_mb();

  uint16_t old_idx = used->idx;
  volatile uint16_t *used_idx_ptr = &used->idx;

  /* Notify */
  pr_debug("VirtIO-Blk: Reading LBA %ld, notify dev 0x%lx\n", sector,
          virtio_blk_dev->hal_dev.base);
  virtio_notify(virtio_blk_dev, 0);

  /* Poll Used Ring (Busy Wait) */
  uint64_t timeout = 1000000000; /* Increased timeout */
  while (*used_idx_ptr == old_idx && timeout > 0) {
    hal_cpu_yield();
    timeout--;
  }

  /* Clear interrupt status (Legacy VirtIO requirement) */
  virtio_read_reg(virtio_blk_dev, VIRTIO_MMIO_INTERRUPT_ACK);

  if (timeout == 0) {
    uint32_t isr = virtio_read_reg(virtio_blk_dev, VIRTIO_MMIO_INTERRUPT_ACK);
    pr_err("VirtIO-Blk: Timeout waiting for device response! (used->idx=%d "
           "old_idx=%d, ISR=%02x)\n",
           *used_idx_ptr, old_idx, isr);
    return -1;
  }

  if (status != VIRTIO_BLK_S_OK) {
    pr_info("VirtIO: Read failed status=%d\n", status);
    return -1;
  }

  return 0;
}

int virtio_blk_write(void *buf, uint64_t sector, uint32_t count) {
  if (!virtio_blk_dev || !desc || !avail || !used)
    return -1;

  static struct virtio_blk_req req_w;
  static uint8_t status_w;

  req_w.type = VIRTIO_BLK_T_OUT;
  req_w.reserved = 0;
  req_w.sector = sector;

  /* Header */
  desc[0].addr = (uint64_t)&req_w;
  desc[0].len = sizeof(struct virtio_blk_req);
  desc[0].flags = VRING_DESC_F_NEXT;
  desc[0].next = 1;

  /* Buffer */
  desc[1].addr = (uint64_t)buf;
  desc[1].len = count * 512;
  desc[1].flags = VRING_DESC_F_NEXT;
  desc[1].next = 2;

  /* Status */
  desc[2].addr = (uint64_t)&status_w;
  desc[2].len = 1;
  desc[2].flags = VRING_DESC_F_WRITE;
  desc[2].next = 0;

  uint16_t idx = avail->idx % virtio_blk_qsize;
  avail->ring[idx] = 0;

  hal_mb();
  avail->idx++;
  hal_mb();

  uint16_t old_idx = used->idx;
  virtio_notify(virtio_blk_dev, 0);

  volatile uint16_t *used_idx_ptr_w = &used->idx;
  uint64_t timeout_w = 10000000;
  while (*used_idx_ptr_w == old_idx && timeout_w > 0) {
    timeout_w--;
  }
  virtio_read_reg(virtio_blk_dev, VIRTIO_MMIO_INTERRUPT_ACK);

  if (status_w != VIRTIO_BLK_S_OK) {
    pr_info("VirtIO: Write failed status=%d\n", status_w);
    return -1;
  }
  return 0;
}
