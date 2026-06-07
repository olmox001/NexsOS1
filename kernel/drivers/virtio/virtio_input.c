/*
 * kernel/drivers/virtio/virtio_input.c
 * VirtIO Input Device Driver (Keyboard/Mouse)
 * Full Virtqueue and Interrupt Implementation
 */
#include <drivers/virtio.h>
#include <drivers/virtio_input.h>
#include <kernel/arch.h>
#include <kernel/graphics.h>
#include <kernel/irq.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>

#ifdef ARCH_AMD64
#include <drivers/pci.h>
#endif

extern void compositor_update_mouse(int dx, int dy, int absolute);

#define MAX_INPUT_DEVS 2
#define INPUT_QSIZE 16

struct virtio_input_dev {
  virtio_handle_t handle;
  uint32_t irq;
  int active;
  int is_pci;
  struct vring_desc *desc;
  struct vring_avail *avail;
  struct vring_used *used;
  uint16_t last_used_idx;
  struct virtio_input_event *events;
};

static struct virtio_input_dev input_devs[MAX_INPUT_DEVS];
static int input_dev_count = 0;

/* Global Input Buffer (Shared by all devices) */
#define INPUT_BUFFER_SIZE 256
static struct virtio_input_event input_buffer[INPUT_BUFFER_SIZE];
static volatile uint32_t input_head = 0;
static volatile uint32_t input_tail = 0;

/* Forward declarations */
static void virtio_input_handler(uint32_t irq, void *data);

/* Helper macros / functions for transport abstraction */
#define v_read32(dev, off) virtio_read_reg((dev)->handle, (off))
#define v_write32(dev, off, val) virtio_write_reg((dev)->handle, (off), (val))
#define v_notify(dev, q) virtio_notify((dev)->handle, (q))

static void virtio_input_add_event(uint16_t type, uint16_t code,
                                   int32_t value) {
  uint32_t next = (input_head + 1) % INPUT_BUFFER_SIZE;
  if (next == input_tail) {
    input_tail = (input_tail + 1) % INPUT_BUFFER_SIZE;
  }
  input_buffer[input_head].type = type;
  input_buffer[input_head].code = code;
  input_buffer[input_head].value = value;
  input_head = next;
}

static void init_device(virtio_handle_t handle, uint32_t irq, int is_pci) {
  (void)is_pci;
  if (input_dev_count >= MAX_INPUT_DEVS)
    return;

  struct virtio_input_dev *dev = &input_devs[input_dev_count++];
  dev->handle = handle;
  dev->irq = irq;
  dev->active = 0;

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

  void *qmem = pmm_alloc_pages(2);
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

  /* Rings are already identity mapped */

  /* Use unified HAL API */
  virtio_setup_queue(handle, 0, (uint64_t)dev->desc, (uint64_t)dev->avail,
                     (uint64_t)dev->used);

  dev->events = (struct virtio_input_event *)pmm_alloc_page();
  if (!dev->events) {
    /* DRV-VIRTIO-07: free the virtqueue memory and bail out gracefully. */
    pr_err("%s", "VirtIO-Input: failed to allocate events buffer\n");
    pmm_free_pages(qmem, 2);
    input_dev_count--;
    return;
  }
  memset(dev->events, 0, sizeof(struct virtio_input_event) * INPUT_QSIZE);
  /* Events buffer is identity mapped */

  for (int i = 0; i < INPUT_QSIZE; i++) {
    dev->desc[i].addr = (uint64_t)&dev->events[i];
    dev->desc[i].len = sizeof(struct virtio_input_event);
    dev->desc[i].flags = VRING_DESC_F_WRITE;
    dev->avail->ring[i] = i;
  }
  dev->avail->idx = INPUT_QSIZE;
  dev->last_used_idx = 0;

  /* Driver OK */
  v_write32(dev, VIRTIO_MMIO_STATUS,
            v_read32(dev, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_DRIVER_OK);

  /* Register and Enable Interrupt */
  irq_register(irq, virtio_input_handler, dev);

  /* Notify device */
  v_notify(dev, 0);

  dev->active = 1;
}

static void virtio_input_handler(uint32_t irq, void *data) {
  (void)data; /* Parametro ignorato a favore del polling vettoriale */
  int needs_render = 0;

  for (int i = 0; i < input_dev_count; i++) {
    struct virtio_input_dev *dev = &input_devs[i];

    /* Filtra i dispositivi disattivi o non pertinenti all'IRQ sollevato */
    if (!dev->active || dev->irq != irq) {
      continue;
    }

    uint32_t status = v_read32(dev, VIRTIO_MMIO_INTERRUPT_STATUS);
    uint16_t processed_count = 0;

    while (dev->last_used_idx != dev->used->idx) {
      struct vring_used_elem *e =
          &dev->used->ring[dev->last_used_idx % INPUT_QSIZE];
      uint32_t id = e->id;
      struct virtio_input_event *evt = &dev->events[id];

      if (evt->type == EV_REL) {
        if (evt->code == REL_X) {
          compositor_update_mouse(evt->value, 0, 0);
          needs_render = 1;
        } else if (evt->code == REL_Y) {
          compositor_update_mouse(0, evt->value, 0);
          needs_render = 1;
        }
      } else if (evt->type == EV_ABS) {
        if (evt->code == 0) {
          compositor_update_mouse(evt->value, -1, 1);
          needs_render = 1;
        } else if (evt->code == 1) {
          compositor_update_mouse(-1, evt->value, 1);
          needs_render = 1;
        }
      } else if (evt->type == EV_KEY) {
        if (evt->code == 272) {
          compositor_handle_click(evt->code, evt->value);
          needs_render = 1;
        } else {
          virtio_input_add_event(evt->type, evt->code, evt->value);
          extern void keyboard_notify_input(void);
          keyboard_notify_input();
        }
      }

      dev->avail->ring[dev->avail->idx % INPUT_QSIZE] = id;
      arch_mb();
      dev->avail->idx++;
      dev->last_used_idx++;
      processed_count++;
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

  if (needs_render) {
    compositor_render();
  }
}

void virtio_input_init(void) {
  pr_info("%s", "VirtIO-Input: Probing devices...\n");

  int count = arch_virtio_get_count(VIRTIO_DEV_INPUT);
  for (int i = 0; i < count; i++) {
    virtio_handle_t dev = NULL;
    uint32_t irq = 0;
    if (arch_virtio_get_device(VIRTIO_DEV_INPUT, i, &dev, &irq) == 0) {
      init_device(dev, irq, 0);
    }
  }
}

int virtio_input_poll(struct virtio_input_event *event) {
  if (input_head == input_tail)
    return 0;
  *event = input_buffer[input_tail];
  input_tail = (input_tail + 1) % INPUT_BUFFER_SIZE;
  return 1;
}

int virtio_input_has_event(void) { return input_head != input_tail; }
