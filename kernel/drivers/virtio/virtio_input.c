/*
 * kernel/drivers/virtio/virtio_input.c
 * VirtIO Input Device Driver (Keyboard/Mouse)
 * Full Virtqueue and Interrupt Implementation
 */
#include <drivers/input.h>
#include <drivers/virtio.h>
#include <drivers/virtio_input.h>
#include <kernel/arch.h>
#include <kernel/irq.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>


#define MAX_INPUT_DEVS 2
#define INPUT_QSIZE 16

struct virtio_input_dev {
  virtio_handle_t handle;
  uint32_t irq;
  int active;
  int irq_handler_registered;
  int is_pci;
  struct vring_desc *desc;
  struct vring_avail *avail;
  struct vring_used *used;
  uint16_t last_used_idx;
  struct virtio_input_event *events;
};

static struct virtio_input_dev input_devs[MAX_INPUT_DEVS];
static int input_dev_count = 0;

/* Forward declarations */
static void virtio_input_handler(uint32_t irq, void *data);

/* One handler services every VirtIO input device sharing its IRQ.  The
 * handler deliberately scans input_devs[] and filters by irq, so registering
 * it again for a second device on the same line is both unnecessary and
 * rejected by irq_register() as -EBUSY. */
static int virtio_input_irq_has_handler(uint32_t irq) {
  for (int i = 0; i < input_dev_count; i++) {
    if (input_devs[i].irq == irq && input_devs[i].irq_handler_registered)
      return 1;
  }
  return 0;
}

/* Helper macros / functions for transport abstraction */
#define v_read32(dev, off) virtio_read_reg((dev)->handle, (off))
#define v_write32(dev, off, val) virtio_write_reg((dev)->handle, (off), (val))
#define v_notify(dev, q) virtio_notify((dev)->handle, (q))

static void init_device(virtio_handle_t handle, uint32_t irq, int is_pci) {
  (void)is_pci;
  if (input_dev_count >= MAX_INPUT_DEVS)
    return;

  struct virtio_input_dev *dev = &input_devs[input_dev_count++];
  dev->handle = handle;
  dev->irq = irq;
  dev->active = 0;
  dev->irq_handler_registered = 0;

  /* Reset */
  v_write32(dev, VIRTIO_MMIO_STATUS, 0);
  v_write32(dev, VIRTIO_MMIO_STATUS,
            VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

  /* Features */
  v_write32(dev, VIRTIO_MMIO_DRIVER_FEATURES, 0);
  v_write32(dev, VIRTIO_MMIO_STATUS,
            v_read32(dev, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FEATURES_OK);

  /* Setup Queue 0 (eventq) */
  v_write32(dev, VIRTIO_MMIO_QUEUE_SEL, 0);
  v_write32(dev, VIRTIO_MMIO_QUEUE_NUM, INPUT_QSIZE);

  void *qmem = pmm_alloc_pages_dma(2);
  if (!qmem) {
    /* DRV-VIRTIO-07: bail out gracefully instead of NULL-dereferencing. */
    pr_err("%s", "VirtIO-Input: failed to allocate virtqueue memory\n");
    input_dev_count--;
    return;
  }
  memset(qmem, 0, 8192);
  dev->desc = (struct vring_desc *)qmem;
  dev->avail = (struct vring_avail *)((uint8_t *)qmem + INPUT_QSIZE * 16);
  dev->used = (struct vring_used *)((uint8_t *)qmem + 4096);

  /* Use unified HAL API; the device needs PHYSICAL ring addresses. */
  virtio_setup_queue(handle, 0, virt_to_phys(dev->desc),
                     virt_to_phys(dev->avail), virt_to_phys(dev->used));

  dev->events = (struct virtio_input_event *)pmm_alloc_page_dma();
  if (!dev->events) {
    /* DRV-VIRTIO-07: free the virtqueue memory and bail out gracefully. */
    pr_err("%s", "VirtIO-Input: failed to allocate events buffer\n");
    pmm_free_pages(qmem, 2);
    input_dev_count--;
    return;
  }
  memset(dev->events, 0, sizeof(struct virtio_input_event) * INPUT_QSIZE);

  for (int i = 0; i < INPUT_QSIZE; i++) {
    /* Descriptor addresses are PHYSICAL (DMA). */
    dev->desc[i].addr = virt_to_phys(&dev->events[i]);
    dev->desc[i].len = sizeof(struct virtio_input_event);
    dev->desc[i].flags = VRING_DESC_F_WRITE;
    dev->avail->ring[i] = i;
  }
  dev->avail->idx = INPUT_QSIZE;
  dev->last_used_idx = 0;

  /* FIX(VINPUT-DMA-04): the device DMA-reads desc[]/avail as soon as
   * v_notify() below runs; pmm_alloc_pages_dma() only cache-cleaned the
   * freshly-zeroed page once, at allocation, not the descriptor table and
   * available-ring contents just written above. Same hazard class as
   * VGPU-DMA-02/VBLK-DMA-02: without this, whether the device's very
   * first read of this queue sees valid descriptors depends on unrelated
   * cache state on real hardware, and is always fine under QEMU/KVM. */
  hal_cache_clean((void *)dev->desc, INPUT_QSIZE * sizeof(*dev->desc));
  hal_cache_clean((void *)dev->avail, sizeof(*dev->avail));
  hal_mb();

  /* Driver OK */
  v_write32(dev, VIRTIO_MMIO_STATUS,
            v_read32(dev, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER_OK);

  /* Register one handler per IRQ line.  virtio_input_handler() scans all
   * active input devices on the acknowledged line, so keyboard and mouse may
   * safely share one PCI INTx/GIC IRQ without a second irq_register() call. */
  if (!virtio_input_irq_has_handler(irq)) {
    if (irq_register(irq, virtio_input_handler, NULL) != 0) {
      pr_err("virtio-input: cannot register IRQ %u; disabling provider\n",
             irq);
      v_write32(dev, VIRTIO_MMIO_STATUS, 0);
      pmm_free_page(dev->events);
      pmm_free_pages(dev->desc, 2);
      dev->events = NULL;
      dev->desc = NULL;
      dev->avail = NULL;
      dev->used = NULL;
      input_dev_count--;
      return;
    }
    dev->irq_handler_registered = 1;
  } else {
    pr_info("VirtIO-Input: sharing IRQ %u with existing input handler\n",
            irq);
  }

  /* Publish the device before notifying it: an immediate interrupt must see
   * active=1 when the shared handler scans input_devs[]. */
  dev->active = 1;
  v_notify(dev, 0);
}

static void virtio_input_handler(uint32_t irq, void *data) {
  (void)data; /* Parametro ignorato a favore del polling vettoriale */

  for (int i = 0; i < input_dev_count; i++) {
    struct virtio_input_dev *dev = &input_devs[i];

    /* Filtra i dispositivi disattivi o non pertinenti all'IRQ sollevato */
    if (!dev->active || dev->irq != irq) {
      continue;
    }

    uint32_t status = v_read32(dev, VIRTIO_MMIO_INTERRUPT_STATUS);
    uint16_t processed_count = 0;

    /* FIX(VINPUT-DMA-01): the device writes dev->used (idx + ring entries)
     * via DMA every time it reports an event; this CPU's cache can hold a
     * stale copy of that line from before the event arrived. Without
     * invalidating it here, this IRQ handler can read a used->idx this
     * core already had cached — the same value as last time — and see NO
     * new event at all, even though the device already wrote one to RAM.
     * That is a hardware-timing-dependent "dropped IRQ" that depends on
     * unrelated cache state, never reproducible under QEMU/KVM's emulated
     * (always-coherent) memory: exactly the "works only after moving the
     * mouse enough times" symptom this device's IRQ was implicated in. */
    hal_cache_invalidate((void *)dev->used, sizeof(*dev->used));

    while (dev->last_used_idx != dev->used->idx) {
      struct vring_used_elem *e =
          &dev->used->ring[dev->last_used_idx % INPUT_QSIZE];
      uint32_t id = e->id;

      /* FIX(VINPUT-DMA-02): dev->events[id] is the DMA target the device
       * just wrote the actual event payload (type/code/value) into — a
       * separate cache line from dev->used, and just as capable of being
       * stale in this CPU's cache. */
      hal_cache_invalidate((void *)&dev->events[id],
                                  sizeof(dev->events[id]));
      struct virtio_input_event *evt = &dev->events[id];

      /* Single dispatch point: keys -> keyboard/IPC, pointer -> compositor,
       * EV_SYN -> repaint. Same path PS/2 and USB HID use now. */
      OS1driver_input_publish(INPUT_PROVIDER_VIRTIO,
                               (uint8_t)(INPUT_DEVICE_VIRTIO_BASE + i),
                               evt->type, evt->code, evt->value);

      dev->avail->ring[dev->avail->idx % INPUT_QSIZE] = id;
      dev->avail->idx++;
      dev->last_used_idx++;
      processed_count++;
    }

    /* FIX(VINPUT-DMA-03): the device DMA-reads dev->avail to learn which
     * descriptor slots are free again (recycled just above); clean it so
     * those writes are actually visible to the device's next DMA read,
     * for the same reason as virtio_gpu.c's virtio_gpu_send()
     * (VGPU-DMA-02) — hal_mb() alone does not push a dirty cache line
     * out to physical RAM. Done once per IRQ, after the recycle loop,
     * instead of per-iteration: avail is only consumed by the device on
     * the v_notify() below, not before. */
    if (processed_count > 0) {
      hal_cache_clean((void *)dev->avail, sizeof(*dev->avail));
      hal_mb();
    }

    if (status != 0) {
      v_write32(dev, VIRTIO_MMIO_INTERRUPT_ACK, status);
    } else if (processed_count == 0) {
      v_read32(dev, VIRTIO_MMIO_INTERRUPT_ACK);
    }

    if (processed_count > 0) {
      v_notify(dev, 0);
    }
  }
}

void virtio_input_init(void) {
  pr_info("%s", "VirtIO-Input: Probing devices...\n");
  input_dev_count = 0; // reset

  int count = arch_virtio_get_count(VIRTIO_DEV_INPUT);
  for (int i = 0; i < count; i++) {
    virtio_handle_t dev = NULL;
    uint32_t irq = 0;
    if (arch_virtio_get_device(VIRTIO_DEV_INPUT, i, &dev, &irq) == 0) {
      init_device(dev, irq, 0);
    }
  }

  if (input_dev_count > 0) {
    pr_info("VirtIO-Input: %d input device(s) initialized\n", input_dev_count);
  }
}
int virtio_input_get_dev_count(void) { return input_dev_count; }

