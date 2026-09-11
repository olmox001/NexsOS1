/*
 * kernel/drivers/gpu/virtio_gpu.c
 * VirtIO GPU Driver — single arch-neutral service over the HAL virtio
 * transport.
 *
 * The same driver drives virtio-gpu on both transports: VirtIO-MMIO on the
 * AArch64 QEMU virt machine and VirtIO-over-PCI on amd64.  It never touches the
 * ISA — it speaks only the transport contract (virtio_read_reg /
 * virtio_write_reg / virtio_notify / virtio_setup_queue) and the GPU contract
 * (struct gpu_ops).
 *
 * GFX-DYN-01:
 *  - #54: resolution is queried from the host with GET_DISPLAY_INFO, not the
 *    old hard-coded 720x1280.
 *  - #49: vgpu_set_mode() really reconfigures the scanout to a new size.
 *  - #53: the virtqueue rings (desc/avail/used) and command/response buffers
 *    are per-device (in struct virtio_gpu_state), not module globals; all ring
 *    access is serialised by gpu_lock, and init runs to completion before
 *    gpu_register() publishes the device (so flush/set_mode cannot race init).
 */
#include <drivers/gpu/gpu.h>
#include <drivers/virtio.h>
#include <drivers/virtio_gpu.h>
#include <kernel/arch.h>
#include <kernel/fault.h>
#include <kernel/kmalloc.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

/* Default fallback when the host advertises no usable mode (landscape, not the
 * old portrait 720x1280 that forced sideways rendering — #54). */
#define VGPU_DEFAULT_W 1024
#define VGPU_DEFAULT_H 768

/* virtio-gpu device-config field offsets (struct virtio_gpu_config), read via
 * the transport-agnostic virtio_config_read32/write32 (works on MMIO + PCI). */
#define VGPU_CFG_EVENTS_READ 0
#define VGPU_CFG_EVENTS_CLEAR 4

struct virtio_gpu_state {
  virtio_handle_t handle;
  uint32_t qsize;
  void *backing_store;  /* guest framebuffer (DMA backing of the resource) */
  uint32_t resource_id; /* current scanout resource id */
  uint32_t next_resource_id;
  struct gpu_device *dev;

  /* Per-device control virtqueue (queue 0) — was module-global (#53). */
  struct vring_desc *desc;
  struct vring_avail *avail;
  struct vring_used *used;
  void *cmd_buf;  /* DMA page for outgoing control commands */
  void *resp_buf; /* DMA page for responses */
};

static DEFINE_SPINLOCK(gpu_lock);

static int virtio_gpu_send(struct virtio_gpu_state *priv, void *cmd,
                           uint32_t cmd_len, void *resp, uint32_t resp_len);

/* ------------------------------------------------------------------------- */
/* Control-queue command helpers (caller serialises via gpu_lock or runs at */
/* init before the device is published). */
/* ------------------------------------------------------------------------- */

static int vgpu_create_2d(struct virtio_gpu_state *priv, uint32_t res_id, int w,
                          int h) {
  memset(priv->cmd_buf, 0, 4096);
  struct virtio_gpu_resource_create_2d *c = priv->cmd_buf;
  c->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
  c->resource_id = res_id;
  c->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
  c->width = w;
  c->height = h;
  return virtio_gpu_send(priv, c, sizeof(*c), priv->resp_buf,
                         sizeof(struct virtio_gpu_ctrl_hdr));
}

static int vgpu_attach_backing(struct virtio_gpu_state *priv, uint32_t res_id,
                               void *backing, uint32_t len) {
  memset(priv->cmd_buf, 0, 4096);
  struct virtio_gpu_resource_attach_backing *a = priv->cmd_buf;
  struct virtio_gpu_mem_entry *ent =
      (struct virtio_gpu_mem_entry *)((uint8_t *)priv->cmd_buf + sizeof(*a));
  a->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
  a->resource_id = res_id;
  a->nr_entries = 1;
  ent->addr = virt_to_phys(backing); /* device DMA: physical address */
  ent->length = len;
  return virtio_gpu_send(priv, a, sizeof(*a) + sizeof(*ent), priv->resp_buf,
                         sizeof(struct virtio_gpu_ctrl_hdr));
}

static int vgpu_set_scanout(struct virtio_gpu_state *priv, uint32_t res_id,
                            int w, int h) {
  memset(priv->cmd_buf, 0, 4096);
  struct virtio_gpu_set_scanout *s = priv->cmd_buf;
  s->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
  s->scanout_id = 0;
  s->resource_id = res_id;
  s->r.x = 0;
  s->r.y = 0;
  s->r.width = w;
  s->r.height = h;
  return virtio_gpu_send(priv, s, sizeof(*s), priv->resp_buf,
                         sizeof(struct virtio_gpu_ctrl_hdr));
}

static int vgpu_unref(struct virtio_gpu_state *priv, uint32_t res_id) {
  memset(priv->cmd_buf, 0, 4096);
  struct virtio_gpu_resource_unref *u = priv->cmd_buf;
  u->hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
  u->resource_id = res_id;
  return virtio_gpu_send(priv, u, sizeof(*u), priv->resp_buf,
                         sizeof(struct virtio_gpu_ctrl_hdr));
}

/* Query the host's preferred display size via GET_DISPLAY_INFO (#54).  Returns
 * 0 and fills w/h with the first enabled scanout's size, -1 on failure or if no
 * scanout reports a usable size. */
static int vgpu_query_display_info(struct virtio_gpu_state *priv, int *w,
                                   int *h) {
  memset(priv->cmd_buf, 0, 4096);
  memset(priv->resp_buf, 0, 4096);
  struct virtio_gpu_ctrl_hdr *cmd = priv->cmd_buf;
  cmd->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

  struct virtio_gpu_resp_display_info *resp = priv->resp_buf;
  if (virtio_gpu_send(priv, cmd, sizeof(*cmd), resp, sizeof(*resp)) != 0)
    return -1;
  if (resp->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
    return -1;

  for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
    if (resp->pmodes[i].enabled && resp->pmodes[i].r.width > 0 &&
        resp->pmodes[i].r.height > 0) {
      if (w)
        *w = (int)resp->pmodes[i].r.width;
      if (h)
        *h = (int)resp->pmodes[i].r.height;
      return 0;
    }
  }
  return -1;
}

/* ------------------------------------------------------------------------- */
/* gpu_ops */
/* ------------------------------------------------------------------------- */

/* vgpu_xfer_flush_locked - issue TRANSFER_TO_HOST_2D + RESOURCE_FLUSH for the
 * region (x,y,w,h) against the CURRENT scanout resource.  Caller MUST hold
 * gpu_lock (or be in single-threaded fault context): it touches priv->cmd_buf
 * and priv->resource_id, which set_mode also mutates. */
static void vgpu_xfer_flush_locked(struct virtio_gpu_state *priv,
                                   struct gpu_device *dev, int x, int y, int w,
                                   int h) {
  /* 1. Transfer guest memory to the host resource. */
  memset(priv->cmd_buf, 0, 4096);
  struct virtio_gpu_transfer_to_host_2d *xfer = priv->cmd_buf;
  xfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
  xfer->r.x = x;
  xfer->r.y = y;
  xfer->r.width = w;
  xfer->r.height = h;
  xfer->offset = (uint64_t)y * dev->width * 4 + (x * 4);
  xfer->resource_id = priv->resource_id;
  virtio_gpu_send(priv, xfer, sizeof(*xfer), priv->resp_buf,
                  sizeof(struct virtio_gpu_ctrl_hdr));

  /* 2. Flush the host resource to the display. */
  memset(priv->cmd_buf, 0, 4096);
  struct virtio_gpu_resource_flush *fl = priv->cmd_buf;
  fl->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
  fl->r.x = x;
  fl->r.y = y;
  fl->r.width = w;
  fl->r.height = h;
  fl->resource_id = priv->resource_id;
  virtio_gpu_send(priv, fl, sizeof(*fl), priv->resp_buf,
                  sizeof(struct virtio_gpu_ctrl_hdr));
}

static int vgpu_flush(struct gpu_device *dev, int x, int y, int w, int h) {
  if (!dev || !dev->priv)
    return -1;
  struct virtio_gpu_state *priv = (struct virtio_gpu_state *)dev->priv;

  /* In a panic/fault context the gpu_lock holder may be a now-halted CPU; the
   * system is single-threaded by then (panic quiesced the others), so skip the
   * lock to present the on-screen panic without deadlocking (DIR-05 #139). */
  int faulting = (fault_depth() > 0);
  uint64_t flags = 0;
  if (!faulting)
    spin_lock_irqsave(&gpu_lock, &flags);

  vgpu_xfer_flush_locked(priv, dev, x, y, w, h);

  if (!faulting)
    spin_unlock_irqrestore(&gpu_lock, flags);
  return 0;
}

/* vgpu_present - RC2 fix: copy the backbuffer region into the scanout backing
 * AND transfer+flush it, atomically under gpu_lock.  This is what makes the
 * compositor↔GPU seam safe against a concurrent set_mode: the same lock that
 * vgpu_set_mode() takes to swap priv->backing_store and free the old buffer is
 * held for the entire copy, so the scanout can never be freed mid-memcpy (the
 * use-after-free behind the resize/zoom panics).  Geometry is validated against
 * the LIVE scanout under the lock; a mismatch (a mode change landed first)
 * skips the frame instead of copying past the new, smaller backing. */
static int vgpu_present(struct gpu_device *dev, const uint32_t *src, int src_w,
                        int src_h, int x, int y, int w, int h) {
  if (!dev || !dev->priv || !src)
    return -1;
  struct virtio_gpu_state *priv = (struct virtio_gpu_state *)dev->priv;

  uint64_t flags;
  spin_lock_irqsave(&gpu_lock, &flags);

  /* The source (compositor backbuffer) must match the CURRENT scanout exactly:
   * both are resized together, but through different locks, so re-check here.
   * If a set_mode already changed dev->{width,height}, skip — the compositor
   * will present again next frame at the new size. */
  uint32_t *fb = (uint32_t *)priv->backing_store;
  if (!fb || src_w != dev->width || src_h != dev->height) {
    spin_unlock_irqrestore(&gpu_lock, flags);
    return -1;
  }

  /* Clamp the region to the scanout (defensive; the compositor already clips)
   */
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > dev->width)
    w = dev->width - x;
  if (y + h > dev->height)
    h = dev->height - y;
  if (w <= 0 || h <= 0) {
    spin_unlock_irqrestore(&gpu_lock, flags);
    return 0;
  }

  /* Row-by-row copy backbuffer -> scanout backing (both stride == dev->width).
   */
  for (int row = y; row < y + h; row++) {
    memcpy((uint8_t *)fb + ((size_t)row * dev->width + x) * 4,
           (const uint8_t *)src + ((size_t)row * src_w + x) * 4, (size_t)w * 4);
  }

  vgpu_xfer_flush_locked(priv, dev, x, y, w, h);

  spin_unlock_irqrestore(&gpu_lock, flags);
  return 0;
}

static void *vgpu_get_framebuffer(struct gpu_device *dev, size_t *size) {
  if (!dev || !dev->priv)
    return NULL;
  struct virtio_gpu_state *priv = (struct virtio_gpu_state *)dev->priv;
  if (size)
    *size = dev->framebuffer_size;
  return priv->backing_store;
}

/* Reconfigure the scanout to width x height at runtime (#49).
 *
 * Sequence under gpu_lock: create a fresh resource at the new size, allocate &
 * attach a new backing store, point the scanout at it, then release the old
 * resource and backing.  dev->{width,height,framebuffer_size} and
 * priv->backing_store are updated so the compositor's next get_framebuffer()
 * + flush() target the new surface.
 */
static int vgpu_set_mode(struct gpu_device *dev, int width, int height) {
  if (!dev || !dev->priv)
    return -1;
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
    return -1;
  struct virtio_gpu_state *priv = (struct virtio_gpu_state *)dev->priv;

  if (width == dev->width && height == dev->height)
    return 0; /* nothing to do */

  size_t new_size = (size_t)width * height * 4;
  int new_pages = (new_size + 4095) / 4096;
  void *new_backing = pmm_alloc_pages_dma(new_pages);
  if (!new_backing) {
    pr_err("VirtIO-GPU: set_mode %dx%d: out of memory\n", width, height);
    return -1;
  }
  memset(new_backing, 0, new_size);

  uint64_t flags;
  spin_lock_irqsave(&gpu_lock, &flags);

  uint32_t old_res = priv->resource_id;
  void *old_backing = priv->backing_store;
  size_t old_size = dev->framebuffer_size;
  uint32_t new_res = priv->next_resource_id++;

  if (vgpu_create_2d(priv, new_res, width, height) != 0 ||
      vgpu_attach_backing(priv, new_res, new_backing, (uint32_t)new_size) !=
          0 ||
      vgpu_set_scanout(priv, new_res, width, height) != 0) {
    /* Roll back: keep the old resource/backing as the live scanout. */
    spin_unlock_irqrestore(&gpu_lock, flags);
    pmm_free_pages(new_backing, new_pages);
    pr_err("VirtIO-GPU: set_mode %dx%d failed; keeping %dx%d\n", width, height,
           dev->width, dev->height);
    return -1;
  }

  /* New scanout is live: publish it and release the old resource. */
  priv->resource_id = new_res;
  priv->backing_store = new_backing;
  dev->width = width;
  dev->height = height;
  dev->framebuffer_size = new_size;

  vgpu_unref(priv, old_res);
  spin_unlock_irqrestore(&gpu_lock, flags);

  if (old_backing)
    pmm_free_pages(old_backing, (old_size + 4095) / 4096);

  pr_info("VirtIO-GPU: mode set to %dx%d (resource %u)\n", width, height,
          new_res);
  return 0;
}

static int vgpu_get_display_info(struct gpu_device *dev, int *width,
                                 int *height) {
  if (!dev || !dev->priv)
    return -1;
  struct virtio_gpu_state *priv = (struct virtio_gpu_state *)dev->priv;
  uint64_t flags;
  spin_lock_irqsave(&gpu_lock, &flags);
  int r = vgpu_query_display_info(priv, width, height);
  spin_unlock_irqrestore(&gpu_lock, flags);
  return r;
}

/* Poll the device-config events register for a host-initiated display change.
 * Returns 1 (and the new size) if the host resized the output to a different
 * resolution, 0 if nothing changed.  Reads device config via the
 * transport-agnostic virtio_config_read32/write32 (F7), so host-driven
 * auto-resize works on both VirtIO-MMIO (aarch64) and VirtIO-PCI (amd64). */
static int vgpu_poll_events(struct gpu_device *dev, int *new_w, int *new_h) {
  if (!dev || !dev->priv)
    return -1;
  struct virtio_gpu_state *priv = (struct virtio_gpu_state *)dev->priv;

  uint32_t events = virtio_config_read32(priv->handle, VGPU_CFG_EVENTS_READ);
  if (!(events & VIRTIO_GPU_EVENT_DISPLAY))
    return 0;

  /* Acknowledge the event. */
  virtio_config_write32(priv->handle, VGPU_CFG_EVENTS_CLEAR,
                        VIRTIO_GPU_EVENT_DISPLAY);

  int w = 0, h = 0;
  uint64_t flags;
  spin_lock_irqsave(&gpu_lock, &flags);
  int r = vgpu_query_display_info(priv, &w, &h);
  spin_unlock_irqrestore(&gpu_lock, flags);
  if (r != 0)
    return 0;

  if (w == dev->width && h == dev->height)
    return 0; /* event but same size */

  if (new_w)
    *new_w = w;
  if (new_h)
    *new_h = h;
  return 1;
}

static void vgpu_destroy(struct gpu_device *dev) {
  kfree(dev->priv);
  kfree(dev);
}

static struct gpu_ops vgpu_ops = {
    .flush = vgpu_flush,
    .get_framebuffer = vgpu_get_framebuffer,
    .present = vgpu_present,
    .set_mode = vgpu_set_mode,
    .destroy = vgpu_destroy,
    .get_display_info = vgpu_get_display_info,
    .poll_events = vgpu_poll_events,
};

/* ------------------------------------------------------------------------- */
/* Raw control-queue transport */
/* ------------------------------------------------------------------------- */

static int virtio_gpu_send(struct virtio_gpu_state *priv, void *cmd,
                           uint32_t cmd_len, void *resp, uint32_t resp_len) {
  if (!priv->handle)
    return -1;

  struct vring_desc *desc = priv->desc;
  struct vring_avail *avail = priv->avail;
  struct vring_used *used = priv->used;

  /* Descriptor addresses are PHYSICAL (DMA). */
  desc[0].addr = virt_to_phys(cmd);
  desc[0].len = cmd_len;
  desc[0].flags = VRING_DESC_F_NEXT;
  desc[0].next = 1;

  volatile uint16_t *idx_ptr = &used->idx;

  /* FIX(VGPU-DMA-01): invalidate 'used' before reading old_idx.  This CPU
   * (or whichever CPU last polled this queue) may still hold a cached
   * copy of this cache line from the previous command's poll loop below;
   * without discarding it first, old_idx can be read from stale cache
   * instead of the real value the device last wrote, corrupting every
   * subsequent "did the device respond yet" comparison for this call. */
  arch_cache_invalidate_range((void *)used, sizeof(*used));
  uint16_t old_idx = *idx_ptr;

  desc[1].addr = virt_to_phys(resp);
  desc[1].len = resp_len;
  desc[1].flags = VRING_DESC_F_WRITE;
  desc[1].next = 0;

  uint16_t ava_slot = avail->idx % priv->qsize;
  avail->ring[ava_slot] = 0;
  avail->idx++;

  /* FIX(VGPU-DMA-02): the device DMA-reads 'cmd' (the caller-filled command
   * buffer), desc[] and avail — all plain cacheable RAM allocated by
   * pmm_alloc_pages_dma(), which cache-cleans them ONLY once, at
   * allocation.  Every write this function makes to desc[]/avail (and
   * every write the caller made to 'cmd' just before calling this) can sit
   * in this CPU's cache indefinitely: arch_mb()/mfence orders memory
   * operations already visible to the coherency system, it does NOT push
   * a dirty cache line out to physical RAM.  Under QEMU/KVM there is no
   * real cache the device could see stale data in (the "device" is
   * software touching the same emulated RAM array), so this was invisible
   * there; on real hardware, whether the device ever sees these writes
   * depends on unrelated platform cache/DMA-coherency behaviour.  Cleaning
   * cmd/desc/avail here, unconditionally, on every send, removes that
   * dependency instead of assuming it away. */
  arch_cache_clean_range(cmd, cmd_len);
  arch_cache_clean_range((void *)desc, 2 * sizeof(*desc));
  arch_cache_clean_range((void *)avail, sizeof(*avail));
  arch_mb();

  virtio_notify(priv->handle, 0);

  /* FIX(VGPU-POLL-01): this loop used to spin with no CPU-relax hint,
   * hammering the same cache line (*idx_ptr) as fast as the core can issue
   * loads. On real hardware this is a tight-loop bus/cache-coherency hog
   * that competes with the device's own DMA write to that same line —
   * never visible under QEMU/KVM's emulated memory, where there is no real
   * bus contention. arch_yield() (PAUSE/YIELD/WFE depending on arch) is a
   * no-op for correctness but lets the core back off between polls,
   * matching how every other busy-wait in this tree is written.
   *
   * FIX(VGPU-DMA-03): each iteration also invalidates 'used' before the
   * re-read.  Without this, once the CPU has cached *idx_ptr == old_idx
   * once, a plain reload can keep hitting that same cached line forever
   * even after the device's DMA write actually landed in RAM — the loop
   * would then spin until pure timeout every single time on any hardware
   * where the device write is not cache-coherent with this core, not just
   * intermittently.
   *
   * FIX(VGPU-STALL-01): the timeout budget used to be a raw ITERATION
   * count (2e8), not a bounded wall-clock time. Every caller of this
   * function that goes through vgpu_flush()/vgpu_xfer_flush_locked() (i.e.
   * every compositor frame — nxinit drives this at ~30 FPS from its own
   * process context, see kernel/graphics/compositor.c and user/sys/bin/
   * nxinit.c) runs this loop TWICE per frame with gpu_lock held and this
   * CPU's local IRQs disabled. An iteration count has no fixed real-time
   * meaning, and this exact pass added a clflush + mfence to every single
   * iteration (arch_cache_invalidate_range), multiplying the real
   * wall-clock cost of "give up after 2e8 tries" by however expensive that
   * turns out to be — an already risky "hold a lock with IRQs off across a
   * device wait, on every frame" design gained a genuinely unbounded,
   * unpredictable worst case. If this stalls on whichever CPU is driving
   * jiffies (kernel/core/timer.c) — which for this driver is exactly the
   * CPU calling flush() from nxinit's supervisor loop — that reads back as
   * "the clock stopped" and "the whole desktop froze" at the same time:
   * precisely the class of intermittent full-system hang this driver was
   * audited for. Bounded to a fixed wall-clock deadline instead (same
   * arch_timer_get_freq()/arch_timer_get_count() seam already used by
   * kernel/core/smp.c's own bounded wait and by virtio_blk.c's matching
   * fix), so the worst case per call is a known, small, real duration
   * regardless of what future changes add to the loop body. A normal
   * virtio-gpu round trip completes in microseconds; 500 ms is generous
   * headroom for host scheduling jitter without letting one wedged frame
   * hold the whole system's IRQs off anywhere near as long as the old
   * budget could. */
  uint64_t vgpu_deadline =
      arch_timer_get_count() + arch_timer_get_freq() / 2ULL; /* +500 ms */
  int completed = 0;
  while ((int64_t)(vgpu_deadline - arch_timer_get_count()) > 0) {
    arch_cache_invalidate_range((void *)used, sizeof(*used));
    if (*idx_ptr != old_idx) {
      completed = 1;
      break;
    }
    arch_yield();
  }

  /* FIX(VGPU-POLL-02): VIRTIO_MMIO_INTERRUPT_ACK used to be written
   * unconditionally, even on timeout — acknowledging an interrupt that may
   * never have been asserted, and masking the fact that this command never
   * got a response. Only ACK (and only report success) once the used-ring
   * index has actually advanced; a timeout is reported as the failure it
   * is, with no ACK write, so a caller retrying (or the caller's caller,
   * e.g. compositor_init()'s single vgpu_create_2d/attach/set_scanout
   * chain) sees a real -1 instead of a silently swallowed timeout. */
  if (!completed) {
    pr_err("%s", "VirtIO-GPU: Timeout waiting for device response!\n");
    return -1;
  }

  /* FIX(VGPU-DMA-04): invalidate 'resp' before the caller reads it — same
   * stale-cache hazard as 'used' above, for the response payload itself
   * (e.g. GET_DISPLAY_INFO's mode list, read by vgpu_query_display_info()
   * immediately after this call returns). */
  arch_cache_invalidate_range(resp, resp_len);

  virtio_read_reg(priv->handle, VIRTIO_MMIO_INTERRUPT_ACK);
  return 0;
}

void virtio_gpu_init(void) {
  pr_info("%s", "VirtIO-GPU: Probing...\n");

  virtio_handle_t dev_handle = NULL;
  uint32_t irq = 0;

  if (arch_virtio_get_device(VIRTIO_DEV_GPU, 0, &dev_handle, &irq) != 0) {
    pr_info("%s", "VirtIO-GPU: Not found\n");
    return;
  }
  pr_info("VirtIO-GPU: Found device (IRQ %u)\n", irq);

  struct gpu_device *dev = kmalloc(sizeof(struct gpu_device));
  struct virtio_gpu_state *priv = kmalloc(sizeof(struct virtio_gpu_state));
  if (!dev || !priv) {
    if (dev)
      kfree(dev);
    if (priv)
      kfree(priv);
    return;
  }
  memset(dev, 0, sizeof(*dev));
  memset(priv, 0, sizeof(*priv));

  priv->handle = dev_handle;
  priv->dev = dev;
  dev->priv = priv;
  dev->ops = &vgpu_ops;
  strlcpy(dev->name, "VirtIO-GPU", sizeof(dev->name));

  /* Reset + Acknowledge + Driver. */
  virtio_write_reg(dev_handle, VIRTIO_MMIO_STATUS, 0);
  uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
  virtio_write_reg(dev_handle, VIRTIO_MMIO_STATUS, status);

  /* Feature negotiation. */
  uint32_t features = virtio_read_reg(dev_handle, VIRTIO_MMIO_DEVICE_FEATURES);
  virtio_write_reg(dev_handle, VIRTIO_MMIO_DRIVER_FEATURES, features);
  status |= VIRTIO_STATUS_FEATURES_OK;
  virtio_write_reg(dev_handle, VIRTIO_MMIO_STATUS, status);
  if (!(virtio_read_reg(dev_handle, VIRTIO_MMIO_STATUS) &
        VIRTIO_STATUS_FEATURES_OK)) {
    pr_err("%s", "VirtIO-GPU: Negotiation failed\n");
    kfree(dev);
    kfree(priv);
    return;
  }

  /* Control queue (queue 0) setup — per-device rings (#53). */
  virtio_write_reg(dev_handle, VIRTIO_MMIO_QUEUE_SEL, 0);
  uint32_t qmax = virtio_read_reg(dev_handle, VIRTIO_MMIO_QUEUE_NUM_MAX);
  priv->qsize = (qmax > 16) ? 16 : qmax;
  virtio_write_reg(dev_handle, VIRTIO_MMIO_QUEUE_NUM, priv->qsize);

  void *qmem = pmm_alloc_pages_dma(2);
  memset(qmem, 0, 8192);
  priv->desc = (struct vring_desc *)qmem;
  priv->avail = (struct vring_avail *)((uint8_t *)qmem + priv->qsize * 16);
  priv->used = (struct vring_used *)((uint8_t *)qmem + 4096);

  virtio_setup_queue(dev_handle, 0, virt_to_phys(priv->desc),
                     virt_to_phys(priv->avail), virt_to_phys(priv->used));

  priv->cmd_buf = pmm_alloc_page_dma();
  priv->resp_buf = pmm_alloc_page_dma();

  /* Driver OK. */
  virtio_write_reg(dev_handle, VIRTIO_MMIO_STATUS,
                   VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                       VIRTIO_STATUS_DRIVER_OK);

  /* Resolution: ask the host (#54) instead of hard-coding 720x1280. */
  int qw = 0, qh = 0;
  if (vgpu_query_display_info(priv, &qw, &qh) != 0 || qw <= 0 || qh <= 0) {
    qw = VGPU_DEFAULT_W;
    qh = VGPU_DEFAULT_H;
    pr_info("VirtIO-GPU: no host mode reported, defaulting to %dx%d\n", qw, qh);
  } else {
    pr_info("VirtIO-GPU: host display %dx%d\n", qw, qh);
  }

  dev->width = qw;
  dev->height = qh;
  dev->bpp = 32;
  dev->framebuffer_size = (size_t)qw * qh * 4;

  int pages = (dev->framebuffer_size + 4095) / 4096;
  priv->backing_store = pmm_alloc_pages_dma(pages);
  memset(priv->backing_store, 0, dev->framebuffer_size);
  /* panic_screen/graphics and the compositor all reach this backing store
   * through the ops->get_framebuffer contract (S-ALIGN F7) — there is no
   * second field-level path any more (the old dev->framebuffer_virt started
   * life unassigned and silently no-op'd the red panic screen, DIR-05 #139). */
  priv->resource_id = 1;
  priv->next_resource_id = 2;

  /* Create the scanout resource at the queried size. */
  vgpu_create_2d(priv, priv->resource_id, dev->width, dev->height);
  vgpu_attach_backing(priv, priv->resource_id, priv->backing_store,
                      (uint32_t)dev->framebuffer_size);
  vgpu_set_scanout(priv, priv->resource_id, dev->width, dev->height);

  gpu_register(dev); /* publish last: flush/set_mode cannot race init (#53) */
}