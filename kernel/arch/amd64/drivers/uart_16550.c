/*
 * kernel/arch/amd64/drivers/uart_16550.c
 * COM1 Serial Driver (16550 compatible)
 *
 * Provides uart_init/uart_putc/uart_puts for serial console output.
 */
#include <kernel/types.h>
#include <kernel/arch.h>
#include <arch/amd64_internal.h>
#include <drivers/uart.h>

#define COM1_PORT 0x3F8

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

/* LSR bits */
#define LSR_DATA_READY  0x01
#define LSR_THRE        0x20 /* Transmit Holding Register Empty */

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

void uart_putc(char c) {
  /* Wait for transmit buffer to be empty */
  while ((inb(COM1_PORT + UART_LSR) & LSR_THRE) == 0)
    ;
  outb(COM1_PORT + UART_THR, (uint8_t)c);
}

void uart_puts(const char *s) {
  while (*s) {
    if (*s == '\n')
      uart_putc('\r');
    uart_putc(*s++);
  }
}

char uart_getc(void) {
  /* Wait for data to be ready */
  while ((inb(COM1_PORT + UART_LSR) & LSR_DATA_READY) == 0) {
    arch_idle();
  }
  return (char)inb(COM1_PORT + UART_RBR);
}

int uart_getc_nonblock(void) {
  if (inb(COM1_PORT + UART_LSR) & LSR_DATA_READY) {
    return (int)inb(COM1_PORT + UART_RBR);
  }
  return -1;
}

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
