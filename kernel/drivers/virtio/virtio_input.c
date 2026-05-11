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

#ifdef ARCH_AMD64
#include <drivers/pci.h>
#endif

extern void compositor_update_mouse(int dx, int dy, int absolute);

#define MAX_INPUT_DEVS 2
#define INPUT_QSIZE 16

struct virtio_input_dev {
  uintptr_t base;
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
static inline uint32_t v_read32(struct virtio_input_dev *dev, uint32_t off) {
#ifdef ARCH_AMD64
  if (dev->is_pci) {
    /* Legacy PCI I/O mapping */
    uint16_t port = (uint16_t)dev->base;
    switch (off) {
    case VIRTIO_MMIO_MAGIC_VALUE: return 0x74726976;
    case VIRTIO_MMIO_VERSION:     return 1;
    case VIRTIO_MMIO_DEVICE_ID:   return 18;
    case VIRTIO_MMIO_STATUS:      return inb(port + 0x12);
    case VIRTIO_MMIO_INTERRUPT_STATUS: return inb(port + 0x13);
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
      outw(port + 0x0E, 0); // Select queue 0
      return inw(port + 0x0C);
    }
    return 0;
  }
#endif
  return (*(volatile uint32_t *)((dev->base) + (off)));
}

static inline void v_write32(struct virtio_input_dev *dev, uint32_t off,
                             uint32_t val) {
#ifdef ARCH_AMD64
  if (dev->is_pci) {
    uint16_t port = (uint16_t)dev->base;
    switch (off) {
    case VIRTIO_MMIO_STATUS:
      outb(port + 0x12, (uint8_t)val);
      break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
      outl(port + 0x04, val);
      break;
    case VIRTIO_MMIO_QUEUE_SEL:
      outw(port + 0x0E, (uint16_t)val);
      break;
    case VIRTIO_MMIO_QUEUE_NUM:
      outw(port + 0x0C, (uint16_t)val);
      break;
    case VIRTIO_MMIO_QUEUE_PFN:
      outl(port + 0x08, val);
      break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
      outw(port + 0x10, (uint16_t)val);
      break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
      /* Legacy PCI ACK is implicit on reading ISR status, 
         but we can write to it if needed. */
      break;
    }
    return;
  }
#endif
  (*(volatile uint32_t *)((dev->base) + (off)) = (val));
}

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

static void init_device(uintptr_t base, uint32_t irq, int is_pci) {
  if (input_dev_count >= MAX_INPUT_DEVS)
    return;

  struct virtio_input_dev *dev = &input_devs[input_dev_count++];
  dev->base = base;
  dev->irq = irq;
  dev->is_pci = is_pci;
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
  memset(qmem, 0, 8192);
  dev->desc = (struct vring_desc *)qmem;
  dev->avail = (struct vring_avail *)((uint8_t *)qmem + INPUT_QSIZE * 16);
  dev->used = (struct vring_used *)((uint8_t *)qmem + 4096);

  uint32_t version = v_read32(dev, VIRTIO_MMIO_VERSION);
  pr_info("VirtIO-Input: Version %d (%s)\n", version, is_pci ? "PCI" : "MMIO");

  if (!is_pci && version >= 2) {
    /* Modern MMIO Setup (AArch64 only for now) */
    uint64_t q_phys = (uint64_t)qmem;
    v_write32(dev, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)q_phys);
    v_write32(dev, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(q_phys >> 32));

    uint64_t avail_phys = q_phys + INPUT_QSIZE * 16;
    v_write32(dev, VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32_t)avail_phys);
    v_write32(dev, VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uint32_t)(avail_phys >> 32));

    uint64_t used_phys = q_phys + 4096;
    v_write32(dev, VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32_t)used_phys);
    v_write32(dev, VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uint32_t)(used_phys >> 32));

    v_write32(dev, VIRTIO_MMIO_QUEUE_READY, 1);
  } else {
    /* Legacy MMIO or Legacy PCI Setup */
    if (!is_pci) {
        v_write32(dev, VIRTIO_MMIO_GUEST_PAGE_SIZE, 4096);
    }
    v_write32(dev, VIRTIO_MMIO_QUEUE_PFN, ((uint64_t)qmem) >> 12);
  }

  dev->events = (struct virtio_input_event *)pmm_alloc_page();
  memset(dev->events, 0, sizeof(struct virtio_input_event) * INPUT_QSIZE);

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
  v_write32(dev, VIRTIO_MMIO_QUEUE_NOTIFY, 0);

  dev->active = 1;
  pr_info("VirtIO-Input: Device at 0x%lx initialized, IRQ %u\n", base, irq);
}

static void virtio_input_handler(uint32_t irq, void *data) {
  struct virtio_input_dev *dev = (struct virtio_input_dev *)data;
  (void)irq;

  uint32_t status = v_read32(dev, VIRTIO_MMIO_INTERRUPT_STATUS);
  v_write32(dev, VIRTIO_MMIO_INTERRUPT_ACK, status);

  if (status == 0)
    return;

  int needs_render = 0;
  while (dev->last_used_idx != dev->used->idx) {
    arch_data_barrier();
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
        needs_render = 1;
      } else {
        virtio_input_add_event(evt->type, evt->code, evt->value);
        extern void keyboard_notify_input(void);
        keyboard_notify_input();
      }
    }

    dev->avail->ring[dev->avail->idx % INPUT_QSIZE] = id;
    arch_data_barrier();
    dev->avail->idx++;
    dev->last_used_idx++;
  }

  v_write32(dev, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
  if (needs_render) {
    compositor_render();
  }
}

void virtio_input_init(void) {
  pr_info("%s", "VirtIO-Input: Probing devices...\n");

#ifdef ARCH_AMD64
  /* Probe PCI for VirtIO Input devices */
  /* Vendor 1AF4, Device 1000-103F. Legacy Input is usually around 0x1012 or matches subsystem. */
  /* For QEMU, we scan all VirtIO devices and check their subsystem ID or Device ID. */
  for (int dev = 0; dev < 32; dev++) {
      uint32_t id = pci_config_read(0, dev, 0, 0);
      if ((id & 0xFFFF) == 0x1AF4) {
          uint16_t devid = id >> 16;
          /* Legacy VirtIO-PCI Device IDs: 0x1000-0x103F. 
             The actual device type is often in the Subsystem Device ID. */
          uint32_t subsys = pci_config_read(0, dev, 0, 0x2C);
          uint16_t virtio_id = subsys >> 16;
          
          if (virtio_id == 18 || (devid >= 0x1000 && devid <= 0x103F && (pci_config_read(0, dev, 0, 0x08) >> 24) == 0)) {
              /* Found a potential VirtIO Input device */
              uint32_t bar0 = pci_get_bar((0 << 16) | (dev << 8) | 0, 0);
              if (bar0 & 1) { /* I/O BAR */
                  uintptr_t iobase = bar0 & ~3;
                  uint32_t irq = pci_get_interrupt((0 << 16) | (dev << 8) | 0);
                  /* In QEMU/x86 with PIC, we need to map the IRQ. 
                     PCI IRQs are usually 10, 11, etc. */
                  init_device(iobase, 32 + irq, 1);
              }
          }
      }
  }
#else
  /* Probe MMIO slots for AArch64 */
  for (uintptr_t addr = 0x0a000000; addr <= 0x0a003e00; addr += 0x200) {
    uint32_t magic = (*(volatile uint32_t *)(addr + 0));
    uint32_t devid = (*(volatile uint32_t *)(addr + 8));
    if (magic == 0x74726976 && devid == 18) {
      uint32_t slot = (addr - 0x0a000000) / 0x200;
      init_device(addr, 48 + slot, 0);
    }
  }
#endif
}

int virtio_input_poll(struct virtio_input_event *event) {
  if (input_head == input_tail)
    return 0;
  *event = input_buffer[input_tail];
  input_tail = (input_tail + 1) % INPUT_BUFFER_SIZE;
  return 1;
}

int virtio_input_has_event(void) { return input_head != input_tail; }
