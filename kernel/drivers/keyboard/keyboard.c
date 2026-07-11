/*
 * kernel/drivers/keyboard/keyboard.c
 * Keyboard Input Subsystem
 *
 * Translates scancodes to ASCII and provides buffered input
 */
#include <drivers/keyboard.h>
#include <drivers/ps2.h>
#include <drivers/usb/usb.h>
#include <drivers/virtio_input.h>
#include <kernel/arch.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <posix_types.h>

/* Keyboard state */
static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int caps_lock = 0;

typedef struct {
  const char *name;
  const char *ascii_map;
  const char *shifted_map;
  struct {
    uint16_t code;
    int shifted;
    const char *utf8;
  } utf8_overrides[16];
} keyboard_layout_t;

/* Scancode to ASCII table (US layout) */
static const char scancode_to_ascii[128] = {
    0,    0,   '1', '2',  '3',  '4', '5',  '6',  /* 0-7 */
    '7',  '8', '9', '0',  '-',  '=', '\b', '\t', /* 8-15 */
    'q',  'w', 'e', 'r',  't',  'y', 'u',  'i',  /* 16-23 */
    'o',  'p', '[', ']',  '\n', 0,   'a',  's',  /* 24-31 */
    'd',  'f', 'g', 'h',  'j',  'k', 'l',  ';',  /* 32-39 */
    '\'', '`', 0,   '\\', 'z',  'x', 'c',  'v',  /* 40-47 */
    'b',  'n', 'm', ',',  '.',  '/', 0,    '*',  /* 48-55 */
    0,    ' ', 0,   0,    0,    0,   0,    0,    /* 56-63 (space at 57) */
    0,    0,   0,   0,    0,    0,   0,    0,    /* 64-71 */
    0,    0,   0,   0,    0,    0,   0,    0,    /* 72-79 */
    0,    0,   0,   0,    0,    0,   0,    0,    /* 80-87 */
    0,    0,   0,   0,    0,    0,   0,    0,    /* 88-95 */
    0,    0,   0,   0,    0,    0,   0,    0,    /* 96-103 */
    0,    0,   0,   0,    0,    0,   0,    0,    /* 104-111 */
    0,    0,   0,   0,    0,    0,   0,    0,    /* 112-119 */
    0,    0,   0,   0,    0,    0,   0,    0     /* 120-127 */
};

/* Shifted scancode to ASCII */
static const char scancode_to_ascii_shift[128] = {
    0,   0,   '!', '@', '#',  '$', '%',  '^',  /* 0-7 */
    '&', '*', '(', ')', '_',  '+', '\b', '\t', /* 8-15 */
    'Q', 'W', 'E', 'R', 'T',  'Y', 'U',  'I',  /* 16-23 */
    'O', 'P', '{', '}', '\n', 0,   'A',  'S',  /* 24-31 */
    'D', 'F', 'G', 'H', 'J',  'K', 'L',  ':',  /* 32-39 */
    '"', '~', 0,   '|', 'Z',  'X', 'C',  'V',  /* 40-47 */
    'B', 'N', 'M', '<', '>',  '?', 0,    '*',  /* 48-55 */
    0,   ' ', 0,   0,   0,    0,   0,    0,    /* 56-63 */
    0,   0,   0,   0,   0,    0,   0,    0,    /* 64-71 */
    0,   0,   0,   0,   0,    0,   0,    0,    /* 72-79 */
    0,   0,   0,   0,   0,    0,   0,    0,    /* 80-87 */
    0,   0,   0,   0,   0,    0,   0,    0,    /* 88-95 */
    0,   0,   0,   0,   0,    0,   0,    0,    /* 96-103 */
    0,   0,   0,   0,   0,    0,   0,    0,    /* 104-111 */
    0,   0,   0,   0,   0,    0,   0,    0,    /* 112-119 */
    0,   0,   0,   0,   0,    0,   0,    0     /* 120-127 */
};

/* Keyboard layouts.  Each layout supplies the base scancode->ASCII maps (so a
 * future layout can remap keys without touching the driver) plus optional
 * UTF-8 overrides for accented keys.  The driver consults current_layout for
 * every keystroke (KBD-LAYOUT-01): previously these maps were unused and the
 * static US tables were read directly. */
static const keyboard_layout_t layout_us = {
    .name = "us",
    .ascii_map = scancode_to_ascii,
    .shifted_map = scancode_to_ascii_shift,
};

static const keyboard_layout_t layout_it = {
    .name = "it",
    .ascii_map = scancode_to_ascii,
    .shifted_map = scancode_to_ascii_shift,
    .utf8_overrides = {{40, 0, "\xC3\xA0"}, // à
                       {40, 1, "\xC3\x80"}, // À
                       {26, 0, "\xC3\xA8"}, // è
                       {26, 1, "\xC3\xA9"}, // é
                       {39, 0, "\xC3\xB2"}, // ò
                       {41, 0, "\xC3\xB9"}, // ù
                       {43, 0, "\xC3\xAC"}, // ì
                       {0, 0, NULL}}};

static const keyboard_layout_t *current_layout = &layout_us;

/*
 * Initialize keyboard subsystem
 */
void keyboard_init(void) {
  shift_pressed = 0;
  ctrl_pressed = 0;
  caps_lock = 0;

  pr_info("%s", "Input: Initializing input subsystem...\n");

  /* Device-manager-driven bring-up: probe every input provider; each one
   * detects its own hardware and feeds the unified input_report() core. No
   * hardcoded per-arch "virtio or PS/2" selection anymore — whatever is
   * actually present (virtio-input, USB HID, legacy PS/2) becomes active, on
   * both architectures. Absent providers no-op (e.g. PS/2 on aarch64). */
  virtio_input_init(); /* virtio-input: MMIO on aarch64, PCI on amd64 */
  usb_init();          /* xHCI/EHCI/UHCI via the PCI driver manager + USB HID */
  ps2_init();          /* legacy 8042 (amd64 only; no-op elsewhere) */

  current_layout = &layout_it;

  INIT_LIST_HEAD(&keyboard_wait_queue.task_list);
  spin_lock_init(&keyboard_wait_queue.lock);

  pr_info("Keyboard: Initialized (Layout: %s)\n", current_layout->name);
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

/*
 * Process a key event
 */
static void keyboard_process_key(uint16_t code, int32_t value) {
  /* value: 0 = released, 1 = pressed, 2 = repeat */

  /* Handle modifier keys */
  if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
    shift_pressed = (value != 0);
    return;
  }

  if (code == KEY_LEFTCTRL) {
    ctrl_pressed = (value != 0);
    return;
  }

  if (code == KEY_CAPSLOCK && value == 1) {
    caps_lock = !caps_lock;
    return;
  }

  /* Convert scancode to ASCII */
  if (code >= 128)
    return;

  char c;
  int use_shift = shift_pressed;

  /* Apply caps lock to letters */
  if (code >= KEY_Q && code <= KEY_P)
    use_shift ^= caps_lock;
  if (code >= KEY_A && code <= KEY_L)
    use_shift ^= caps_lock;
  if (code >= KEY_Z && code <= KEY_M)
    use_shift ^= caps_lock;

  /* Read the ASCII from the active layout's maps (KBD-LAYOUT-01), falling back
   * to the built-in US tables if a layout omits them. */
  const char *amap =
      (current_layout && current_layout->ascii_map) ? current_layout->ascii_map
                                                     : scancode_to_ascii;
  const char *smap = (current_layout && current_layout->shifted_map)
                         ? current_layout->shifted_map
                         : scancode_to_ascii_shift;
  if (use_shift)
    c = smap[code];
  else
    c = amap[code];

  /* Ctrl + key -> control code (TTY semantics): Ctrl-A..Ctrl-Z => 0x01..0x1A,
   * Ctrl-@..Ctrl-_ => 0x00..0x1F.  Generalises the old Ctrl-C-only path so
   * full-screen apps (kilo: Ctrl-S/Q/F) and the shell's Ctrl-C all work
   * uniformly.  When folding to a control code we skip the UTF-8 override
   * pass below (a control char is never an accented glyph). */
  int is_ctrl_combo = 0;
  if (ctrl_pressed && c) {
    unsigned char uc = (unsigned char)c;
    if (uc >= 'a' && uc <= 'z') {
      c = (char)(uc - 'a' + 1);
      is_ctrl_combo = 1;
    } else if (uc >= 'A' && uc <= 'Z') {
      c = (char)(uc - 'A' + 1);
      is_ctrl_combo = 1;
    } else if (uc >= '@' && uc <= '_') {
      c = (char)(uc - '@');
      is_ctrl_combo = 1;
    }
  }

  /* Per-keystroke logging is debug-only: at pr_info level every press AND
   * release printed a line from IRQ context, flooding the serial log and
   * serialising all CPUs on the printk path while typing. */
  /* Snapshot the focus hint ONCE (#67): route this keystroke to a single,
   * consistent target instead of re-reading the shared global between the test
   * and the send (where the compositor could change it). */
  int focus_pid = sched_get_focus_pid();

  if (c != 0) {
    pr_debug("Keyboard: Char='%c' (val=%d) -> PID %d\n", c, value, focus_pid);
  }

  /* Send IPC message if we have a focus PID */
  if (focus_pid > 0) {
    struct ipc_message msg;
    memset(&msg, 0, sizeof(msg));
    msg.from = 0; /* Kernel/Driver */
    msg.type = IPC_TYPE_INPUT;
    msg.data1 = ((uint64_t)code << 16) | (uint8_t)c;
    msg.data2 = (uint64_t)value; /* 0=release, 1=press, 2=repeat */

    /* UTF-8 Handling */
    if (c != 0) {
      msg.payload[0] = c;
      msg.payload[1] = '\0';
    }

    /* Apply Layout Overrides (skipped for Ctrl-folded control codes) */
    if (value != 0 && current_layout && !is_ctrl_combo) {
      for (int i = 0; i < 16 && current_layout->utf8_overrides[i].utf8 != NULL;
           i++) {
        if (current_layout->utf8_overrides[i].code == code &&
            current_layout->utf8_overrides[i].shifted == shift_pressed) {
          strlcpy(msg.payload, current_layout->utf8_overrides[i].utf8,
                  sizeof(msg.payload));
          break;
        }
      }
    }

    kernel_ipc_send(focus_pid, &msg);
  }
}

/* Compositor sinks for pointer events (graphics layer). These update window
 * state + damage; the repaint is the compositor's own 30 Hz tick. */
extern void compositor_update_mouse(int dx, int dy, int absolute);
extern void compositor_handle_click(int button, int state);

/* ------------------------------------------------------------------------- *
 * INPUT DECOUPLING (DIR-02/DIR-03, #68/#131/#194).
 *
 * The hardware input IRQs (virtio-input, PS/2) no longer touch the compositor.
 * They ONLY enqueue a raw evdev event into the ring below (input_report); the
 * dispatch — key->layout+IPC, pointer->compositor — runs in input_drain(),
 * called from the compositor tick (timer IRQ, CPU0).  So the input IRQs never
 * take compositor_lock and never mutate the window-list, killing the
 * cross-core input-IRQ-vs-render stall (#194) and the IRQ-context window
 * mutation from those paths (#68).  Dispatch is now a single serialized
 * bottom-half in one context (the tick), reflected in the same tick's render.
 *
 * NEXT STEP (foundation committed, staged): move input_drain into the
 * input server kernel THREAD (input_thread_entry, task context) via
 * kthread_block/arch_cpu_yield — fully off IRQ.  The arch_cpu_yield HAL
 * primitive + kthread are validated for kernel-thread switches, but the
 * cooperative switch TO a freshly-woken USER task still stalls CPU0 (a subtle
 * EL0-return / cross-CPU interaction in the hand-rolled epilogue); once that
 * is routed through the proven trap epilogue, input_server_start() replaces
 * the tick-drain.  input_server_start() is therefore not called yet.
 * ------------------------------------------------------------------------- */

struct input_evt {
  uint16_t type;
  uint16_t code;
  int32_t value;
};

#define INPUT_RING_SZ 256 /* power of two; ~256 evdev events of backlog */
static struct input_evt input_ring[INPUT_RING_SZ];
static volatile uint32_t input_ring_head; /* producer index (IRQ) */
static volatile uint32_t input_ring_tail; /* consumer index (thread) */
static DEFINE_SPINLOCK(input_ring_lock);
static uint64_t input_ring_dropped;
static struct wait_queue_head input_wait_queue;
static int input_ring_pop(struct input_evt *out);

/* input_dispatch - the actual event handling (key->layout+IPC, pointer->
 * compositor).  Currently invoked SYNCHRONOUSLY from input_report; becomes
 * the input server thread's body once the yield-to-user path is fixed. */
static void input_dispatch(uint16_t type, uint16_t code, int32_t value) {
  switch (type) {
  case EV_KEY:
    if (code == BTN_LEFT || code == BTN_RIGHT || code == BTN_MIDDLE) {
      /* All pointer buttons reach the compositor; it drives WM actions
       * (drag/resize/titlebar) from BTN_LEFT only and forwards every
       * button to the focused app (right/middle were silently dropped
       * before, so apps could never see them). */
      compositor_handle_click((int)code, value);
    } else {
      keyboard_process_key(code, value);
      wake_up(&keyboard_wait_queue);
    }
    break;
  case EV_REL:
    if (code == REL_X) compositor_update_mouse(value, 0, 0);
    else if (code == REL_Y) compositor_update_mouse(0, value, 0);
    break;
  case EV_ABS:
    if (code == ABS_X) compositor_update_mouse(value, -1, 1);
    else if (code == ABS_Y) compositor_update_mouse(-1, value, 1);
    break;
  default: /* EV_SYN and others */
    break;
  }
}

/*
 * input_report - the single entry point for every input provider (IRQ / poll
 * context).  DIAGNOSTIC/bottom-half mode: enqueue the raw event; the actual
 * dispatch runs in input_drain() from the compositor tick (timer IRQ, CPU0) —
 * proven context — so the virtio/ps2 INPUT IRQs no longer take compositor_lock
 * (kills the cross-core input-IRQ-vs-render stall, #194/#68).
 */
void input_report(uint16_t type, uint16_t code, int32_t value) {
  uint64_t flags;
  spin_lock_irqsave(&input_ring_lock, &flags);
  uint32_t next = (input_ring_head + 1) & (INPUT_RING_SZ - 1);
  if (next == input_ring_tail) {
    input_ring_dropped++;
  } else {
    input_ring[input_ring_head].type = type;
    input_ring[input_ring_head].code = code;
    input_ring[input_ring_head].value = value;
    input_ring_head = next;
  }
  spin_unlock_irqrestore(&input_ring_lock, flags);
}

/* input_drain - dispatch all queued raw events.  Called from the compositor
 * tick (timer IRQ, CPU0), so input dispatch is serialized with the render in
 * one context and never spins cross-core on compositor_lock from an input IRQ. */
void input_drain(void) {
  struct input_evt e;
  while (input_ring_pop(&e))
    input_dispatch(e.type, e.code, e.value);
}

/* input_ring_pop - consumer side (input thread): pull one event, or 0 if empty.
 * Part of the staged decoupling plumbing (unused until the thread is on). */
static int input_ring_pop(struct input_evt *out) {
  uint64_t flags;
  int got = 0;
  spin_lock_irqsave(&input_ring_lock, &flags);
  if (input_ring_tail != input_ring_head) {
    *out = input_ring[input_ring_tail];
    input_ring_tail = (input_ring_tail + 1) & (INPUT_RING_SZ - 1);
    got = 1;
  }
  spin_unlock_irqrestore(&input_ring_lock, flags);
  return got;
}

/* input_ring_nonempty - lost-wakeup guard: evaluated under the wq lock,
 * serialized against a producer's wake_up() (see kthread_block). */
static int input_ring_nonempty(void *arg) {
  (void)arg;
  return input_ring_head != input_ring_tail;
}

/* input_thread_entry - the input server kernel thread body (staged): drains
 * the ring and dispatches in TASK context, blocking when idle.  Wired but not
 * launched yet (input_server_start is not called). */
static void input_thread_entry(void) {
  for (;;) {
    struct input_evt e;
    while (input_ring_pop(&e))
      input_dispatch(e.type, e.code, e.value);
    kthread_block(&input_wait_queue, input_ring_nonempty, NULL);
  }
}

/* input_server_start - create the input server thread (staged; not yet
 * called).  When activated, input_report enqueues to the ring + wakes this. */
void input_server_start(void) {
  INIT_LIST_HEAD(&input_wait_queue.task_list);
  spin_lock_init(&input_wait_queue.lock);
  (void)input_ring_dropped;
  kthread_create("input", input_thread_entry);
  pr_info("%s", "Input: server thread started (IRQ-decoupled dispatch)\n");
}

/*
 * Poll keyboard for new input
 */
void keyboard_poll(void) {
  struct virtio_input_event event;

  while (virtio_input_poll(&event)) {
    if (event.type == EV_KEY) {
      keyboard_process_key(event.code, event.value);
    }
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
