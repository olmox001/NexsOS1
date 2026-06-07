/*
 * kernel/arch/amd64/drivers/uart_16550.c
 * COM1 Serial Driver (16550 compatible) — amd64
 *
 * Drives the standard PC COM1 port (I/O base 0x3F8) using the 16550A UART
 * register set.  Provides polled TX and polled RX; no interrupt-driven RX
 * ring buffer (unlike the PL011 driver).  RX interrupts are enabled in the
 * UART (IER bit 0) but the amd64 IRQ dispatch path handles them via the IDT
 * common handler and irq_dispatch(), not through this driver's own handler.
 *
 * Architecture:
 *   TX: uart_putc busy-waits on LSR.THRE (bit 5) before writing to THR.
 *       uart_puts adds an explicit '\r' before each '\n' (CR+LF expansion).
 *       No spinlock is used on the TX path — safe on single-core QEMU, but
 *       a data race on SMP.
 *
 *   RX: uart_getc busy-waits on LSR.DR (bit 0); uart_getc_nonblock polls it
 *       once.  Neither function holds a lock; safe only on single-core.
 *
 * DLAB sequence (required to set baud divisor):
 *   1. Write 0x80 to LCR to set DLAB=1 (enables DLL/DLH at offsets 0/1).
 *   2. Write divisor low byte to DLL (0x00), high byte to DLH (0x01).
 *      Divisor = 1 → baud = 1843200 / (16 * 1) = 115200.
 *   3. Write line-control word (8N1 = 0x03) to LCR, clearing DLAB.
 *
 * Invariants:
 *   - uart_init() must be called once before any TX/RX operation.
 *   - I/O-port access via outb/inb requires no memory barrier on x86 because
 *     the processor serialises I/O instructions.
 *
 * Known issues:
 *   None from the 04-drivers-irq finding list apply specifically to this file;
 *   the absence of a lock on TX/RX is a latent SMP issue not tracked
 *   separately.
 */
#include <kernel/types.h>
#include <kernel/arch.h>
#include <arch/amd64_internal.h>
#include <drivers/uart.h>

/* COM1_PORT: I/O base address of the first serial port (COM1) on x86 PC. */
#define COM1_PORT 0x3F8

/* 16550 register offsets relative to COM1_PORT.
 * THR/RBR/DLL share offset 0; selection depends on DLAB bit in LCR and
 * whether the access is a read (RBR) or write (THR).  With DLAB=1 both
 * reads and writes at offset 0 access DLL. */
/* Register offsets */
#define UART_THR 0 /* Transmit Holding Register */
#define UART_RBR 0 /* Receive Buffer Register */
#define UART_IER 1 /* Interrupt Enable Register */
#define UART_FCR 2 /* FIFO Control Register */
#define UART_LCR 3 /* Line Control Register */
#define UART_MCR 4 /* Modem Control Register */
#define UART_LSR 5 /* Line Status Register */
#define UART_DLL 0 /* Divisor Latch Low (when DLAB=1) */
#define UART_DLH 1 /* Divisor Latch High (when DLAB=1) */

/* LSR_DATA_READY: LSR bit 0 — set when the RX FIFO / holding register has
 *                 at least one character available to read.
 * LSR_THRE:       LSR bit 5 — set when the TX holding register is empty;
 *                 safe to write a new byte to THR. */
/* LSR bits */
#define LSR_DATA_READY  0x01
#define LSR_THRE        0x20 /* Transmit Holding Register Empty */

/*
 * uart_init - initialise the 16550A UART at COM1 (0x3F8).
 *
 * Follows the standard DLAB-based initialisation sequence:
 *   1. IER = 0x00  — disable all UART interrupts while programming.
 *   2. LCR = 0x80  — set DLAB=1 to expose divisor latch registers.
 *   3. DLL = 0x01, DLH = 0x00 — divisor = 1 → 115200 baud at 1.8432 MHz.
 *   4. LCR = 0x03  — 8N1; clears DLAB so THR/RBR are accessible again.
 *   5. FCR = 0xC7  — enable FIFO, clear TX+RX FIFOs, 14-byte RX threshold.
 *   6. MCR = 0x0B  — DTR, RTS, OUT2 (required to enable IRQ delivery).
 *   7. IER = 0x01  — enable RX-data-available interrupt.
 *
 * Locking: none; called once from boot CPU before SMP.
 * IRQ context: NO.
 */
void uart_init(void) {
  /* Disable interrupts */
  outb(COM1_PORT + UART_IER, 0x00);

  /* Enable DLAB (set baud rate divisor) */
  outb(COM1_PORT + UART_LCR, 0x80);

  /* Set divisor: 115200 baud (divisor = 1) */
  outb(COM1_PORT + UART_DLL, 0x01);
  outb(COM1_PORT + UART_DLH, 0x00);

  /* 8 bits, no parity, one stop bit (8N1) */
  outb(COM1_PORT + UART_LCR, 0x03);

  /* Enable FIFO, clear them, 14-byte threshold */
  outb(COM1_PORT + UART_FCR, 0xC7);

  /* IRQs enabled, RTS/DSR set */
  outb(COM1_PORT + UART_MCR, 0x0B);

  /* Enable receive data available interrupt */
  outb(COM1_PORT + UART_IER, 0x01);
}

/*
 * uart_putc - transmit one character (polled, no lock).
 *
 * @c: character to transmit.
 *
 * Busy-waits on LSR.THRE before writing to THR.  No CR+LF expansion here;
 * uart_puts() performs expansion instead.
 *
 * Locking: none (unsafe on SMP).
 * IRQ context: technically safe on single-core QEMU; SMP-unsafe.
 */
void uart_putc(char c) {
  /* Wait for transmit buffer to be empty */
  while ((inb(COM1_PORT + UART_LSR) & LSR_THRE) == 0)
    ;
  outb(COM1_PORT + UART_THR, (uint8_t)c);
}

/*
 * uart_puts - transmit a NUL-terminated string with CR+LF expansion.
 *
 * @s: NUL-terminated string to transmit.
 *
 * Inserts '\r' before every '\n' so terminal emulators display correct line
 * breaks.  Each character is sent via uart_putc (polled).
 *
 * Locking: none (inherits no-lock from uart_putc).
 * IRQ context: same as uart_putc.
 */
void uart_puts(const char *s) {
  while (*s) {
    if (*s == '\n')
      uart_putc('\r');
    uart_putc(*s++);
  }
}

/*
 * uart_getc - receive one character (blocking, polled).
 *
 * Busy-waits on LSR.DR (bit 0) calling arch_idle() (HLT on x86) between
 * polls; reads and returns one byte from RBR once available.
 *
 * Returns: received character.
 *
 * Locking: none.
 * IRQ context: NO — calls arch_idle() (HLT); must not be called from an
 *              IRQ handler.
 */
char uart_getc(void) {
  /* Wait for data to be ready */
  while ((inb(COM1_PORT + UART_LSR) & LSR_DATA_READY) == 0) {
    arch_idle();
  }
  return (char)inb(COM1_PORT + UART_RBR);
}

/*
 * uart_getc_nonblock - receive one character without blocking.
 *
 * Checks LSR.DR once; if set reads and returns the byte from RBR, else
 * returns -1.
 *
 * Returns: received character cast to int, or -1 if no data available.
 *
 * Locking: none.
 * IRQ context: safe (no sleeping).
 */
int uart_getc_nonblock(void) {
  if (inb(COM1_PORT + UART_LSR) & LSR_DATA_READY) {
    return (int)inb(COM1_PORT + UART_RBR);
  }
  return -1;
}

/*
 * uart_puthex - transmit a 64-bit value in hexadecimal.
 *
 * @val: value to format and transmit.
 *
 * Formats @val as a fixed-width 16-digit hex string into a local stack
 * buffer and calls uart_puts().  Useful for early-boot diagnostics.
 *
 * Locking: none (via uart_puts).
 * IRQ context: same as uart_puts.
 */
void uart_puthex(uint64_t val) {
  static const char hex[] = "0123456789abcdef";
  char buf[17];
  int i;

  buf[16] = '\0';
  for (i = 15; i >= 0; i--) {
    buf[i] = hex[val & 0xF];
    val >>= 4;
  }

  uart_puts("0x");
  uart_puts(buf);
}
