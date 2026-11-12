/* kernel/drivers/ps2/ps2.c */
#include <hal/core.h>
#include <drivers/input.h>
#include <drivers/input_codes.h>
#include <drivers/ps2.h>
#include <kernel/io_poll.h>
#include <kernel/irq.h>
#include <kernel/printk.h>


/* The 8259 PIC remaps IRQ n -> IDT vector 32+n (pic_init).  irq_register(),
 * irq_dispatch() and pic_chip_enable() all key on the VECTOR, not the bare
 * IRQ line, so device IRQs must be registered as 32+line. */
#define PIC_VECTOR_BASE 32

/* Bounded polls — return 0 when ready, -1 on timeout. The iteration cap is the
 * whole point: on a board without an 8042 the status port floats high, so a
 * naive wait would never progress. Bring-up must degrade, never block. */
static int ps2_wait_write(void) {
  int ok;
  poll_until(ok, !(hal_inb(0x64) & 0x02), 100000);
  return ok ? 0 : -1;
}

static int ps2_wait_read(void) {
  int ok;
  poll_until(ok, hal_inb(0x64) & 0x01, 100000);
  return ok ? 0 : -1;
}

/* Returns 0 and stores the byte on success, -1 on timeout (leaves *out). */
static int ps2_read_data(uint8_t *out) {
  if (ps2_wait_read() != 0)
    return -1;
  *out = hal_inb(0x60);
  return 0;
}

static int ps2_write_cmd(uint8_t cmd) {
  if (ps2_wait_write() != 0)
    return -1;
  hal_outb(0x64, cmd);
  return 0;
}

static int ps2_write_data(uint8_t data) {
  if (ps2_wait_write() != 0)
    return -1;
  hal_outb(0x60, data);
  return 0;
}

/* Send a byte to the SECOND port (mouse/AUX): the 8042 routes the next 0x60
 * write to the mouse only after the 0xD4 controller command.  Without it every
 * "mouse" byte goes to the keyboard, so the mouse is never enabled.  Returns
 * the device's reply (ACK 0xFA on success). */
static uint8_t ps2_mouse_cmd(uint8_t cmd) {
  uint8_t reply = 0xFF;
  ps2_write_cmd(0xD4);
  ps2_write_data(cmd);
  ps2_read_data(&reply);
  return reply;
}

/* ==================== KEYBOARD ==================== */
static uint16_t ps2_set1_extended_code(uint8_t scan) {
  switch (scan) {
  case 0x1C: return KEY_KPENTER;
  case 0x1D: return KEY_RIGHTCTRL;
  case 0x35: return KEY_KPSLASH;
  case 0x38: return KEY_RIGHTALT;
  case 0x47: return KEY_HOME;
  case 0x48: return KEY_UP;
  case 0x49: return KEY_PAGEUP;
  case 0x4B: return KEY_LEFT;
  case 0x4D: return KEY_RIGHT;
  case 0x4F: return KEY_END;
  case 0x50: return KEY_DOWN;
  case 0x51: return KEY_PAGEDOWN;
  case 0x52: return KEY_INSERT;
  case 0x53: return KEY_DELETE;
  default: return KEY_RESERVED;
  }
}

void ps2_keyboard_decoder_init(struct ps2_keyboard_decoder *decoder) {
  if (decoder == NULL)
    return;
  decoder->extended = 0U;
  decoder->e1_bytes_remaining = 0U;
}

int ps2_keyboard_decode_set1(struct ps2_keyboard_decoder *decoder,
                             uint8_t byte, struct ps2_key_event *event) {
  uint8_t scan;
  uint16_t code;

  if (decoder == NULL || event == NULL)
    return -1;
  if (decoder->e1_bytes_remaining != 0U) {
    --decoder->e1_bytes_remaining;
    return 0;
  }
  if (byte == UINT8_C(0xE1)) {
    decoder->extended = 0U;
    decoder->e1_bytes_remaining = 5U;
    return 0;
  }
  if (byte == UINT8_C(0xE0)) {
    decoder->extended = 1U;
    return 0;
  }

  scan = byte & UINT8_C(0x7F);
  code = decoder->extended != 0U ? ps2_set1_extended_code(scan) : scan;
  decoder->extended = 0U;
  if (code == KEY_RESERVED)
    return 0;
  event->code = code;
  event->value = (byte & UINT8_C(0x80)) == 0U ? 1 : 0;
  return 1;
}

static struct ps2_keyboard_decoder ps2_keyboard_decoder;

static void ps2_keyboard_handler(uint32_t irq, void *data) {
  struct ps2_key_event event;
  uint8_t byte;

  (void)irq;
  (void)data;
  byte = hal_inb(0x60);
  if (ps2_keyboard_decode_set1(&ps2_keyboard_decoder, byte, &event) != 1)
    return;

  OS1driver_input_publish(INPUT_PROVIDER_PS2, INPUT_DEVICE_PS2_KEYBOARD,
                           EV_KEY, event.code, event.value);
  OS1driver_input_publish(INPUT_PROVIDER_PS2, INPUT_DEVICE_PS2_KEYBOARD,
                           EV_SYN, 0, 0);
}

/* ==================== MOUSE ==================== */
static uint8_t mouse_packet[4];
static int mouse_byte = 0;
static int mouse_has_wheel = 0;

static void ps2_mouse_handler(uint32_t irq, void *data) {
  (void)irq;
  (void)data;

  uint8_t byte = hal_inb(0x60);

  /* Packet resync: packet[0] (status) always has bit 3 set. If the byte we
   * expect as the first one doesn't, the stream is misaligned (a leftover init
   * byte or a missed IRQ) — drop it instead of reading dx/dy from the wrong
   * bytes, which biases motion and drags the cursor sideways forever. */
  if (mouse_byte == 0 && !(byte & 0x08))
    return;

  mouse_packet[mouse_byte++] = byte;
  if (mouse_byte < (mouse_has_wheel ? 4 : 3))
    return;
  mouse_byte = 0;

  uint8_t status = mouse_packet[0];

  OS1driver_input_publish(INPUT_PROVIDER_PS2, INPUT_DEVICE_PS2_MOUSE, EV_KEY, BTN_LEFT, (status & 0x01) ? 1 : 0);
  OS1driver_input_publish(INPUT_PROVIDER_PS2, INPUT_DEVICE_PS2_MOUSE, EV_KEY, BTN_RIGHT, (status & 0x02) ? 1 : 0);
  OS1driver_input_publish(INPUT_PROVIDER_PS2, INPUT_DEVICE_PS2_MOUSE, EV_KEY, BTN_MIDDLE, (status & 0x04) ? 1 : 0);

  /* Skip motion when the device flags an X/Y overflow — the deltas are bogus. */
  if (!(status & 0xC0)) {
    int dx = (int8_t)mouse_packet[1];
    int dy = -(int8_t)mouse_packet[2];
    if (dx)
      OS1driver_input_publish(INPUT_PROVIDER_PS2, INPUT_DEVICE_PS2_MOUSE, EV_REL, REL_X, dx);
    if (dy)
      OS1driver_input_publish(INPUT_PROVIDER_PS2, INPUT_DEVICE_PS2_MOUSE, EV_REL, REL_Y, dy);
    if (mouse_has_wheel && mouse_packet[3] != 0)
      OS1driver_input_publish(INPUT_PROVIDER_PS2, INPUT_DEVICE_PS2_MOUSE, EV_REL, REL_WHEEL, (int8_t)mouse_packet[3]);
  }

  OS1driver_input_publish(INPUT_PROVIDER_PS2, INPUT_DEVICE_PS2_MOUSE, EV_SYN, 0, 0);
}

static void ps2_flush_output(void) {
  for (unsigned int i = 0U;
       i < 16U && (hal_inb(0x64) & UINT8_C(0x01)) != 0U; ++i)
    (void)hal_inb(0x60);
}

static int ps2_controller_setup(void) {
  uint8_t response = 0U;
  uint8_t config = 0U;

  if (ps2_write_cmd(0xAD) != 0 || ps2_write_cmd(0xA7) != 0)
    return -1;
  ps2_flush_output();
  if (ps2_write_cmd(0xAA) != 0 || ps2_read_data(&response) != 0 ||
      response != UINT8_C(0x55)) {
    pr_info("PS/2: self-test failed (0x%x) — skipping\n", response);
    return -1;
  }
  if (ps2_write_cmd(0x20) != 0 || ps2_read_data(&config) != 0)
    return -1;
  config |= UINT8_C(0x03);
  if (ps2_write_cmd(0x60) != 0 || ps2_write_data(config) != 0)
    return -1;
  if (ps2_write_cmd(0xAE) != 0 || ps2_write_cmd(0xA8) != 0)
    return -1;
  return 0;
}

static void ps2_mouse_setup(void) {
  uint8_t id = 0U;

  mouse_has_wheel = 0;
  (void)ps2_mouse_cmd(0xF6);
  (void)ps2_mouse_cmd(0xF4);
  (void)ps2_mouse_cmd(0xF3);
  (void)ps2_mouse_cmd(200);
  (void)ps2_mouse_cmd(0xF3);
  (void)ps2_mouse_cmd(100);
  (void)ps2_mouse_cmd(0xF3);
  (void)ps2_mouse_cmd(80);
  (void)ps2_mouse_cmd(0xF2);
  if (ps2_read_data(&id) == 0 && id == UINT8_C(0x03)) {
    mouse_has_wheel = 1;
    pr_info("%s", "PS/2: Mouse with scroll wheel detected\n");
  }
  ps2_flush_output();
}

static void ps2_register_irqs(void) {
  if (irq_register(PIC_VECTOR_BASE + 1, ps2_keyboard_handler, NULL) != 0)
    pr_err("%s", "ps2: cannot register the keyboard IRQ — no key events\n");
  if (irq_register(PIC_VECTOR_BASE + 12, ps2_mouse_handler, NULL) != 0)
    pr_err("%s", "ps2: cannot register the mouse IRQ — no pointer events\n");
}

void ps2_init(void) {
  ps2_keyboard_decoder_init(&ps2_keyboard_decoder);
  if (!hal_ioport_available()) {
    pr_info("%s", "PS/2: HAL provider has no port-I/O transport — skipping\n");
    return;
  }
  pr_info("%s", "PS/2: Initializing controller...\n");
  if (hal_inb(0x64) == UINT8_C(0xFF)) {
    pr_info("%s", "PS/2: no controller present (0xFF) — skipping\n");
    return;
  }
  if (ps2_controller_setup() != 0)
    return;
  ps2_mouse_setup();
  ps2_register_irqs();
  pr_info("%s", "PS/2: Keyboard + Mouse ready (IRQ 1/12 -> vec 33/44)\n");
}
