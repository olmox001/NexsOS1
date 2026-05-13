/*
 * kernel/arch/amd64/drivers/pic_pit.c
 * Legacy 8259 PIC and 8253 PIT initialization for x86-64
 */
#include <kernel/types.h>
#include <kernel/hal.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <arch/pt_regs.h>

/* Prototypes to satisfy -Wmissing-prototypes */
void pic_init(void);
void pic_send_eoi(uint8_t irq);
struct pt_regs *amd64_timer_interrupt(struct pt_regs *regs);
struct pt_regs *amd64_keyboard_interrupt(struct pt_regs *regs);

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define PIT_CH0   0x40
#define PIT_CMD   0x43

extern volatile uint64_t jiffies;
extern void timer_tick(void); /* generic scheduler tick */
void pit_init_hz(uint32_t hz);

void pic_unmask(uint8_t irq);
void pic_mask(uint8_t irq);

static void pic_chip_enable(uint32_t irq) {
    if (irq >= 32 && irq < 48) {
        pic_unmask(irq - 32);
    }
}

void pic_mask(uint8_t irq) {
    uint16_t port;
    uint8_t value;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = hal_read8(port) | (1 << irq);
    hal_write8(port, value);
}

static void pic_chip_disable(uint32_t irq) {
    if (irq >= 32 && irq < 48) {
        pic_mask(irq - 32);
    }
}

static uint32_t pic_chip_acknowledge(void) {
    /* Not used by PIC on x86 because the vector is in pt_regs */
    return 1023; 
}

static void pic_chip_end(uint32_t irq) {
    if (irq >= 32 && irq < 48) {
        pic_send_eoi(irq - 32);
    }
}

static struct irq_chip pic_chip = {
    .name = "8259 PIC",
    .init = NULL,
    .enable = pic_chip_enable,
    .disable = pic_chip_disable,
    .acknowledge = pic_chip_acknowledge,
    .end = pic_chip_end,
};

void pic_init(void) {
    irq_register_chip(&pic_chip);
  /* Remap PIC IRQs 0-15 to interrupts 32-47 */
  hal_write8(PIC1_CMD, 0x11); /* ICW1: Init + ICW4 */
  hal_write8(PIC2_CMD, 0x11);
  
  hal_write8(PIC1_DATA, 0x20); /* ICW2: PIC1 vector offset 32 */
  hal_write8(PIC2_DATA, 0x28); /* ICW2: PIC2 vector offset 40 */
  
  hal_write8(PIC1_DATA, 0x04); /* ICW3: PIC1 has slave on IRQ2 */
  hal_write8(PIC2_DATA, 0x02); /* ICW3: PIC2 cascade identity */
  
  hal_write8(PIC1_DATA, 0x01); /* ICW4: 8086 mode */
  hal_write8(PIC2_DATA, 0x01);
  
  /* Start with all IRQs masked except slave cascade (IRQ 2) */
  hal_write8(PIC1_DATA, 0xFB);
  hal_write8(PIC2_DATA, 0xFF);
  
  pr_info("PIC Initialized and remapped to 32-47.\n");
}

extern struct pt_regs *kernel_timer_tick(struct pt_regs *regs);

/* The interrupt handlers amd64_timer_interrupt and amd64_keyboard_interrupt 
 * have been moved to idt.c for unified LAPIC/PIC EOI management. */

void pit_init_hz(uint32_t hz) {
  /* Frequency = 1193182 / divisor */
  if (hz == 0) hz = 100;
  uint32_t divisor = 1193182 / hz;
  if (divisor > 0xFFFF) divisor = 0xFFFF;
  
  hal_write8(PIT_CMD, 0x36); /* Channel 0, lobyte/hibyte, square wave */
  hal_write8(PIT_CH0, (uint8_t)(divisor & 0xFF));
  hal_write8(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));
  
  pr_info("PIT Initialized at %u Hz (divisor %u).\n", hz, (uint16_t)divisor);
}

void pic_send_eoi(uint8_t irq) {
  if (irq >= 8) {
    hal_write8(PIC2_CMD, 0x20);
  }
  hal_write8(PIC1_CMD, 0x20);
}

void pic_unmask(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if(irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = hal_read8(port) & ~(1 << irq);
    hal_write8(port, value);
}

/* Keyboard interrupt is now handled in idt.c or via generic IRQ dispatch */
