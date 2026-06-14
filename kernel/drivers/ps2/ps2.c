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

static void ps2_wait_write(void) {
  int timeout = 100000;
  while ((inb(0x64) & 0x02) && timeout--)
    ;
  if (timeout <= 0)
    pr_warn("PS/2: write timeout\n");
}

static void ps2_wait_read(void) {
  int timeout = 100000;
  while (!(inb(0x64) & 0x01) && timeout--)
    ;
  if (timeout <= 0)
    pr_warn("PS/2: read timeout\n");
}

static uint8_t ps2_read_data(void) {
  ps2_wait_read();
  return inb(0x60);
}

static void ps2_write_cmd(uint8_t cmd) {
  ps2_wait_write();
  outb(0x64, cmd);
}

static void ps2_write_data(uint8_t data) {
  ps2_wait_write();
  outb(0x60, data);
}

/* Send a byte to the SECOND port (mouse/AUX): the 8042 routes the next 0x60
 * write to the mouse only after the 0xD4 controller command.  Without it every
 * "mouse" byte goes to the keyboard, so the mouse is never enabled.  Returns
 * the device's reply (ACK 0xFA on success). */
static uint8_t ps2_mouse_cmd(uint8_t cmd) {
  ps2_write_cmd(0xD4);
  ps2_write_data(cmd);
  return ps2_read_data();
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
  pr_info("PS/2: Initializing controller...\n");

  ps2_write_cmd(0xAD);
  ps2_write_cmd(0xA7);

  while (inb(0x64) & 1)
    inb(0x60);

  ps2_write_cmd(0x20);
  uint8_t cmd = ps2_read_data() | 0x03;
  ps2_write_cmd(0x60);
  ps2_write_data(cmd);

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
  ps2_mouse_cmd(0xF2);          /* get device id: returns ACK ... */
  if (ps2_read_data() == 0x03) { /* ... then the id byte */
    mouse_has_wheel = 1;
    pr_info("PS/2: Mouse with scroll wheel detected\n");
  }

  /* Register keyboard (IRQ 1) at vector 33 and mouse (IRQ 12) at vector 44. */
  irq_register(PIC_VECTOR_BASE + 1, ps2_keyboard_handler, NULL);
  irq_register(PIC_VECTOR_BASE + 12, ps2_mouse_handler, NULL);

  pr_info("PS/2: Keyboard + Mouse ready (IRQ 1/12 -> vec 33/44)\n");
}

#else

/* Stub per AArch64 */
void ps2_init(void) { /* Do nothing on AArch64 */ }

#endif