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
#define VGPU_MAX_DIMENSION 8192u

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
  if (!priv || !priv->cmd_buf || !priv->resp_buf || w <= 0 || h <= 0 ||
      (uint32_t)w > VGPU_MAX_DIMENSION ||
      (uint32_t)h > VGPU_MAX_DIMENSION)
    return -1;
  memset(priv->cmd_buf, 0, 4096);
  struct virtio_gpu_resource_create_2d *c = priv->cmd_buf;
  c->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
  c->resource_id = res_id;
  c->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
  c->width = (uint32_t)w;
  c->height = (uint32_t)h;
  return virtio_gpu_send(priv, c, sizeof(*c), priv->resp_buf,
                         sizeof(struct virtio_gpu_ctrl_hdr));
}

static int vgpu_attach_backing(struct virtio_gpu_state *priv, uint32_t res_id,
                               void *backing, uint32_t len) {
  if (!priv || !priv->cmd_buf || !priv->resp_buf || !backing || len == 0)
    return -1;
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
  if (!priv || !priv->cmd_buf || !priv->resp_buf || w <= 0 || h <= 0 ||
      (uint32_t)w > VGPU_MAX_DIMENSION ||
      (uint32_t)h > VGPU_MAX_DIMENSION)
    return -1;
  memset(priv->cmd_buf, 0, 4096);
  struct virtio_gpu_set_scanout *s = priv->cmd_buf;
  s->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
  s->scanout_id = 0;
  s->resource_id = res_id;
  s->r.x = 0;
  s->r.y = 0;
  s->r.width = (uint32_t)w;
  s->r.height = (uint32_t)h;
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
    uint32_t mode_w = resp->pmodes[i].r.width;
    uint32_t mode_h = resp->pmodes[i].r.height;
    if (resp->pmodes[i].enabled && mode_w > 0 && mode_h > 0 &&
        mode_w <= VGPU_MAX_DIMENSION && mode_h <= VGPU_MAX_DIMENSION) {
      if (w)
        *w = (int)mode_w;
      if (h)
        *h = (int)mode_h;
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
static int vgpu_xfer_flush_locked(struct virtio_gpu_state *priv,
                                  struct gpu_device *dev, int x, int y, int w,
                                  int h) {
  /* Every caller clips against the live scanout before reaching this helper.
   * Keep a final guard here because these coordinates become unsigned wire
   * fields and are also used in a DMA backing offset. */
  if (!priv || !priv->cmd_buf || !priv->resp_buf || !dev || x < 0 || y < 0 ||
      w <= 0 || h <= 0 || x >= dev->width || y >= dev->height)
    return -1;
  if ((int64_t)x + w > dev->width)
    w = dev->width - x;
  if ((int64_t)y + h > dev->height)
    h = dev->height - y;
  if (w <= 0 || h <= 0)
    return 0;

  uint32_t ux = (uint32_t)x;
  uint32_t uy = (uint32_t)y;
  uint32_t uw = (uint32_t)w;
  uint32_t uh = (uint32_t)h;

  /* 1. Transfer guest memory to the host resource. */
  memset(priv->cmd_buf, 0, 4096);
  struct virtio_gpu_transfer_to_host_2d *xfer = priv->cmd_buf;
  xfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
  xfer->r.x = ux;
  xfer->r.y = uy;
  xfer->r.width = uw;
  xfer->r.height = uh;
  xfer->offset = (uint64_t)uy * (uint64_t)(uint32_t)dev->width * 4u +
                 (uint64_t)ux * 4u;
  xfer->resource_id = priv->resource_id;
  if (virtio_gpu_send(priv, xfer, sizeof(*xfer), priv->resp_buf,
                      sizeof(struct virtio_gpu_ctrl_hdr)) != 0)
    return -1;

  /* 2. Flush the host resource to the display. */
  memset(priv->cmd_buf, 0, 4096);
  struct virtio_gpu_resource_flush *fl = priv->cmd_buf;
  fl->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
  fl->r.x = ux;
  fl->r.y = uy;
  fl->r.width = uw;
  fl->r.height = uh;
  fl->resource_id = priv->resource_id;
  return virtio_gpu_send(priv, fl, sizeof(*fl), priv->resp_buf,
                         sizeof(struct virtio_gpu_ctrl_hdr));
}

static int vgpu_flush(struct gpu_device *dev, int x, int y, int w, int h) {
  if (!dev || !dev->priv || dev->width <= 0 || dev->height <= 0)
    return -1;
  if (w <= 0 || h <= 0)
    return 0;

  /* Clip in 64-bit signed space before converting to the unsigned VirtIO
   * rectangle fields; x+w/y+h must not overflow signed int. */
  int64_t x1 = x, y1 = y;
  int64_t x2 = (int64_t)x + w;
  int64_t y2 = (int64_t)y + h;
  if (x2 <= 0 || y2 <= 0 || x1 >= dev->width || y1 >= dev->height)
    return 0;
  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 > dev->width) x2 = dev->width;
  if (y2 > dev->height) y2 = dev->height;
  if (x2 <= x1 || y2 <= y1)
    return 0;

  x = (int)x1;
  y = (int)y1;
  w = (int)(x2 - x1);
  h = (int)(y2 - y1);
  struct virtio_gpu_state *priv = (struct virtio_gpu_state *)dev->priv;

  /* In a panic/fault context the gpu_lock holder may be a now-halted CPU; the
   * system is single-threaded by then (panic quiesced the others), so skip the
   * lock to present the on-screen panic without deadlocking (DIR-05 #139). */
  int faulting = (fault_depth() > 0);
  uint64_t flags = 0;
  if (!faulting)
    spin_lock_irqsave(&gpu_lock, &flags);

  int result = vgpu_xfer_flush_locked(priv, dev, x, y, w, h);

  if (!faulting)
    spin_unlock_irqrestore(&gpu_lock, flags);
  return result;
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

  if (src_w <= 0 || src_h <= 0 || dev->width <= 0 || dev->height <= 0) {
    spin_unlock_irqrestore(&gpu_lock, flags);
    return -1;
  }

  /* Clip in 64-bit signed space before any pointer arithmetic. */
  if (w <= 0 || h <= 0) {
    spin_unlock_irqrestore(&gpu_lock, flags);
    return 0;
  }
  int64_t x1 = x, y1 = y;
  int64_t x2 = (int64_t)x + w;
  int64_t y2 = (int64_t)y + h;
  if (x2 <= 0 || y2 <= 0 || x1 >= dev->width || y1 >= dev->height) {
    spin_unlock_irqrestore(&gpu_lock, flags);
    return 0;
  }
  if (x1 < 0) x1 = 0;
  if (y1 < 0) y1 = 0;
  if (x2 > dev->width) x2 = dev->width;
  if (y2 > dev->height) y2 = dev->height;
  if (x2 <= x1 || y2 <= y1) {
    spin_unlock_irqrestore(&gpu_lock, flags);
    return 0;
  }

  x = (int)x1;
  y = (int)y1;
  w = (int)(x2 - x1);
  h = (int)(y2 - y1);

  /* Row-by-row copy backbuffer -> scanout backing (both stride == dev->width).
   */
  size_t dst_stride = (size_t)(uint32_t)dev->width;
  size_t src_stride = (size_t)(uint32_t)src_w;
  size_t copy_x = (size_t)(uint32_t)x;
  size_t copy_bytes = (size_t)(uint32_t)w * sizeof(uint32_t);
  size_t end_y = (size_t)(uint32_t)(y + h);
  for (size_t row = (size_t)(uint32_t)y; row < end_y; row++) {
    size_t dst_offset = (row * dst_stride + copy_x) * sizeof(uint32_t);
    size_t src_offset = (row * src_stride + copy_x) * sizeof(uint32_t);
    memcpy((uint8_t *)fb + dst_offset, (const uint8_t *)src + src_offset,
           copy_bytes);
  }

  int present_result = vgpu_xfer_flush_locked(priv, dev, x, y, w, h);

  spin_unlock_irqrestore(&gpu_lock, flags);
  return present_result;
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

  size_t new_size = (size_t)(uint32_t)width * (size_t)(uint32_t)height *
                    sizeof(uint32_t);
  if (new_size > UINT32_MAX)
    return -1; /* VirtIO mem_entry.length is a 32-bit byte count. */
  size_t new_pages = (new_size + 4095u) / 4096u;
  void *new_backing = pmm_alloc_pages_dma(new_pages);
  if (!new_backing) {
    pr_err("VirtIO-GPU: set_mode %dx%d: out of memory\n", width, height);
    return -1;
  }
  memset(new_backing, 0, new_size);

  uint64_t flags;
  spin_lock_irqsave(&gpu_lock, &flags);

  /* Another caller may have completed this mode while the new backing was
   * being allocated outside the lock. */
  if (width == dev->width && height == dev->height) {
    spin_unlock_irqrestore(&gpu_lock, flags);
    pmm_free_pages(new_backing, new_pages);
    return 0;
  }

  uint32_t old_res = priv->resource_id;
  void *old_backing = priv->backing_store;
  size_t old_size = dev->framebuffer_size;
  uint32_t new_res = priv->next_resource_id++;

  if (vgpu_create_2d(priv, new_res, width, height) != 0) {
    spin_unlock_irqrestore(&gpu_lock, flags);
    pmm_free_pages(new_backing, new_pages);
    pr_err("VirtIO-GPU: set_mode %dx%d resource creation failed\n", width,
           height);
    return -1;
  }

  if (vgpu_attach_backing(priv, new_res, new_backing, (uint32_t)new_size) !=
          0 ||
      vgpu_set_scanout(priv, new_res, width, height) != 0) {
    /* The new resource exists.  Release it before freeing attached memory;
     * if the device cannot confirm unref, retain the backing rather than risk
     * DMA into freed pages. */
    int unref_result = vgpu_unref(priv, new_res);
    spin_unlock_irqrestore(&gpu_lock, flags);
    if (unref_result == 0)
      pmm_free_pages(new_backing, new_pages);
    else
      pr_err("VirtIO-GPU: set_mode rollback could not unref resource %u; "
             "backing retained for DMA safety\n",
             new_res);
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

  int old_unref_result = vgpu_unref(priv, old_res);
  spin_unlock_irqrestore(&gpu_lock, flags);

  if (old_backing) {
    if (old_unref_result == 0) {
      pmm_free_pages(old_backing, (old_size + 4095u) / 4096u);
    } else {
      /* The old resource may still DMA from its backing.  Retain it rather
       * than freeing pages whose ownership the device has not released. */
      pr_err("VirtIO-GPU: old resource %u unref failed; backing retained\n",
             old_res);
    }
  }

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

/* Caller must first stop new submissions and quiesce device DMA/IRQs.
 * A successful status reset is required before returning DMA-visible pages. */
static int vgpu_reset_device(struct virtio_gpu_state *priv) {
  if (!priv || !priv->handle)
    return 0;
  virtio_write_reg(priv->handle, VIRTIO_MMIO_STATUS, 0);
  for (uint32_t timeout = 0; timeout < 1000000u; timeout++) {
    if (virtio_read_reg(priv->handle, VIRTIO_MMIO_STATUS) == 0)
      return 0;
    hal_cpu_yield();
  }
  return -1;
}

static int vgpu_release_resources(struct gpu_device *dev,
                                  struct virtio_gpu_state *priv) {
  if (vgpu_reset_device(priv) != 0) {
    /* Do not free memory while the device may still DMA into it.  This is an
     * intentional quarantine/leak on reset failure, not a use-after-free. */
    pr_err("%s", "VirtIO-GPU: reset did not quiesce; DMA memory quarantined\n");
    return -1;
  }

  if (priv && priv->backing_store && dev && dev->framebuffer_size) {
    size_t pages = (dev->framebuffer_size + 4095u) / 4096u;
    pmm_free_pages(priv->backing_store, pages);
  }
  if (priv && priv->resp_buf)
    pmm_free_page(priv->resp_buf);
  if (priv && priv->cmd_buf)
    pmm_free_page(priv->cmd_buf);
  if (priv && priv->desc)
    pmm_free_pages(priv->desc, 2u);
  if (priv)
    kfree(priv);
  if (dev)
    kfree(dev);
  return 0;
}

static void vgpu_destroy(struct gpu_device *dev) {
  if (!dev)
    return;
  vgpu_release_resources(dev, (struct virtio_gpu_state *)dev->priv);
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
  if (!priv || !priv->handle || !priv->desc || !priv->avail || !priv->used ||
      !priv->cmd_buf || !priv->resp_buf || priv->qsize == 0 ||
      priv->qsize > UINT16_MAX)
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
  hal_cache_invalidate((void *)used, sizeof(*used));
  uint16_t old_idx = *idx_ptr;

  desc[1].addr = virt_to_phys(resp);
  desc[1].len = resp_len;
  desc[1].flags = VRING_DESC_F_WRITE;
  desc[1].next = 0;

  uint16_t ava_slot = (uint16_t)(avail->idx % (uint16_t)priv->qsize);
  avail->ring[ava_slot] = 0;
  avail->idx++;

  /* FIX(VGPU-DMA-02): the device DMA-reads 'cmd' (the caller-filled command
   * buffer), desc[] and avail — all plain cacheable RAM allocated by
   * pmm_alloc_pages_dma(), which cache-cleans them ONLY once, at
   * allocation.  Every write this function makes to desc[]/avail (and
   * every write the caller made to 'cmd' just before calling this) can sit
   * in this CPU's cache indefinitely: hal_mb()/mfence orders memory
   * operations already visible to the coherency system, it does NOT push
   * a dirty cache line out to physical RAM.  Under QEMU/KVM there is no
   * real cache the device could see stale data in (the "device" is
   * software touching the same emulated RAM array), so this was invisible
   * there; on real hardware, whether the device ever sees these writes
   * depends on unrelated platform cache/DMA-coherency behaviour.  Cleaning
   * cmd/desc/avail here, unconditionally, on every send, removes that
   * dependency instead of assuming it away. */
  hal_cache_clean(cmd, cmd_len);
  hal_cache_clean((void *)desc, 2 * sizeof(*desc));
  hal_cache_clean((void *)avail, sizeof(*avail));
  hal_mb();

  virtio_notify(priv->handle, 0);

  /* FIX(VGPU-POLL-01): this loop used to spin with no CPU-relax hint,
   * hammering the same cache line (*idx_ptr) as fast as the core can issue
   * loads. On real hardware this is a tight-loop bus/cache-coherency hog
   * that competes with the device's own DMA write to that same line —
   * never visible under QEMU/KVM's emulated memory, where there is no real
   * bus contention. hal_cpu_yield() (PAUSE/YIELD/WFE depending on arch) is a
   * no-op for correctness but lets the core back off between polls,
   * matching how every other busy-wait in this tree is written.
   *
   * FIX(VGPU-DMA-03): each iteration also invalidates 'used' before the
   * re-read.  Without this, once the CPU has cached *idx_ptr == old_idx
   * once, a plain reload can keep hitting that same cached line forever
   * even after the device's DMA write actually landed in RAM — the loop
   * would then spin until pure timeout every single time on any hardware
   * where the device write is not cache-coherent with this core, not just
   * intermittently. */
  uint64_t timeout = 200000000;
  while (timeout > 0) {
    hal_cache_invalidate((void *)used, sizeof(*used));
    if (*idx_ptr != old_idx)
      break;
    hal_cpu_yield();
    timeout--;
  }

  /* FIX(VGPU-POLL-02): VIRTIO_MMIO_INTERRUPT_ACK used to be written
   * unconditionally, even on timeout — acknowledging an interrupt that may
   * never have been asserted, and masking the fact that this command never
   * got a response. Only ACK (and only report success) once the used-ring
   * index has actually advanced; a timeout is reported as the failure it
   * is, with no ACK write, so a caller retrying (or the caller's caller,
   * e.g. compositor_init()'s single vgpu_create_2d/attach/set_scanout
   * chain) sees a real -1 instead of a silently swallowed timeout. */
  if (timeout == 0) {
    pr_err("%s", "VirtIO-GPU: Timeout waiting for device response!\n");
    return -1;
  }

  /* FIX(VGPU-DMA-04): invalidate 'resp' before the caller reads it — same
   * stale-cache hazard as 'used' above, for the response payload itself
   * (e.g. GET_DISPLAY_INFO's mode list, read by vgpu_query_display_info()
   * immediately after this call returns). */
  hal_cache_invalidate(resp, resp_len);

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
    vgpu_release_resources(dev, priv);
    return;
  }

  /* Control queue (queue 0) setup — per-device rings (#53). */
  virtio_write_reg(dev_handle, VIRTIO_MMIO_QUEUE_SEL, 0);
  uint32_t qmax = virtio_read_reg(dev_handle, VIRTIO_MMIO_QUEUE_NUM_MAX);
  if (qmax < 2) {
    pr_err("VirtIO-GPU: invalid control queue size %u\n", qmax);
    vgpu_release_resources(dev, priv);
    return;
  }
  priv->qsize = (qmax > 16) ? 16u : qmax;
  virtio_write_reg(dev_handle, VIRTIO_MMIO_QUEUE_NUM, priv->qsize);

  void *qmem = pmm_alloc_pages_dma(2u);
  if (!qmem) {
    pr_err("%s", "VirtIO-GPU: cannot allocate control queue memory\n");
    vgpu_release_resources(dev, priv);
    return;
  }
  memset(qmem, 0, 8192);
  priv->desc = (struct vring_desc *)qmem;
  priv->avail = (struct vring_avail *)((uint8_t *)qmem + priv->qsize * 16u);
  priv->used = (struct vring_used *)((uint8_t *)qmem + 4096u);

  virtio_setup_queue(dev_handle, 0, virt_to_phys(priv->desc),
                     virt_to_phys(priv->avail), virt_to_phys(priv->used));

  priv->cmd_buf = pmm_alloc_page_dma();
  priv->resp_buf = pmm_alloc_page_dma();
  if (!priv->cmd_buf || !priv->resp_buf) {
    pr_err("%s", "VirtIO-GPU: cannot allocate command/response buffers\n");
    vgpu_release_resources(dev, priv);
    return;
  }

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
  dev->framebuffer_size = (size_t)(uint32_t)qw * (size_t)(uint32_t)qh *
                          sizeof(uint32_t);

  size_t pages = (dev->framebuffer_size + 4095u) / 4096u;
  priv->backing_store = pmm_alloc_pages_dma(pages);
  if (!priv->backing_store) {
    pr_err("%s", "VirtIO-GPU: cannot allocate scanout backing store\n");
    vgpu_release_resources(dev, priv);
    return;
  }
  memset(priv->backing_store, 0, dev->framebuffer_size);
  /* panic_screen/graphics and the compositor all reach this backing store
   * through the ops->get_framebuffer contract (S-ALIGN F7) — there is no
   * second field-level path any more (the old dev->framebuffer_virt started
   * life unassigned and silently no-op'd the red panic screen, DIR-05 #139). */
  priv->resource_id = 1;
  priv->next_resource_id = 2;

  /* Create the scanout resource at the queried size.  Do not publish a
   * partially initialized device: every control command must succeed first. */
  if (vgpu_create_2d(priv, priv->resource_id, dev->width, dev->height) != 0 ||
      vgpu_attach_backing(priv, priv->resource_id, priv->backing_store,
                          (uint32_t)dev->framebuffer_size) != 0 ||
      vgpu_set_scanout(priv, priv->resource_id, dev->width, dev->height) != 0) {
    pr_err("%s", "VirtIO-GPU: scanout resource initialization failed\n");
    vgpu_release_resources(dev, priv);
    return;
  }

  if (gpu_register(dev) != 0) {
    pr_err("%s", "VirtIO-GPU: GPU registry rejected device\n");
    vgpu_release_resources(dev, priv);
    return;
  } /* publish last: flush/set_mode cannot race init (#53) */
}