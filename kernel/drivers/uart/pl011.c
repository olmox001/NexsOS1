/*
 * kernel/drivers/uart/pl011.c
 * PL011 UART driver for ARM QEMU virt machine
 */
#include <drivers/uart.h>
#include <kernel/arch.h>
#include <kernel/irq.h>
#include <kernel/platform.h>
#include <kernel/sched.h>
#include <kernel/types.h>
#include <stdint.h>

/* MMIO access macros */
#define UART_REG(offset) (*(volatile uint32_t *)(PLATFORM_UART_BASE + (offset)))

/* Ring Buffer */
#define RX_BUF_SIZE 128
static char rx_buf[RX_BUF_SIZE];
static volatile int rx_head = 0;
static volatile int rx_tail = 0;

/* Interrupt bits */
#define UART_IMSC_RXIM BIT(4)
#define UART_ICR_RXIC BIT(4)

static void uart_irq_handler(uint32_t irq, void *data) {
  (void)irq;
  (void)data;

  /* Check masked status */
  if (UART_REG(UART_MIS) & UART_IMSC_RXIM) {
    /* Read until FIFO empty or buffer full */
    while (!(UART_REG(UART_FR) & UART_FR_RXFE)) {
      char c = (char)(UART_REG(UART_DR) & 0xFF);

      int next = (rx_head + 1) % RX_BUF_SIZE;
      if (next != rx_tail) {
        rx_buf[rx_head] = c;
        rx_head = next;
      }
    }
    /* Clear interrupt */
    UART_REG(UART_ICR) = UART_ICR_RXIC;

    /* Wake up waiting processes */
    extern struct wait_queue_head keyboard_wait_queue;
    wake_up(&keyboard_wait_queue);
  }
}

/*
 * Initialize UART
 * QEMU's PL011 is pre-configured, but we set it up properly anyway
 */
void uart_init(void) {
  /* Disable UART */
  UART_REG(UART_CR) = 0;

  /* Clear pending interrupts */
  UART_REG(UART_ICR) = 0x7FF;

  /* Set baud rate (115200 @ 24MHz clock) */
  UART_REG(UART_IBRD) = 13;
  UART_REG(UART_FBRD) = 1;

  /* 8N1, enable FIFOs */
  UART_REG(UART_LCR_H) = UART_LCR_H_WLEN_8 | UART_LCR_H_FEN;

  /* Enable Interrupts (RX) */
  UART_REG(UART_IMSC) = UART_IMSC_RXIM;

  /* Register IRQ */
  irq_register(PLATFORM_IRQ_UART0, uart_irq_handler, NULL);

  /* Enable UART, TX and RX */
  UART_REG(UART_CR) = UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE;
}

DEFINE_SPINLOCK(uart_lock);

static void _uart_putc_unlocked(char c) {
  /* Wait until TX FIFO is not full */
  while (UART_REG(UART_FR) & UART_FR_TXFF)
    ;

  UART_REG(UART_DR) = c;

  /* Add carriage return for newline */
  if (c == '\n') {
    while (UART_REG(UART_FR) & UART_FR_TXFF)
      ;
    UART_REG(UART_DR) = '\r';
  }
}

/*
 * Send a character
 */
void uart_putc(char c) {
  uint64_t flags;
  spin_lock_irqsave(&uart_lock, &flags);
  _uart_putc_unlocked(c);
  spin_unlock_irqrestore(&uart_lock, flags);
}

/*
 * Receive a character (blocking)
 */
char uart_getc(void) {
  /* Wait for data in buffer */
  while (rx_head == rx_tail) {
    /* Wait for interrupt (wfi) to save power? */
    /* Or just busy wait for now */
    arch_idle();
  }

  char c = rx_buf[rx_tail];
  rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
  return c;
}

/*
 * Receive a character (non-blocking)
 * Returns -1 if no character available
 */
int uart_getc_nonblock(void) {
  if (rx_head == rx_tail)
    return -1;

  char c = rx_buf[rx_tail];
  rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
  return (int)c;
}

/*
 * Send a string
 */
void uart_puts(const char *s) {
  uint64_t flags;
  spin_lock_irqsave(&uart_lock, &flags);
  while (*s)
    _uart_putc_unlocked(*s++);
  spin_unlock_irqrestore(&uart_lock, flags);
}

/*
 * Print a hexadecimal number
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

