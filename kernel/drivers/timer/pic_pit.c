/*
 * kernel/arch/amd64/drivers/pic_pit.c
 * Legacy 8259 PIC and 8253 PIT — amd64
 *
 * Initialises the 8259A Programmable Interrupt Controller (PIC) pair and the
 * 8253/8254 Programmable Interval Timer (PIT) for x86-64.  Also provides the
 * struct irq_chip implementation (pic_chip) that is registered as the active
 * chip on amd64 via pic_init().
 *
 * 8259 PIC pair overview:
 *   Two cascaded 8259A chips (PIC1 + PIC2) provide IRQ lines 0-15.  They are
 *   remapped in pic_init() to IDT vectors 32-47 (IRQ n → vector 32+n) to
 *   avoid collision with CPU exception vectors 0-31.  PIC1 handles IRQs 0-7;
 *   PIC2 handles IRQs 8-15 and cascades to PIC1 via IRQ2.
 *
 * 8253/8254 PIT overview:
 *   pit_init_hz() programs PIT Channel 0 in mode 3 (square wave) with
 *   divisor = 1193182 / hz.  The PIT output is connected to PIC1 IRQ0
 *   (vector 32).  On QEMU the LAPIC timer supersedes the PIT for SMP, but
 *   pic_send_eoi is still needed for PIC IRQ acknowledgement.
 *
 * pic_chip and the IRQ-01 design problem:
 *   pic_chip is registered as current_chip in irq.c via irq_register_chip().
 *   However, pic_chip_acknowledge() always returns 1023, which causes
 *   irq_handler() to exit immediately on the first iteration — making the
 *   generic dispatch loop a permanent no-op on amd64.  Actual IRQ dispatch
 *   is performed by irq_dispatch() called from the IDT common handler in
 *   idt.c, which also issues lapic_eoi() and pic_send_eoi() directly,
 *   bypassing chip->end() entirely.
 *
 * Invariants:
 *   - pic_init() must be called before any device IRQ is unmasked on amd64.
 *   - IRQ numbers passed to pic_chip_enable/disable are in the range 32-47
 *     (IDT vector space); the PIC hardware line = irq - 32.
 *
 * Known issues:
 *   IRQ-01  (W4 WRONG-DESIGN) pic_chip_acknowledge() returns 1023;
 *           irq_handler()'s generic chip loop is dead on amd64.  Actual
 *           dispatch bypasses the chip contract.  See file header above.
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

/* PIC command and data I/O ports.
 * PIC1 (master): CMD = 0x20, DATA = 0x21 (OCW1/ICW registers).
 * PIC2 (slave):  CMD = 0xA0, DATA = 0xA1. */
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

/* PIT (8253/8254) I/O ports.
 * PIT_CH0 (0x40): Channel 0 data port (read/write counter).
 * PIT_CMD (0x43): Mode/Command register (write-only). */
#define PIT_CH0   0x40
#define PIT_CMD   0x43

extern volatile uint64_t jiffies;
extern void timer_tick(void); /* generic scheduler tick */
void pit_init_hz(uint32_t hz);

void pic_unmask(uint8_t irq);
void pic_mask(uint8_t irq);

/*
 * pic_chip_enable - unmask a PIC IRQ line (via pic_unmask).
 *
 * @irq: IDT vector number (32-47 for PIC IRQs 0-15).
 *
 * Translates the IDT vector to a PIC hardware line (irq - 32) and clears
 * the corresponding mask bit in PIC1_DATA or PIC2_DATA.
 *
 * I/O side effects: reads then writes PIC1_DATA or PIC2_DATA.
 * Locking: none; called from irq_register() which is boot-path only.
 * IRQ context: safe on single-core.
 */
static void pic_chip_enable(uint32_t irq) {
    if (irq >= 32 && irq < 48) {
        pic_unmask(irq - 32);
    }
}

/*
 * pic_mask - mask (disable) a PIC hardware IRQ line.
 *
 * @irq: PIC hardware line (0-15; 0-7 for PIC1, 8-15 for PIC2).
 *
 * Sets the corresponding bit in the OCW1 (mask) register of PIC1_DATA (for
 * irq 0-7) or PIC2_DATA (for irq 8-15; hardware line = irq - 8).
 *
 * I/O side effects: hal_read8 + hal_write8 on PIC1_DATA or PIC2_DATA.
 * Locking: none.
 * IRQ context: safe.
 */
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

/*
 * pic_chip_disable - mask a PIC IRQ line (via pic_mask).
 *
 * @irq: IDT vector number (32-47).
 *
 * Translates to hardware line and calls pic_mask().
 *
 * I/O side effects: same as pic_mask.
 * Locking: none.
 * IRQ context: safe.
 */
static void pic_chip_disable(uint32_t irq) {
    if (irq >= 32 && irq < 48) {
        pic_mask(irq - 32);
    }
}

/*
 * pic_chip_acknowledge - chip->acknowledge() for the amd64 PIC path.
 *
 * Returns 1023 unconditionally.
 *
 * NOTE(IRQ-01): This is the root of the dead-irq_handler() problem on amd64.
 * irq_handler() treats 1023 as "no more interrupts" and exits immediately.
 * On x86, the IDT stub delivers the vector in pt_regs before even reaching
 * this function, so there is no IAR-equivalent read needed; but the chip
 * contract requires acknowledge() to return the vector, not a sentinel.
 * Returning 1023 permanently breaks the generic dispatch loop for x86.
 *
 * Locking: none.
 * IRQ context: safe (but irrelevant — this is never called in the hot path).
 */
static uint32_t pic_chip_acknowledge(void) {
    /* Not used by PIC on x86 because the vector is in pt_regs */
    return 1023;
}

/*
 * pic_chip_end - send End-Of-Interrupt to the PIC (chip->end for amd64).
 *
 * @irq: IDT vector number (32-47).
 *
 * Translates to hardware IRQ line and calls pic_send_eoi().  This would be
 * correct EOI path if irq_handler() were used on amd64, but per IRQ-01 it
 * is not.  The actual EOI on amd64 is issued by the IDT common handler.
 *
 * I/O side effects: same as pic_send_eoi.
 * Locking: none.
 * IRQ context: safe.
 */
static void pic_chip_end(uint32_t irq) {
    if (irq >= 32 && irq < 48) {
        pic_send_eoi(irq - 32);
    }
}

/* pic_chip: irq_chip implementation for the 8259A PIC pair.
 * .init is NULL because pic_init() itself calls irq_register_chip() and
 * then initialises the PIC; there is no separate init() callback needed.
 * NOTE(IRQ-01): .acknowledge always returns 1023 — see pic_chip_acknowledge. */
static struct irq_chip pic_chip = {
    .name = "8259 PIC",
    .init = NULL,
    .enable = pic_chip_enable,
    .disable = pic_chip_disable,
    .acknowledge = pic_chip_acknowledge,
    .end = pic_chip_end,
};

/*
 * pic_init - initialise the 8259A PIC pair and register pic_chip.
 *
 * Sends the four ICW (Initialization Command Word) sequences to both PICs:
 *   ICW1 (0x11 to CMD): start init, cascaded, ICW4 required.
 *   ICW2 (to DATA): vector offset — PIC1 → 32, PIC2 → 40.
 *   ICW3 (to DATA): PIC1 has slave on IRQ2 (0x04); PIC2 id=2 (0x02).
 *   ICW4 (0x01 to DATA): 8086 mode, normal EOI.
 * Then masks all IRQs except IRQ2 (cascade): PIC1_DATA=0xFB, PIC2_DATA=0xFF.
 *
 * Also registers pic_chip as the active irq_chip via irq_register_chip().
 *
 * I/O ports written: PIC1_CMD, PIC2_CMD, PIC1_DATA, PIC2_DATA.
 * Locking: none; called once before SMP.
 * IRQ context: NO.
 */
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

/*
 * pit_init_hz - programme the 8253/8254 PIT Channel 0 at @hz interrupts/sec.
 *
 * @hz: desired interrupt frequency (clamped to 100 if 0).
 *
 * PIT input clock = 1193182 Hz (derived from 14.318 MHz / 12).
 * Divisor = 1193182 / hz; clamped to 0xFFFF to avoid overflow.
 *
 * Programs PIT in mode 3 (square wave generator):
 *   PIT_CMD (0x43) = 0x36: channel 0, lo/hibyte access, mode 3, binary.
 *   PIT_CH0 (0x40): write divisor low byte, then high byte.
 *
 * I/O ports written: PIT_CMD (0x43), PIT_CH0 (0x40) x2.
 * Locking: none; called once during boot.
 * IRQ context: NO.
 */
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

/*
 * pic_send_eoi - send a non-specific End-Of-Interrupt command to the PIC(s).
 *
 * @irq: PIC hardware IRQ line (0-15).
 *
 * Sends OCW2 EOI byte (0x20) to PIC1_CMD.  For IRQs 8-15 (from PIC2), sends
 * the EOI to PIC2_CMD first, then to PIC1_CMD (cascade EOI required).
 *
 * Called by the amd64 IDT common handler (idt.c) after irq_dispatch(); also
 * reachable via pic_chip_end() but that path is dead (IRQ-01).
 *
 * I/O ports written: PIC2_CMD (0xA0) if irq>=8; PIC1_CMD (0x20) always.
 * Locking: none.
 * IRQ context: YES — called from IRQ handler (IDT path).
 */
void pic_send_eoi(uint8_t irq) {
  if (irq >= 8) {
    hal_write8(PIC2_CMD, 0x20);
  }
  hal_write8(PIC1_CMD, 0x20);
}

/*
 * pic_unmask - unmask (enable) a PIC hardware IRQ line.
 *
 * @irq: PIC hardware line (0-15).
 *
 * Reads the current OCW1 mask from PIC1_DATA or PIC2_DATA, clears the
 * target bit, and writes back.  IRQs 8-15 adjust the line to 0-7 for PIC2.
 *
 * I/O side effects: hal_read8 + hal_write8 on PIC1_DATA or PIC2_DATA.
 * Locking: none.
 * IRQ context: safe (no sleeping).
 */
void pic_unmask(uint8_t irq) {
    uint16_t port;
    uint8_t value;

    if(irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    pr_info("PIC: Unmasking IRQ %u\n", irq);
    value = hal_read8(port) & ~(1 << irq);
    hal_write8(port, value);
}

/* Keyboard interrupt is now handled in idt.c or via generic IRQ dispatch */
