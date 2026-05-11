/*
 * kernel/drivers/gpu/virtio_gpu.c
 * VirtIO GPU Driver Implementation (HAL Compliant)
 */
#include <drivers/gpu/gpu.h>
#include <drivers/virtio.h>
#include <drivers/virtio_gpu.h>
#include <kernel/arch.h>
#include <kernel/kmalloc.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

/* Vring structures for Control Queue (Queue 0) */
static struct vring_desc *desc;
static struct vring_avail *avail;
static struct vring_used *used;

struct virtio_gpu_state {
  uint64_t base;
  uint32_t qsize;
  void *backing_store;
  uint32_t resource_id;
  struct gpu_device *dev;
};

static void *gpu_cmd_buf = NULL;
static void *gpu_resp_buf = NULL;
static DEFINE_SPINLOCK(gpu_lock);

extern uint64_t *kernel_pgd;

static int virtio_gpu_send(struct virtio_gpu_state *priv, void *cmd,
                           uint32_t cmd_len, void *resp, uint32_t resp_len);

static int vgpu_flush(struct gpu_device *dev, int x, int y, int w, int h) {
  if (!dev || !dev->priv)
    return -1;
  struct virtio_gpu_state *priv = (struct virtio_gpu_state *)dev->priv;

  uint64_t flags;
  spin_lock_irqsave(&gpu_lock, &flags);

  /* 1. Transfer guest memory to host resource */
  struct virtio_gpu_transfer_to_host_2d *cmd_xfer =
      (struct virtio_gpu_transfer_to_host_2d *)gpu_cmd_buf;
  struct virtio_gpu_ctrl_hdr *resp = (struct virtio_gpu_ctrl_hdr *)gpu_resp_buf;

  cmd_xfer->hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
  cmd_xfer->r.x = x;
  cmd_xfer->r.y = y;
  cmd_xfer->r.width = w;
  cmd_xfer->r.height = h;
  cmd_xfer->offset = (y * dev->width + x) * 4;
  cmd_xfer->resource_id = priv->resource_id;

  virtio_gpu_send(priv, cmd_xfer, sizeof(*cmd_xfer), resp, sizeof(*resp));

  /* 2. Flush host resource to display */
  struct virtio_gpu_resource_flush *cmd =
      (struct virtio_gpu_resource_flush *)gpu_cmd_buf;

  cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
  cmd->r.x = x;
  cmd->r.y = y;
  cmd->r.width = w;
  cmd->r.height = h;
  cmd->resource_id = priv->resource_id;

  virtio_gpu_send(priv, cmd, sizeof(*cmd), resp, sizeof(*resp));

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

static int vgpu_set_mode(struct gpu_device *dev, int width, int height) {
  (void)dev;
  (void)width;
  (void)height;
  return 0;
}

static void vgpu_destroy(struct gpu_device *dev) {
  kfree(dev->priv);
  kfree(dev);
}

static struct gpu_ops vgpu_ops = {
    .flush = vgpu_flush,
    .get_framebuffer = vgpu_get_framebuffer,
    .set_mode = vgpu_set_mode,
    .destroy = vgpu_destroy,
};

static int virtio_gpu_send(struct virtio_gpu_state *priv, void *cmd,
                           uint32_t cmd_len, void *resp, uint32_t resp_len) {
  if (!priv->base)
    return -1;

  desc[0].addr = (uint64_t)cmd;
  desc[0].len = cmd_len;
  desc[0].flags = VRING_DESC_F_NEXT;
  desc[0].next = 1;

  volatile uint16_t *idx_ptr = &used->idx;
  uint16_t old_idx = *idx_ptr;

  desc[1].addr = (uint64_t)resp;
  desc[1].len = resp_len;
  desc[1].flags = VRING_DESC_F_WRITE;
  desc[1].next = 0;

  uint16_t idx = avail->idx % priv->qsize;
  avail->ring[idx] = 0;

  arch_data_barrier();
  avail->idx++;
  arch_data_barrier();

  virtio_notify(priv->base, 0);

  uint64_t timeout = 100000000;
  while (*idx_ptr == old_idx && timeout > 0) {
    arch_yield();
    timeout--;
  }

  if (timeout == 0) {
    pr_err("%s", "VirtIO-GPU: Timeout!\n");
    return -1;
  }
  return 0;
}

void virtio_gpu_init(void) {
  pr_info("%s", "VirtIO-GPU: Probing...\n");

  uintptr_t base = 0;
  uint32_t irq = 0;

  if (arch_virtio_probe(VIRTIO_DEV_GPU, &base, &irq) == 0) {
    pr_info("VirtIO-GPU: Found at 0x%016lx (IRQ %u)\n", base, irq);

    /* Map MMIO if it's a Modern (MMIO) device */
    if (base & (1ULL << 62)) {
      uintptr_t phys = base & ~(1ULL << 62);
      extern uint64_t *kernel_pgd;
      for (int i = 0; i < 4; i++) {
        vmm_map_page(kernel_pgd, phys + i * 4096, phys + i * 4096, PAGE_DEVICE);
      }
      pr_info("VirtIO-GPU: MMIO mapped at 0x%lx\n", phys);
    }

    struct gpu_device *dev = kmalloc(sizeof(struct gpu_device));
    struct virtio_gpu_state *priv = kmalloc(sizeof(struct virtio_gpu_state));
    memset(dev, 0, sizeof(*dev));
    memset(priv, 0, sizeof(*priv));

    priv->base = base;
    priv->dev = dev;
    dev->priv = priv;
    dev->ops = &vgpu_ops;

    strcpy(dev->name, "VirtIO-GPU");

    /* Reset device */
    virtio_write_reg(base, VIRTIO_MMIO_STATUS, 0);

    /* Acknowledge + Driver */
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    virtio_write_reg(base, VIRTIO_MMIO_STATUS, status);

    /* Feature negotiation */
    uint32_t features = virtio_read_reg(base, VIRTIO_MMIO_DEVICE_FEATURES);
    virtio_write_reg(base, VIRTIO_MMIO_DRIVER_FEATURES, features);

    status |= VIRTIO_STATUS_FEATURES_OK;
    virtio_write_reg(base, VIRTIO_MMIO_STATUS, status);

    if (!(virtio_read_reg(base, VIRTIO_MMIO_STATUS) &
          VIRTIO_STATUS_FEATURES_OK)) {
      pr_err("%s", "VirtIO-GPU: Negotiation failed\n");
      kfree(dev);
      kfree(priv);
      return;
    }

    /* Queue 0 setup */
    virtio_write_reg(base, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = virtio_read_reg(base, VIRTIO_MMIO_QUEUE_NUM_MAX);
    priv->qsize = (qmax > 16) ? 16 : qmax;
    virtio_write_reg(base, VIRTIO_MMIO_QUEUE_NUM, priv->qsize);
    virtio_write_reg(base, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);

    void *qmem = pmm_alloc_pages(2);
    memset(qmem, 0, 8192);
    desc = (struct vring_desc *)qmem;
    avail = (struct vring_avail *)((uint8_t *)qmem + priv->qsize * 16);
    used = (struct vring_used *)((uint8_t *)qmem + 4096);

    vmm_map_page(kernel_pgd, (uint64_t)qmem, (uint64_t)qmem, PAGE_DEVICE);
    vmm_map_page(kernel_pgd, (uint64_t)qmem + 4096, (uint64_t)qmem + 4096,
                 PAGE_DEVICE);
    virtio_write_reg(base, VIRTIO_MMIO_QUEUE_PFN, (uint64_t)qmem >> 12);

    if (!gpu_cmd_buf)
      gpu_cmd_buf = pmm_alloc_page();
    if (!gpu_resp_buf)
      gpu_resp_buf = pmm_alloc_page();

    vmm_map_page(kernel_pgd, (uint64_t)gpu_cmd_buf, (uint64_t)gpu_cmd_buf,
                 PAGE_DEVICE);
    vmm_map_page(kernel_pgd, (uint64_t)gpu_resp_buf, (uint64_t)gpu_resp_buf,
                 PAGE_DEVICE);

    /* Driver OK */
    virtio_write_reg(base, VIRTIO_MMIO_STATUS,
                     VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                         VIRTIO_STATUS_DRIVER_OK);

    dev->width = 720;
    dev->height = 1280;
    dev->bpp = 32;
    dev->framebuffer_size = 720 * 1280 * 4;

    int pages = (dev->framebuffer_size + 4095) / 4096;
    priv->backing_store = pmm_alloc_pages(pages);
    memset(priv->backing_store, 0, dev->framebuffer_size);

    priv->resource_id = 1;

    void *cmd_page = pmm_alloc_page();
    void *resp_page = pmm_alloc_page();
    memset(cmd_page, 0, 4096);
    memset(resp_page, 0, 4096);

    struct virtio_gpu_resource_create_2d *create_cmd =
        (struct virtio_gpu_resource_create_2d *)cmd_page;
    create_cmd->hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create_cmd->resource_id = priv->resource_id;
    create_cmd->format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    create_cmd->width = dev->width;
    create_cmd->height = dev->height;
    virtio_gpu_send(priv, create_cmd, sizeof(*create_cmd), resp_page,
                    sizeof(struct virtio_gpu_ctrl_hdr));

    memset(cmd_page, 0, 4096);
    struct virtio_gpu_resource_attach_backing *attach =
        (struct virtio_gpu_resource_attach_backing *)cmd_page;
    struct virtio_gpu_mem_entry *ents =
        (struct virtio_gpu_mem_entry *)((uint8_t *)cmd_page + sizeof(*attach));
    attach->hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach->resource_id = priv->resource_id;
    attach->nr_entries = 1;
    ents[0].addr = (uint64_t)priv->backing_store;
    ents[0].length = dev->framebuffer_size;
    virtio_gpu_send(priv, attach, sizeof(*attach) + sizeof(*ents), resp_page,
                    sizeof(struct virtio_gpu_ctrl_hdr));

    memset(cmd_page, 0, 4096);
    struct virtio_gpu_set_scanout *scanout =
        (struct virtio_gpu_set_scanout *)cmd_page;
    scanout->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout->resource_id = priv->resource_id;
    scanout->r.width = dev->width;
    scanout->r.height = dev->height;
    virtio_gpu_send(priv, scanout, sizeof(*scanout), resp_page,
                    sizeof(struct virtio_gpu_ctrl_hdr));

    for (int p = 0; p < pages; p++) {
      uint64_t vaddr = (uint64_t)priv->backing_store + p * 4096;
      vmm_map_page(kernel_pgd, vaddr, vaddr, PAGE_DEVICE);
    }

    pmm_free_page(cmd_page);
    pmm_free_page(resp_page);

    gpu_register(dev);
  } else {
    pr_info("%s", "VirtIO-GPU: Not found\n");
  }
}
