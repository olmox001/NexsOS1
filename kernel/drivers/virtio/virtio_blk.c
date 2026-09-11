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

  void *qmem = pmm_alloc_pages_dma(2);
  if (!qmem) {
    pr_err("%s", "VirtIO: Failed to allocate queue memory\n");
    return;
  }
  memset(qmem, 0, 8192);

  desc = (struct vring_desc *)qmem;
  avail = (struct vring_avail *)((uint8_t *)qmem + qsize * 16);
  used = (struct vring_used *)((uint8_t *)qmem + 4096);

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
    arch_cache_clean_range(buf, count * 512);
  arch_cache_clean_range((void *)desc, 3 * sizeof(*desc));
  arch_cache_clean_range((void *)avail, sizeof(*avail));
  hal_mb();

  /* Notify */
  virtio_notify(virtio_blk_dev, 0);

  /* Poll Used Ring (busy wait).
   * FIX(VBLK-DMA-03): invalidate 'used' every iteration before re-reading
   * it — without this a cached read can keep hitting the same stale line
   * forever even after the device's completion write actually landed in
   * RAM, on any hardware where that write is not cache-coherent with this
   * core (see VGPU-DMA-03 in virtio_gpu.c for the identical hazard).
   *
   * FIX(VBLK-STALL-01): the timeout budget used to be a raw ITERATION
   * count (1e9), not a bounded wall-clock time. This loop runs with
   * virtio_blk_lock held and this CPU's local IRQs disabled (see the lock
   * comment above) — every device driver on this CPU, this CPU's local
   * timer tick, and (via the lock) every OTHER CPU trying to issue block
   * I/O concurrently are all stalled for however long this loop actually
   * takes to give up. An iteration count has no fixed real-time meaning:
   * this exact pass added a clflush + two mfences to every single
   * iteration (arch_cache_invalidate_range), which multiplies the real
   * wall-clock cost of "give up after 1e9 tries" by however expensive
   * that turns out to be on the running hardware — turning an already
   * risky "hold a global lock with IRQs off across a device wait" design
   * into one whose worst-case duration is not just large but genuinely
   * unbounded and unpredictable. If this CPU happens to be the one
   * driving jiffies (kernel/core/timer.c), that reads back as "the clock
   * stopped"; system-wide it reads back as a full stall, matching exactly
   * the class of intermittent hang this driver was audited for. Bounded
   * to a fixed wall-clock deadline instead (same arch_timer_get_freq()/
   * arch_timer_get_count() seam already used by kernel/core/smp.c's own
   * bounded wait), so the worst case is a known, small, real duration
   * regardless of what future changes add to the loop body. A normal
   * virtio round trip completes in microseconds; 500 ms is generous
   * headroom for host scheduling jitter without letting a genuinely wedged
   * device hold the whole system's IRQs off for anything close to as long
   * as the old budget could. */
  uint64_t vblk_deadline =
      arch_timer_get_count() + arch_timer_get_freq() / 2ULL; /* +500 ms */
  int completed = 0;
  while ((int64_t)(vblk_deadline - arch_timer_get_count()) > 0) {
    arch_cache_invalidate_range((void *)used, sizeof(*used));
    if (*used_idx_ptr != old_idx) {
      completed = 1;
      break;
    }
    hal_cpu_yield();
  }

  /* Clear interrupt status (Legacy VirtIO requirement) */
  virtio_read_reg(virtio_blk_dev, VIRTIO_MMIO_INTERRUPT_ACK);

  if (!completed) {
    uint32_t isr = virtio_read_reg(virtio_blk_dev, VIRTIO_MMIO_INTERRUPT_ACK);
    pr_err("VirtIO-Blk: Timeout waiting for device response! (used->idx=%d "
           "old_idx=%d, ISR=%02x)\n",
           *used_idx_ptr, old_idx, isr);
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