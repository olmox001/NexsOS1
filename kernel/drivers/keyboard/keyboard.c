/*
 * kernel/drivers/keyboard/keyboard.c
 * Keyboard Input Subsystem
 *
 * Translates scancodes to ASCII and provides buffered input
 */
#include <kernel/nx_contract.h>
#include <drivers/keyboard.h>
#include <drivers/ps2.h>
#include <drivers/usb/usb.h>
#include <drivers/virtio_input.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/fault.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <os1_input.h>

/*
 * Initialize keyboard subsystem
 */
void keyboard_init(void) {
  pr_info("%s", "Input: Initializing input subsystem...\n");

  /* Device-manager-driven bring-up: probe every input provider; each one
   * detects its own hardware and feeds the unified input_report() core. No
   * hardcoded per-arch "virtio or PS/2" selection anymore — whatever is
   * actually present (virtio-input, USB HID, legacy PS/2) becomes active, on
   * both architectures. Absent providers no-op (e.g. PS/2 on aarch64). */
  virtio_input_init(); /* virtio-input: MMIO on aarch64, PCI on amd64 */
  usb_init();          /* xHCI/EHCI/UHCI via the PCI driver manager + USB HID */
  ps2_init();          /* legacy 8042 (amd64 only; no-op elsewhere) */

  INIT_LIST_HEAD(&keyboard_wait_queue.task_list);
  spin_lock_init(&keyboard_wait_queue.lock);

  pr_info("%s", "Keyboard: Raw input providers initialized\n");
}

/* Wait Queue for blocking reads */
struct wait_queue_head keyboard_wait_queue;

/*
 * Notification from low-level driver
 * Called from Interrupt Context (VirtIO Handler)
 */
void keyboard_notify_input(void) {
  /* Poll hardware to transfer from VirtIO buffer to Keyboard buffer */
  keyboard_poll();

  /* Wake up waiting processes */
  wake_up(&keyboard_wait_queue);
}

/* Key translation and client routing execute in ring-3 nxkeyboardd. */

/* IRQ and polling providers enqueue only; nxkeyboardd and nxmoused consume
 * independent raw queues and own all input policy in ring 3. */

struct input_evt {
  uint64_t seq;
  uint32_t producer_cpu;
  uint8_t provider;
  uint8_t device_id;
  uint8_t _reserved[2];
  uint16_t type;
  uint16_t code;
  int32_t value;
};

#define INPUT_RING_SZ 256u
static struct input_evt keyboard_ring[INPUT_RING_SZ];
static struct input_evt mouse_ring[INPUT_RING_SZ];
static volatile uint32_t keyboard_head, keyboard_tail;
static volatile uint32_t mouse_head, mouse_tail;
static uint64_t input_event_seq;
static uint64_t input_ring_dropped;
static uint8_t input_frame_pending[256];
static DEFINE_SPINLOCK(input_ring_lock);
static DEFINE_SPINLOCK(input_poll_lock);

/* Hardware providers only enqueue raw events. Each userland input service
 * owns a separate queue, so key and pointer consumers cannot steal events. */
void input_report_from_device(uint8_t provider, uint8_t device_id,
                              uint16_t type, uint16_t code, int32_t value) {
  uint64_t flags;
  struct cpu_info *cpu = cpu_info_fault_safe();
  spin_lock_irqsave(&input_ring_lock, &flags);
  uint64_t sequence = ++input_event_seq;
  if (type == EV_SYN) {
    struct input_evt event = {
        .seq = sequence, .producer_cpu = cpu ? cpu->cpu_id : UINT32_MAX,
        .provider = provider, .device_id = device_id,
        .type = type, .code = code, .value = value};
    if (input_frame_pending[device_id] & 1u) {
      uint32_t next = (keyboard_head + 1u) & (INPUT_RING_SZ - 1u);
      if (next == keyboard_tail) ++input_ring_dropped;
      else { keyboard_ring[keyboard_head] = event; keyboard_head = next; }
    }
    if (input_frame_pending[device_id] & 2u) {
      uint32_t next = (mouse_head + 1u) & (INPUT_RING_SZ - 1u);
      if (next == mouse_tail) ++input_ring_dropped;
      else { mouse_ring[mouse_head] = event; mouse_head = next; }
    }
    input_frame_pending[device_id] = 0;
    spin_unlock_irqrestore(&input_ring_lock, flags);
    return;
  }
  int is_mouse = (type == EV_REL || type == EV_ABS) ||
                 (type == EV_KEY && (code == BTN_LEFT || code == BTN_RIGHT ||
                                     code == BTN_MIDDLE || code == BTN_TOUCH));
  if (is_mouse) input_frame_pending[device_id] |= 2u;
  else if (type == EV_KEY || type == EV_MSC)
    input_frame_pending[device_id] |= 1u;
  struct input_evt *ring = is_mouse ? mouse_ring : keyboard_ring;
  volatile uint32_t *head = is_mouse ? &mouse_head : &keyboard_head;
  volatile uint32_t *tail = is_mouse ? &mouse_tail : &keyboard_tail;
  uint32_t next = (*head + 1u) & (INPUT_RING_SZ - 1u);
  if (next == *tail) {
    ++input_ring_dropped;
  } else {
    ring[*head] = (struct input_evt){
        .seq = sequence,
        .producer_cpu = cpu ? cpu->cpu_id : UINT32_MAX,
        .provider = provider,
        .device_id = device_id,
        .type = type,
        .code = code,
        .value = value,
    };
    *head = next;
  }
  spin_unlock_irqrestore(&input_ring_lock, flags);
}

void input_report_from(uint8_t provider, uint16_t type, uint16_t code,
                       int32_t value) {
  input_report_from_device(provider, provider, type, code, value);
}

void input_report(uint16_t type, uint16_t code, int32_t value) {
  input_report_from_device(INPUT_PROVIDER_UNKNOWN, INPUT_PROVIDER_UNKNOWN,
                           type, code, value);
}

void input_trace_dump(void) {
  fault_printf("[INPUT-TRACE] event_seq=%llu ring_dropped=%llu\n",
               (unsigned long long)input_event_seq,
               (unsigned long long)input_ring_dropped);
}

static int input_service_ring_pop(struct input_evt *out, unsigned role) {
  uint64_t flags;
  int got = 0;
  spin_lock_irqsave(&input_ring_lock, &flags);
  volatile uint32_t *head = role == OS1_INPUT_ROLE_MOUSE ? &mouse_head : &keyboard_head;
  volatile uint32_t *tail = role == OS1_INPUT_ROLE_MOUSE ? &mouse_tail : &keyboard_tail;
  struct input_evt *ring = role == OS1_INPUT_ROLE_MOUSE ? mouse_ring : keyboard_ring;
  if (*tail != *head) {
    *out = ring[*tail];
    *tail = (*tail + 1u) & (INPUT_RING_SZ - 1u);
    got = 1;
  }
  spin_unlock_irqrestore(&input_ring_lock, flags);
  return got;
}

long input_next_raw_event(struct os1_raw_input_event_v1 *user_event,
                          unsigned role) {
  if (!user_event)
    return -EFAULT;
  if (role != OS1_INPUT_ROLE_KEYBOARD && role != OS1_INPUT_ROLE_MOUSE)
    return -EINVAL;
  uint64_t poll_flags;
  spin_lock_irqsave(&input_poll_lock, &poll_flags);
  usb_hid_poll();
  keyboard_poll();
  spin_unlock_irqrestore(&input_poll_lock, poll_flags);
  struct input_evt event;
  if (!input_service_ring_pop(&event, role))
    return 0;
  struct os1_raw_input_event_v1 out = {
      .abi_version = OS1_INPUT_ABI_V1,
      .struct_size = sizeof(out),
      .sequence = event.seq,
      .producer_cpu = event.producer_cpu,
      .provider = event.provider,
      .device_id = event.device_id,
      .type = event.type,
      .code = event.code,
      .value = event.value,
  };
  return vmm_copy_to_user(user_event, &out, sizeof(out)) == 0 ? 1 : -EFAULT;
}

/* Retained source-compatibility entry point; no kernel input thread is used. */
void input_server_start(void) {}

/* Poll provider queues for the supervised ring-3 input daemons. */
void keyboard_poll(void) {
  struct virtio_input_event event;

  while (virtio_input_poll(&event)) {
    input_report_from(INPUT_PROVIDER_VIRTIO, event.type, event.code, event.value);
  }
}

/*
 * Check if keyboard has buffered input
 */
/*
 * Read one character from keyboard buffer (non-blocking) - DEPRECATED
 * Standard input should now be handled via IPC messages.
 */
int keyboard_read_char_nonblock(void) { return -1; }

/*
 * Read one character from keyboard buffer (blocking) - DEPRECATED
 */
char keyboard_read_char(void) { return 0; }

/*
 * Read a line of input (blocking, with echo)
 */
int keyboard_read_line(char *buf, int max_len) {
  int i = 0;

  while (i < max_len - 1) {
    char c = keyboard_read_char();

    if (c == '\n' || c == '\r') {
      buf[i] = '\0';
      return i;
    }

    if (c == '\b' || c == 127) {
      if (i > 0) {
        i--;
        /* Echo backspace */
        printk("\b \b");
      }
      continue;
    }

    if (c >= 32 && c < 127) {
      buf[i++] = c;
      /* Echo character */
      printk("%c", c);
    }
  }

  buf[i] = '\0';
  return i;
}
