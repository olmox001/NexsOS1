/*
 * kernel/drivers/virtio/virtio_input.c
 * VirtIO Input Device Driver (Keyboard/Mouse)
 * Full Virtqueue and Interrupt Implementation
 */
#include <kernel/irq.h>
#include <kernel/arch.h>
#include <drivers/virtio.h>
#include <drivers/virtio_input.h>
#include <kernel/graphics.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/types.h>

extern void compositor_update_mouse(int dx, int dy, int absolute);

#define MAX_INPUT_DEVS 2
#define INPUT_QSIZE 16

struct virtio_input_dev {
  uintptr_t base;
  uint32_t irq;
  int active;
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

/* Helper macros */
#define V_REG(base, off) (*(volatile uint32_t *)((base) + (off)))

/*
 * Add event to global buffer
 */
/*
 * Add event to global buffer
 */
static void virtio_input_add_event(uint16_t type, uint16_t code,
                                   int32_t value) {
  /* Debug buffer add */
  /* pr_info("Buffer Add: t=%d c=%d v=%d\n", type, code, value); */
  uint32_t next = (input_head + 1) % INPUT_BUFFER_SIZE;
  if (next == input_tail) {
    input_tail = (input_tail + 1) % INPUT_BUFFER_SIZE;
  }
  input_buffer[input_head].type = type;
  input_buffer[input_head].code = code;
  input_buffer[input_head].value = value;
  input_head = next;
}

/*
 * Initialize a single input device
 */
static void init_device(uintptr_t base, uint32_t irq) {
  if (input_dev_count >= MAX_INPUT_DEVS)
    return;

  struct virtio_input_dev *dev = &input_devs[input_dev_count++];
  dev->base = base;
  dev->irq = irq;
  dev->active = 0;

  /* Reset */
  V_REG(base, VIRTIO_MMIO_STATUS) = 0;
  V_REG(base, VIRTIO_MMIO_STATUS) =
      VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;

  /* Features */
  V_REG(base, VIRTIO_MMIO_DRIVER_FEATURES) = 0;
  V_REG(base, VIRTIO_MMIO_STATUS) |= VIRTIO_STATUS_FEATURES_OK;

  /* Setup Queue 0 (eventq) */
  V_REG(base, VIRTIO_MMIO_QUEUE_SEL) = 0;
  V_REG(base, VIRTIO_MMIO_QUEUE_NUM) = INPUT_QSIZE;

  /* Allocate queue memory (2 pages for descriptors/avail/used to satisfy
   * alignment) */
  /* Legacy requires Used ring to be on a 4KB boundary by default */
  void *qmem = pmm_alloc_pages(2);
  memset(qmem, 0, 8192);
  dev->desc = (struct vring_desc *)qmem;
  dev->avail = (struct vring_avail *)((uint8_t *)qmem + INPUT_QSIZE * 16);
  /* Used ring at offset 4096 (start of second page) */
  dev->used = (struct vring_used *)((uint8_t *)qmem + 4096);

  uint32_t version = V_REG(base, VIRTIO_MMIO_VERSION);
  pr_info("VirtIO-Input: Version %d\n", version);

  if (version >= 2) {
    /* Modern MMIO Setup */
    V_REG(base, VIRTIO_MMIO_QUEUE_SEL) = 0;

    uint64_t q_phys = (uint64_t)qmem;
    V_REG(base, VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint32_t)q_phys;
    V_REG(base, VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint32_t)(q_phys >> 32);

    uint64_t avail_phys = q_phys + INPUT_QSIZE * 16;
    V_REG(base, VIRTIO_MMIO_QUEUE_DRIVER_LOW) = (uint32_t)avail_phys;
    V_REG(base, VIRTIO_MMIO_QUEUE_DRIVER_HIGH) = (uint32_t)(avail_phys >> 32);

    uint64_t used_phys = q_phys + 4096;
    V_REG(base, VIRTIO_MMIO_QUEUE_DEVICE_LOW) = (uint32_t)used_phys;
    V_REG(base, VIRTIO_MMIO_QUEUE_DEVICE_HIGH) = (uint32_t)(used_phys >> 32);

    V_REG(base, VIRTIO_MMIO_QUEUE_READY) = 1;
  } else {
    /* Legacy MMIO Setup */
    V_REG(base, VIRTIO_MMIO_GUEST_PAGE_SIZE) = 4096;
    V_REG(base, VIRTIO_MMIO_QUEUE_PFN) = ((uint64_t)qmem) >> 12;
  }

  /* Allocate event buffers */
  dev->events = (struct virtio_input_event *)pmm_alloc_page();
  memset(dev->events, 0, sizeof(struct virtio_input_event) * INPUT_QSIZE);

  /* Fill descriptors with event buffers */
  for (int i = 0; i < INPUT_QSIZE; i++) {
    dev->desc[i].addr = (uint64_t)&dev->events[i];
    dev->desc[i].len = sizeof(struct virtio_input_event);
    dev->desc[i].flags = VRING_DESC_F_WRITE;
    dev->avail->ring[i] = i;
  }
  dev->avail->idx = INPUT_QSIZE;
  dev->last_used_idx = 0;

  /* Driver OK */
  V_REG(base, VIRTIO_MMIO_STATUS) |= VIRTIO_STATUS_DRIVER_OK;

  /* Register and Enable Interrupt */
  irq_register(irq, virtio_input_handler, dev);

  /* Notify device */
  /* Notify device */
  /* V1 legacy uses QUEUE_NOTIFY with queue index. V2 may also use it or
     doorbell. For V2, offset 0x50 is QueueNotify, so it's compatible. */
  V_REG(base, VIRTIO_MMIO_QUEUE_NOTIFY) = 0;

  dev->active = 1;
  pr_info("VirtIO-Input: Device at 0x%lx initialized, IRQ %u\n", base, irq);
}

/*
 * Interrupt Handler
 */
static void virtio_input_handler(uint32_t irq, void *data) {
  struct virtio_input_dev *dev = (struct virtio_input_dev *)data;
  (void)irq;

  uint32_t status = V_REG(dev->base, VIRTIO_MMIO_INTERRUPT_STATUS);
  V_REG(dev->base, VIRTIO_MMIO_INTERRUPT_ACK) = status;

  if (status == 0)
    return; /* Not for us or already handled */

  /* Restore light debug prints */
  /* pr_info("VirtIO-Input: IRQ status 0x%x\n", status); */

  int needs_render = 0;

  while (dev->last_used_idx != dev->used->idx) {
    /* Ensure we see the updated ring index */
    arch_data_barrier();

    struct vring_used_elem *e =
        &dev->used->ring[dev->last_used_idx % INPUT_QSIZE];
    uint32_t id = e->id;

    struct virtio_input_event *evt = &dev->events[id];

    /* Log event - useful for debugging keyboard/mouse issues */
    /* pr_info("Input: type=%d, code=%d, val=%d\n", evt->type, evt->code,
            evt->value); */

    if (evt->type == EV_REL) {
      if (evt->code == REL_X) {
        compositor_update_mouse(evt->value, 0, 0);
        needs_render = 1;
      } else if (evt->code == REL_Y) {
        compositor_update_mouse(0, evt->value, 0);
        needs_render = 1;
      }
    } else if (evt->type == EV_ABS) {
      if (evt->code == 0) { /* ABS_X */
        compositor_update_mouse(evt->value, -1, 1);
        needs_render = 1;
      } else if (evt->code == 1) { /* ABS_Y */
        compositor_update_mouse(-1, evt->value, 1);
        needs_render = 1;
      }
    } else if (evt->type == EV_KEY) {
      if (evt->code == 272) { /* BTN_LEFT */
        compositor_handle_click(evt->code, evt->value);
        needs_render = 1; /* Click might change z-order/focus */
      } else {
        virtio_input_add_event(evt->type, evt->code, evt->value);
        /* Notify upper layer (Keyboard) */
        extern void keyboard_notify_input(void);
        keyboard_notify_input();
      }
    }

    /* Return descriptor to available ring */
    dev->avail->ring[dev->avail->idx % INPUT_QSIZE] = id;
    arch_data_barrier();
    dev->avail->idx++;
    dev->last_used_idx++;
  }

  /* Notify device of new available buffers */
  V_REG(dev->base, VIRTIO_MMIO_QUEUE_NOTIFY) = 0;
  /* For V2, using doorbell might be needed but V1 works for now since we
   * processed Used */

  /* Batch render: Only draw once per interrupt if needed */
  if (needs_render) {
    compositor_render();
  }
}

/*
 * Global Initialization
 */
void virtio_input_init(void) {
  pr_info("%s", "VirtIO-Input: Probing devices...\n");

  /* Probe all MMIO slots for VirtIO Input devices (Keyboard/Mouse) */
  for (uintptr_t addr = 0x0a000000; addr <= 0x0a003e00; addr += 0x200) {
    uint32_t magic = V_REG(addr, 0);
    uint32_t devid = V_REG(addr, 8);
    if (magic == 0x74726976 && devid == 18) {
      uint32_t slot = (addr - 0x0a000000) / 0x200;
      init_device(addr, 48 + slot);
    }
  }
}

/*
 * Poll interface (backward compatibility)
 */
int virtio_input_poll(struct virtio_input_event *event) {
  if (input_head == input_tail)
    return 0;

  *event = input_buffer[input_tail];
  input_tail = (input_tail + 1) % INPUT_BUFFER_SIZE;
  return 1;
}

int virtio_input_has_event(void) { return input_head != input_tail; }
