/*
 * kernel/drivers/virtio/virtio_blk.c
 * VirtIO Block Device Driver
 */
#include <drivers/virtio.h>
#include <drivers/virtio_blk.h>
#include <kernel/arch.h>
#include <kernel/block.h>
#include <kernel/irq.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

static virtio_handle_t virtio_blk_dev = NULL;
static uint32_t virtio_blk_qsize = 0;

/* Vring structures */
static struct vring_desc *desc;
static struct vring_avail *avail;
static struct vring_used *used;

/* virtio_blk_lock serialises the whole request lifecycle (descriptor setup,
 * publish, poll, status read).  The driver keeps ONE set of ring descriptors
 * (desc[0..2]) and one request header: before this lock existed, two CPUs
 * issuing I/O concurrently (doom loading WAD lumps while another process hit
 * the FS) clobbered each other's descriptors mid-flight — the device then
 * read a torn request header and returned VIRTIO_BLK_S_IOERR ("VirtIO: Read
 * failed status=1" / "W_ReadLump: only read 0 of N" in the boot traces).
 * irqsave also keeps a timer-tick preemption from interleaving a second
 * request from the SAME CPU into a half-built ring. */
static DEFINE_SPINLOCK(virtio_blk_lock);

/* DMA targets for the request header and status byte (DRV-VIRTIO-03): these
 * used to live on the caller's STACK while the device DMA-wrote into them.
 * Static copies are safe now that virtio_blk_lock guarantees one request in
 * flight at a time. */
static struct virtio_blk_req blk_req;
static volatile uint8_t blk_status;

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

  /* This driver currently implements only the base block request
   * protocol. Do not claim optional legacy-PCI features (EVENT_IDX,
   * INDIRECT_DESC, MQ, etc.) that this single-queue implementation does not
   * consume. Modern negotiation is retained as before pending 64-bit feature
   * selector support in the transport layer. */
  uint32_t offered_features =
      virtio_read_reg(dev, VIRTIO_MMIO_DEVICE_FEATURES);
  uint32_t features = dev->is_legacy ? 0U : offered_features;
  virtio_write_reg(dev, VIRTIO_MMIO_DRIVER_FEATURES, features);
  if (dev->is_legacy) {
    pr_info("VirtIO: Legacy PCI block features offered=%08x accepted=%08x\n",
            offered_features, features);
  }

  if (version >= 2) {
    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_write_reg(dev, VIRTIO_MMIO_STATUS, status);
  }

  /* Queue 0 size is transport-specific: legacy PCI has a fixed, read-only
   * QueueSize, while MMIO/modern PCI accept a requested size. The transport
   * returns the actual size so the ring layout always matches the device. */
  uint32_t qsize = virtio_queue_set_size(dev, 0, 16);
  virtio_blk_qsize = qsize;
  if (virtio_blk_qsize == 0) {
    pr_err("%s", "VirtIO-Blk: Invalid queue size (0)!\n");
    return;
  }

  /* Legacy PCI fixes QueueSize in hardware, so the Used Ring offset is
   * ALIGN(desc_bytes + avail_bytes, 4096), not always 4096. Include the
   * optional EVENT_IDX trailer space as well, even when not negotiated. */
  const size_t queue_page_size = 4096U;
  const size_t desc_bytes = (size_t)qsize * sizeof(struct vring_desc);
  const size_t avail_bytes =
      sizeof(struct vring_avail) +
      (size_t)qsize * sizeof(uint16_t) + sizeof(uint16_t);
  const size_t used_offset =
      (desc_bytes + avail_bytes + queue_page_size - 1U) &
      ~(queue_page_size - 1U);
  const size_t used_bytes =
      sizeof(struct vring_used) +
      (size_t)qsize * sizeof(struct vring_used_elem) + sizeof(uint16_t);
  const size_t total_bytes = used_offset + used_bytes;
  const uint32_t qpages =
      (uint32_t)((total_bytes + queue_page_size - 1U) / queue_page_size);

  void *qmem = pmm_alloc_pages_dma(qpages);
  if (!qmem) {
    pr_err("VirtIO: Failed to allocate %u pages for queue memory\n",
           (unsigned)qpages);
    return;
  }
  memset(qmem, 0, (size_t)qpages * queue_page_size);

  desc = (struct vring_desc *)qmem;
  avail = (struct vring_avail *)((uint8_t *)qmem + desc_bytes);
  used = (struct vring_used *)((uint8_t *)qmem + used_offset);
  pr_info("VirtIO: Block queue layout qsize=%u pages=%u avail_off=%u "
          "used_off=%u\n",
          qsize, qpages, (unsigned)desc_bytes, (unsigned)used_offset);

  /* Unified HAL API handles Legacy/Modern address registration.
   * The device needs PHYSICAL ring addresses (virt_to_phys). */
  virtio_setup_queue(dev, 0, virt_to_phys(desc), virt_to_phys(avail),
                     virt_to_phys(used));

  /* Driver OK */
  status |= VIRTIO_STATUS_DRIVER_OK;
  virtio_write_reg(dev, VIRTIO_MMIO_STATUS, status);

  pr_info("%s", "VirtIO: Block Device Initialized successfully\n");

  /* Register as the active block backend (ASTRA block contract).  A boot
   * module, if present, overrides this later in main.c. */
  static const struct block_dev virtio_blk_bdev = {
      "virtio-blk", virtio_blk_read, virtio_blk_write};
  block_register(&virtio_blk_bdev);
}

/*
 * virtio_blk_xfer: issue one synchronous block request (busy-wait).
 *
 * @write: 0 = VIRTIO_BLK_T_IN (device writes @buf), 1 = VIRTIO_BLK_T_OUT.
 *
 * Caller context: process/boot only (the poll loop with IRQs masked makes it
 * unsuitable for IRQ context).  Serialised by virtio_blk_lock — see the lock
 * comment for the SMP corruption this prevents.
 */
static int virtio_blk_xfer(int write, void *buf, uint64_t sector,
                           uint32_t count) {
  if (!virtio_blk_dev || !desc || !avail || !used)
    return -1;

  uint64_t flags;
  spin_lock_irqsave(&virtio_blk_lock, &flags);

  blk_req.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  blk_req.reserved = 0;
  blk_req.sector = sector;
  blk_status = 0xFF; /* poisoned: device must overwrite it on completion */

  /* Descriptor addresses are PHYSICAL (DMA): virt_to_phys on kernel ptrs. */
  /* Header (read-only for the device) */
  desc[0].addr = virt_to_phys(&blk_req);
  desc[0].len = sizeof(struct virtio_blk_req);
  desc[0].flags = VRING_DESC_F_NEXT;
  desc[0].next = 1;

  /* Data buffer (device writes it on reads, reads it on writes) */
  desc[1].addr = virt_to_phys(buf);
  desc[1].len = count * 512;
  desc[1].flags = (write ? 0 : VRING_DESC_F_WRITE) | VRING_DESC_F_NEXT;
  desc[1].next = 2;

  /* Status byte (write-only for the device) */
  desc[2].addr = virt_to_phys((void *)&blk_status);
  desc[2].len = 1;
  desc[2].flags = VRING_DESC_F_WRITE;
  desc[2].next = 0;

  /* Snapshot the used index BEFORE publishing the request (DRV-VIRTIO-04):
   * snapshotting after avail->idx++ could miss a completion that lands
   * between publish and snapshot, and then poll for one that never comes. */
  volatile uint16_t *used_idx_ptr = &used->idx;

  /* FIX(VBLK-DMA-01): invalidate 'used' before the snapshot read, for the
   * same reason as virtio_gpu.c's virtio_gpu_send() (VGPU-DMA-01): a
   * cached copy of this line from a previous request's poll loop can be
   * stale, corrupting old_idx before the request is even issued. */
  arch_cache_invalidate_range((void *)used, sizeof(*used));
  uint16_t old_idx = *used_idx_ptr;

  /* Publish in the Available Ring */
  uint16_t idx = avail->idx % virtio_blk_qsize;
  avail->ring[idx] = 0;
  avail->idx++;

  /* FIX(VBLK-DMA-02): the device DMA-reads blk_req, desc[] and avail — all
   * plain cacheable RAM (pmm_alloc_pages_dma() cache-cleans them only once,
   * at allocation) — same hazard as VGPU-DMA-02 in virtio_gpu.c.  hal_mb()
   * orders memory operations already visible to the coherency system; it
   * does not push a dirty cache line out to physical RAM.  Clean the
   * request header, the data buffer on a WRITE (the device reads it), and
   * the descriptor/avail ring unconditionally, before notifying. */
  arch_cache_clean_range(&blk_req, sizeof(blk_req));
  if (write)
    arch_cache_clean_range(buf, (size_t)count * 512U);
  else
    arch_cache_invalidate_range(buf, (size_t)count * 512U);
  arch_cache_clean_range((void *)desc, 3 * sizeof(*desc));
  arch_cache_clean_range(
      (void *)avail,
      sizeof(*avail) +
          (size_t)virtio_blk_qsize * sizeof(uint16_t) + sizeof(uint16_t));
  hal_mb();

  /* Notify */
  virtio_notify(virtio_blk_dev, 0);

  /* Poll the Used Ring against the free-running hardware counter.
   * A fixed 1,000,000,000-iteration loop is not a time bound: its duration
   * changes with CPU/ISA/emulator speed and can monopolise the BSP while
   * IRQs are masked by virtio_blk_lock. Use a measured 2 s deadline instead.
   * The counter is available before userland and does not depend on jiffies
   * or on the timer interrupt being enabled. */
  const uint64_t timer_freq = arch_timer_get_freq();
  const uint64_t timeout_cycles =
      (timer_freq / 1000ULL) * 2000ULL; /* 2,000 ms in hardware-counter units */
  const uint64_t timeout_start = arch_timer_get_count();
  uint64_t polls = 0;
  int completed = 0;

  while (arch_timer_get_count() - timeout_start < timeout_cycles) {
    arch_cache_invalidate_range((void *)used, sizeof(*used));
    if (*used_idx_ptr != old_idx) {
      completed = 1;
      break;
    }
    hal_cpu_yield();
    ++polls;
  }

  /* Read/ack the interrupt status exactly once: legacy VirtIO acknowledges
   * the ISR on read, so a second read would erase the diagnostic evidence. */
  const uint32_t isr =
      virtio_read_reg(virtio_blk_dev, VIRTIO_MMIO_INTERRUPT_ACK);

  if (!completed) {
    const uint64_t elapsed_cycles =
        arch_timer_get_count() - timeout_start;
    const uint64_t elapsed_ms =
        timer_freq ? (elapsed_cycles * 1000ULL) / timer_freq : 0;
    const uint32_t device_status =
        virtio_read_reg(virtio_blk_dev, VIRTIO_MMIO_STATUS);
    pr_err("VirtIO-Blk: timeout after %llu ms; used=%u old=%u avail=%u "
           "qsize=%u blk_status=%02x dev_status=%02x ISR=%02x polls=%llu\n",
           (unsigned long long)elapsed_ms, (unsigned)*used_idx_ptr,
           (unsigned)old_idx, (unsigned)avail->idx,
           (unsigned)virtio_blk_qsize, (unsigned)blk_status,
           (unsigned)device_status, (unsigned)isr,
           (unsigned long long)polls);
    spin_unlock_irqrestore(&virtio_blk_lock, flags);
    return -1;
  }

  /* FIX(VBLK-DMA-04): invalidate the status byte and, on a READ, the data
   * buffer, before this CPU reads what the device wrote via DMA — same
   * hazard as VGPU-DMA-04 in virtio_gpu.c. */
  arch_cache_invalidate_range((void *)&blk_status, sizeof(blk_status));
  if (!write)
    arch_cache_invalidate_range(buf, count * 512);

  /* Order the status read after the used-ring update we just observed. */
  hal_mb();
  uint8_t status = blk_status;
  spin_unlock_irqrestore(&virtio_blk_lock, flags);

  if (status != VIRTIO_BLK_S_OK) {
    pr_info("VirtIO: %s failed status=%d\n", write ? "Write" : "Read", status);
    return -1;
  }

  return 0;
}

/*
 * virtio_blk_read: Read sectors from disk
 * Synchronous implementation (busy-wait), serialised by virtio_blk_lock.
 */
int virtio_blk_read(void *buf, uint64_t sector, uint32_t count) {
  return virtio_blk_xfer(0, buf, sector, count);
}

int virtio_blk_write(void *buf, uint64_t sector, uint32_t count) {
  return virtio_blk_xfer(1, buf, sector, count);
}