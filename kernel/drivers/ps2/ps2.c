/* kernel/drivers/ps2/ps2.c */
#include <arch/arch.h> // ← Questo include inb/outb
#include <drivers/keyboard.h>
#include <drivers/ps2.h>
#include <drivers/virtio_input.h>
#include <kernel/irq.h>
#include <kernel/printk.h>

#ifdef ARCH_AMD64

/* The 8259 PIC remaps IRQ n -> IDT vector 32+n (pic_init).  irq_register(),
 * irq_dispatch() and pic_chip_enable() all key on the VECTOR, not the bare
 * IRQ line, so device IRQs must be registered as 32+line. */
#define PIC_VECTOR_BASE 32

/* Bounded polls — return 0 when ready, -1 on timeout. The iteration cap is the
 * whole point: on a board without an 8042 the status port floats high, so a
 * naive wait would never progress. Bring-up must degrade, never block. */
static int ps2_wait_write(void) {
  for (int t = 100000; t > 0; t--)
    if (!(inb(0x64) & 0x02))
      return 0;
  return -1;
}

static int ps2_wait_read(void) {
  for (int t = 100000; t > 0; t--)
    if (inb(0x64) & 0x01)
      return 0;
  return -1;
}

/* Returns 0 and stores the byte on success, -1 on timeout (leaves *out). */
static int ps2_read_data(uint8_t *out) {
  if (ps2_wait_read() != 0)
    return -1;
  *out = inb(0x60);
  return 0;
}

static int ps2_write_cmd(uint8_t cmd) {
  if (ps2_wait_write() != 0)
    return -1;
  outb(0x64, cmd);
  return 0;
}

static int ps2_write_data(uint8_t data) {
  if (ps2_wait_write() != 0)
    return -1;
  outb(0x60, data);
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
static void ps2_keyboard_handler(uint32_t irq, void *data) {
  (void)irq;
  (void)data;
  uint8_t scancode = inb(0x60);

  uint16_t code = scancode & 0x7F;
  int pressed = (scancode & 0x80) == 0;

  input_report(EV_KEY, code, pressed ? 1 : 0);
  input_report(EV_SYN, 0, 0);
}

/* ==================== MOUSE ==================== */
static uint8_t mouse_packet[4];
static int mouse_byte = 0;
static int mouse_has_wheel = 0;

static void ps2_mouse_handler(uint32_t irq, void *data) {
  (void)irq;
  (void)data;

  if (mouse_byte >= 4)
    mouse_byte = 0;
  mouse_packet[mouse_byte++] = inb(0x60);

  if (mouse_byte == (mouse_has_wheel ? 4 : 3)) {
    uint8_t status = mouse_packet[0];
    int dx = (int8_t)mouse_packet[1];
    int dy = -(int8_t)mouse_packet[2];

    input_report(EV_KEY, BTN_LEFT, (status & 0x01) ? 1 : 0);
    input_report(EV_KEY, BTN_RIGHT, (status & 0x02) ? 1 : 0);
    input_report(EV_KEY, BTN_MIDDLE, (status & 0x04) ? 1 : 0);

    if (dx)
      input_report(EV_REL, REL_X, dx);
    if (dy)
      input_report(EV_REL, REL_Y, dy);

    if (mouse_has_wheel && mouse_packet[3] != 0) {
      input_report(EV_REL, REL_WHEEL, (int8_t)mouse_packet[3]);
    }

    input_report(EV_SYN, 0, 0);
    mouse_byte = 0;
  }
}

void ps2_init(void) {
  pr_info("%s", "PS/2: Initializing controller...\n");

  /* Non-blocking presence gate. On machines without an 8042 (many amd64 VMs,
   * e.g. UTM on Apple silicon) the status port floats to 0xFF; the old
   * unbounded "while (inb(0x64) & 1) inb(0x60);" flush then spun forever and
   * hung the boot. Detect the absent controller and skip cleanly. */
  if (inb(0x64) == 0xFF) {
    pr_info("%s", "PS/2: no controller present (0xFF) — skipping\n");
    return;
  }

  ps2_write_cmd(0xAD); /* disable keyboard port during setup */
  ps2_write_cmd(0xA7); /* disable mouse (AUX) port during setup */

  /* Bounded output-buffer flush (this loop was the UTM hang). */
  for (int i = 0; i < 16 && (inb(0x64) & 0x01); i++)
    (void)inb(0x60);

  /* Controller self-test: 0xAA must answer 0x55. Any other reply — or a read
   * timeout — means there is no usable controller, so skip non-blocking. */
  uint8_t resp = 0;
  ps2_write_cmd(0xAA);
  if (ps2_read_data(&resp) != 0 || resp != 0x55) {
    pr_info("PS/2: self-test failed (0x%x) — skipping\n", resp);
    return;
  }

  /* Read config byte, enable IRQs for both ports (bits 0,1), write it back. */
  ps2_write_cmd(0x20);
  uint8_t cfg = 0;
  if (ps2_read_data(&cfg) != 0)
    cfg = 0;
  cfg |= 0x03;
  ps2_write_cmd(0x60);
  ps2_write_data(cfg);

  ps2_write_cmd(0xAE); /* enable keyboard port */
  ps2_write_cmd(0xA8); /* enable mouse (AUX) port */

  /* Mouse setup — every byte goes through 0xD4 (ps2_mouse_cmd), or it would
   * be delivered to the keyboard and the mouse would never report. */
  ps2_mouse_cmd(0xF6); /* set defaults */
  ps2_mouse_cmd(0xF4); /* enable data reporting */

  /* IntelliMouse 3-button + wheel "magic knock": sample rates 200/100/80,
   * then read the device id; 0x03 means the wheel is active. */
  ps2_mouse_cmd(0xF3);
  ps2_mouse_cmd(200);
  ps2_mouse_cmd(0xF3);
  ps2_mouse_cmd(100);
  ps2_mouse_cmd(0xF3);
  ps2_mouse_cmd(80);
  ps2_mouse_cmd(0xF2); /* get device id: returns ACK ... */
  uint8_t id = 0;
  if (ps2_read_data(&id) == 0 && id == 0x03) { /* ... then the id byte */
    mouse_has_wheel = 1;
    pr_info("%s", "PS/2: Mouse with scroll wheel detected\n");
  }

  /* Register keyboard (IRQ 1) at vector 33 and mouse (IRQ 12) at vector 44. */
  irq_register(PIC_VECTOR_BASE + 1, ps2_keyboard_handler, NULL);
  irq_register(PIC_VECTOR_BASE + 12, ps2_mouse_handler, NULL);

  pr_info("%s", "PS/2: Keyboard + Mouse ready (IRQ 1/12 -> vec 33/44)\n");
}

#else

/* Stub per AArch64 */
void ps2_init(void) { /* Do nothing on AArch64 */ }

#endif