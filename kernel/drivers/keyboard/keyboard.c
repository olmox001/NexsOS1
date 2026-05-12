/*
 * kernel/drivers/keyboard/keyboard.c
 * Keyboard Input Subsystem
 *
 * Translates scancodes to ASCII and provides buffered input
 */
#include <drivers/keyboard.h>
#include <drivers/virtio_input.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/types.h>

/* Keyboard state */
static int shift_pressed = 0;
static int ctrl_pressed = 0;
static int caps_lock = 0;

/* Input buffer */
#define KB_BUFFER_SIZE 256
static char kb_buffer[KB_BUFFER_SIZE];
static volatile uint32_t kb_head = 0;
static volatile uint32_t kb_tail = 0;

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

/*
 * Initialize keyboard subsystem
 */
void keyboard_init(void) {
  kb_head = 0;
  kb_tail = 0;
  shift_pressed = 0;
  ctrl_pressed = 0;
  caps_lock = 0;

  /* Initialize VirtIO Input driver */
  virtio_input_init();

  INIT_LIST_HEAD(&keyboard_wait_queue.task_list);
  spin_lock_init(&keyboard_wait_queue.lock);

  pr_info("%s", "Keyboard: Initialized\n");
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

  /* Only process key presses, not releases */
  if (value == 0)
    return;

  /* Check for Ctrl+C */
  if (ctrl_pressed && code == KEY_C) {
    /* Add ETX (End of Text) to buffer */
    char c = 0x03;
    uint32_t next = (kb_head + 1) % KB_BUFFER_SIZE;
    if (next != kb_tail) {
      kb_buffer[kb_head] = c;
      kb_head = next;
    }
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

  if (use_shift)
    c = scancode_to_ascii_shift[code];
  else
    c = scancode_to_ascii[code];

  /* Send IPC message if we have a focus PID */
  if (c != 0 && keyboard_focus_pid > 0) {
    struct ipc_message msg;
    msg.from = 0; /* Kernel/Driver */
    msg.type = IPC_TYPE_INPUT;
    msg.data1 = (uint64_t)c;
    msg.data2 = 0;
    /* No payload needed for single char */

    kernel_ipc_send(keyboard_focus_pid, &msg);
  }
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
