#include <drivers/input.h>
#include <drivers/ps2.h>
#include <drivers/usb/usb.h>
#include <drivers/virtio_input.h>
#include <kernel/cpu.h>
#include <kernel/fault.h>
#include <kernel/nx_contract.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <stdint.h>

#define OS1_INPUT_RING_SIZE UINT32_C(256)
#define OS1_INPUT_WAKE_KEYBOARD UINT32_C(1)
#define OS1_INPUT_WAKE_MOUSE UINT32_C(2)

struct os1_input_event_internal {
  uint64_t sequence;
  uint32_t producer_cpu;
  uint8_t provider;
  uint8_t device_id;
  uint16_t type;
  uint16_t code;
  int32_t value;
};

static struct os1_input_event_internal keyboard_ring[OS1_INPUT_RING_SIZE];
static struct os1_input_event_internal mouse_ring[OS1_INPUT_RING_SIZE];
static uint32_t keyboard_head;
static uint32_t keyboard_tail;
static uint32_t mouse_head;
static uint32_t mouse_tail;
static uint64_t input_event_sequence;
static uint64_t input_ring_dropped;
static uint8_t input_frame_pending[UINT8_MAX + 1U];
static struct wait_queue_head keyboard_wait_queue;
static struct wait_queue_head mouse_wait_queue;
static DEFINE_SPINLOCK(input_ring_lock);
static DEFINE_SPINLOCK(input_poll_lock);

int OS1libkernel_input_role_valid(unsigned role) {
  return role == OS1_INPUT_ROLE_KEYBOARD || role == OS1_INPUT_ROLE_MOUSE;
}

static int input_event_type_valid(uint16_t type) {
  return type == EV_SYN || type == EV_KEY || type == EV_REL ||
         type == EV_ABS || type == EV_MSC;
}

static struct wait_queue_head *input_role_wait_queue(unsigned role) {
  if (role == OS1_INPUT_ROLE_KEYBOARD)
    return &keyboard_wait_queue;
  if (role == OS1_INPUT_ROLE_MOUSE)
    return &mouse_wait_queue;
  return NULL;
}

static int input_ring_push_locked(struct os1_input_event_internal *ring,
                                  uint32_t *head, uint32_t *tail,
                                  const struct os1_input_event_internal *event) {
  uint32_t next = (*head + 1U) & (OS1_INPUT_RING_SIZE - 1U);

  if (next == *tail) {
    ++input_ring_dropped;
    return 0;
  }
  ring[*head] = *event;
  *head = next;
  return 1;
}

static int input_is_mouse_event(uint16_t type, uint16_t code) {
  return type == EV_REL || type == EV_ABS ||
         (type == EV_KEY &&
          (code == BTN_LEFT || code == BTN_RIGHT ||
           code == BTN_MIDDLE || code == BTN_TOUCH));
}

static struct os1_input_event_internal input_make_event(
    uint8_t provider, uint8_t device_id, uint16_t type, uint16_t code,
    int32_t value, uint64_t sequence) {
  struct cpu_info *cpu = cpu_info_fault_safe();
  struct os1_input_event_internal event = {
      .sequence = sequence,
      .producer_cpu = cpu != NULL ? cpu->cpu_id : UINT32_MAX,
      .provider = provider,
      .device_id = device_id,
      .type = type,
      .code = code,
      .value = value,
  };
  return event;
}

static unsigned input_publish_syn_locked(
    const struct os1_input_event_internal *event) {
  unsigned roles = 0U;
  uint8_t pending = input_frame_pending[event->device_id];

  if ((pending & OS1_INPUT_WAKE_KEYBOARD) != 0U &&
      input_ring_push_locked(keyboard_ring, &keyboard_head, &keyboard_tail,
                             event) != 0)
    roles |= OS1_INPUT_WAKE_KEYBOARD;
  if ((pending & OS1_INPUT_WAKE_MOUSE) != 0U &&
      input_ring_push_locked(mouse_ring, &mouse_head, &mouse_tail, event) != 0)
    roles |= OS1_INPUT_WAKE_MOUSE;
  input_frame_pending[event->device_id] = 0U;
  return roles;
}

static unsigned input_publish_data_locked(
    const struct os1_input_event_internal *event) {
  if (input_is_mouse_event(event->type, event->code) != 0) {
    input_frame_pending[event->device_id] |= OS1_INPUT_WAKE_MOUSE;
    return input_ring_push_locked(mouse_ring, &mouse_head, &mouse_tail, event)
               ? OS1_INPUT_WAKE_MOUSE : 0U;
  }
  if (event->type == EV_KEY || event->type == EV_MSC)
    input_frame_pending[event->device_id] |= OS1_INPUT_WAKE_KEYBOARD;
  return input_ring_push_locked(keyboard_ring, &keyboard_head, &keyboard_tail,
                                event)
             ? OS1_INPUT_WAKE_KEYBOARD : 0U;
}

static void input_wake_roles(unsigned roles) {
  if ((roles & OS1_INPUT_WAKE_KEYBOARD) != 0U)
    wake_up(&keyboard_wait_queue);
  if ((roles & OS1_INPUT_WAKE_MOUSE) != 0U)
    wake_up(&mouse_wait_queue);
}

void OS1driver_input_publish(uint8_t provider, uint8_t device_id,
                             uint16_t type, uint16_t code, int32_t value) {
  struct os1_input_event_internal event;
  unsigned roles;
  uint64_t flags;

  if (provider > INPUT_PROVIDER_LEGACY || input_event_type_valid(type) == 0) {
    __atomic_fetch_add(&input_ring_dropped, 1U, __ATOMIC_RELAXED);
    return;
  }
  spin_lock_irqsave(&input_ring_lock, &flags);
  event = input_make_event(provider, device_id, type, code, value,
                           ++input_event_sequence);
  roles = type == EV_SYN ? input_publish_syn_locked(&event)
                         : input_publish_data_locked(&event);
  spin_unlock_irqrestore(&input_ring_lock, flags);
  input_wake_roles(roles);
}

static int input_ring_pop(struct os1_input_event_internal *out, unsigned role) {
  struct os1_input_event_internal *ring;
  uint32_t *head;
  uint32_t *tail;
  uint64_t flags;
  int got = 0;

  spin_lock_irqsave(&input_ring_lock, &flags);
  head = role == OS1_INPUT_ROLE_MOUSE ? &mouse_head : &keyboard_head;
  tail = role == OS1_INPUT_ROLE_MOUSE ? &mouse_tail : &keyboard_tail;
  ring = role == OS1_INPUT_ROLE_MOUSE ? mouse_ring : keyboard_ring;
  if (*tail != *head) {
    *out = ring[*tail];
    *tail = (*tail + 1U) & (OS1_INPUT_RING_SIZE - 1U);
    got = 1;
  }
  spin_unlock_irqrestore(&input_ring_lock, flags);
  return got;
}

static int input_role_ready(unsigned role) {
  uint32_t head;
  uint32_t tail;
  uint64_t flags;

  spin_lock_irqsave(&input_ring_lock, &flags);
  head = role == OS1_INPUT_ROLE_MOUSE ? mouse_head : keyboard_head;
  tail = role == OS1_INPUT_ROLE_MOUSE ? mouse_tail : keyboard_tail;
  spin_unlock_irqrestore(&input_ring_lock, flags);
  return head != tail;
}

static int input_role_still_blocked(void *arg) {
  unsigned role = (unsigned)(uintptr_t)arg;
  return input_role_ready(role) == 0;
}

static void input_poll_compat_providers(void) {
  uint64_t flags;

  spin_lock_irqsave(&input_poll_lock, &flags);
  usb_hid_poll();
  spin_unlock_irqrestore(&input_poll_lock, flags);
}

static void input_export_event(struct os1_raw_input_event_v1 *out,
                               const struct os1_input_event_internal *event) {
  *out = (struct os1_raw_input_event_v1){
      .abi_version = OS1_INPUT_ABI_V1,
      .struct_size = sizeof(*out),
      .sequence = event->sequence,
      .producer_cpu = event->producer_cpu,
      .provider = event->provider,
      .device_id = event->device_id,
      .type = event->type,
      .code = event->code,
      .value = event->value,
  };
}

long OS1libkernel_input_read_raw(struct os1_raw_input_event_v1 *event,
                                 unsigned role, unsigned mode) {
  struct os1_input_event_internal raw;
  struct wait_queue_head *queue;

  if (event == NULL || OS1libkernel_input_role_valid(role) == 0)
    return -EINVAL;
  if (mode != OS1_INPUT_READ_TRY && mode != OS1_INPUT_READ_WAIT)
    return -EINVAL;
  input_poll_compat_providers();
  if (input_ring_pop(&raw, role) != 0) {
    input_export_event(event, &raw);
    return 1;
  }
  if (mode == OS1_INPUT_READ_TRY)
    return 0;
  if (usb_hid_polling_required() != 0)
    return -EAGAIN;
  queue = input_role_wait_queue(role);
  kthread_block(queue, input_role_still_blocked, (void *)(uintptr_t)role);
  if (input_ring_pop(&raw, role) == 0)
    return -EAGAIN;
  input_export_event(event, &raw);
  return 1;
}

void OS1driver_input_core_init(void) {
  keyboard_head = 0U;
  keyboard_tail = 0U;
  mouse_head = 0U;
  mouse_tail = 0U;
  input_event_sequence = 0U;
  input_ring_dropped = 0U;
  memset(input_frame_pending, 0, sizeof(input_frame_pending));
  INIT_LIST_HEAD(&keyboard_wait_queue.task_list);
  spin_lock_init(&keyboard_wait_queue.lock);
  INIT_LIST_HEAD(&mouse_wait_queue.task_list);
  spin_lock_init(&mouse_wait_queue.lock);
}

void OS1driver_input_providers_init(void) {
  pr_info("%s", "Input: initializing modular providers...\n");
  virtio_input_init();
  usb_init();
  ps2_init();
  pr_info("%s", "Input: provider discovery complete\n");
}

void OS1libkernel_input_trace_dump(void) {
  fault_printf("[INPUT-TRACE] event_seq=%llu ring_dropped=%llu\n",
               (unsigned long long)input_event_sequence,
               (unsigned long long)input_ring_dropped);
}
