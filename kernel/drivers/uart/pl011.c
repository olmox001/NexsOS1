/*
 * kernel/drivers/uart/pl011.c
 * PL011 UART Driver — ARM QEMU virt machine (aarch64)
 *
 * Drives the ARM PrimeCell UART (PL011) at PLATFORM_UART_BASE (typically
 * 0x09000000 on the QEMU virt board).  Provides both polled TX output and
 * interrupt-driven RX into a 128-byte software ring buffer.
 *
 * Architecture:
 *   TX: uart_putc / uart_puts acquire uart_lock (spinlock + IRQ save) and
 *       busy-wait on UART_FR.TXFF before writing to UART_DR.  Newlines are
 *       expanded to CR+LF by _uart_putc_unlocked.
 *
 *   RX: uart_irq_handler (IRQ context) is registered for PLATFORM_IRQ_UART0
 *       (typically GIC IRQ 33 on QEMU virt).  On each RX interrupt the FIFO
 *       is drained into rx_buf[] via a head pointer and the process wait queue
 *       is woken.  Consumers read via uart_getc (blocking) or
 *       uart_getc_nonblock (polling).
 *
 * Baud rate: 115200 at assumed 24 MHz UART reference clock.
 *   IBRD = 13, FBRD = 1  (13.020... -> close to 13.020833 for 115200).
 *
 * Invariants:
 *   - uart_init() must be called before any TX or RX operation.
 *   - UART_CR must be cleared (UART disabled) before changing IBRD/FBRD/LCR_H.
 *   - All TX paths hold uart_lock; _uart_putc_unlocked is called only while
 *     uart_lock is held.
 *
 * Known issues:
 *   DRV-UART-01  (W3 BUG/SECURITY) uart_getc and uart_getc_nonblock read
 *                rx_tail without a lock; uart_irq_handler writes rx_buf[] and
 *                rx_head without a lock.  On SMP the aarch64 weak memory model
 *                requires at minimum a dmb/stlr to order the rx_buf store
 *                before the rx_head store and to order the rx_head load before
 *                the rx_buf load on the consumer side.  Two concurrent readers
 *                also corrupt rx_tail.
 */
#include <drivers/uart.h>
#include <kernel/arch.h>
#include <kernel/irq.h>
#include <kernel/memlayout.h>
#include <kernel/platform.h>
#include <kernel/sched.h>
#include <kernel/types.h>
#include <stdint.h>

/* UART_REG(offset): dereference a 32-bit PL011 MMIO register at
 * PLATFORM_UART_BASE + offset (a physical address) through its direct-map
 * kernel VA (phys_to_virt; identity while KERNEL_VIRT_BASE == 0).  All
 * PL011 registers are 32-bit wide with the upper bits reserved. */
/* MMIO access macros */
#define UART_REG(offset) (*(volatile uint32_t *)phys_to_virt(PLATFORM_UART_BASE + (offset)))

/* rx_buf[]: circular ring buffer for characters received via IRQ.
 * rx_head: write index, advanced by uart_irq_handler (IRQ context).
 * rx_tail: read index, advanced by uart_getc / uart_getc_nonblock (any ctx).
 * Buffer is full when (rx_head + 1) % RX_BUF_SIZE == rx_tail.
 * NOTE(DRV-UART-01): both indices are volatile but not protected by a lock;
 * this is unsafe on SMP (see file header). */
/* Ring Buffer */
#define RX_BUF_SIZE 128
static char rx_buf[RX_BUF_SIZE];
static volatile int rx_head = 0;
static volatile int rx_tail = 0;

/* UART_IMSC_RXIM: Receive Interrupt Mask bit in UART_IMSC (offset 0x038).
 *   Bit 4 = RXIM: assert interrupt when RX FIFO reaches trigger level.
 * UART_ICR_RXIC: Receive interrupt clear bit in UART_ICR (offset 0x044).
 *   Writing bit 4 clears the masked RX interrupt in UART_MIS. */
/* Interrupt bits */
#define UART_IMSC_RXIM BIT(4)
#define UART_ICR_RXIC BIT(4)

/*
 * uart_irq_handler - PL011 RX interrupt service routine.
 *
 * @irq:  IRQ number (ignored; registered for PLATFORM_IRQ_UART0 only).
 * @data: opaque data (unused; registered as NULL).
 *
 * Called from irq_handler() when the GIC delivers PLATFORM_IRQ_UART0.
 * Checks UART_MIS (masked interrupt status, offset 0x040) for the RX bit;
 * drains the RX FIFO into rx_buf[] until UART_FR.RXFE (FIFO empty, bit 4)
 * is set or the ring buffer is full; clears the interrupt via UART_ICR;
 * wakes up keyboard_wait_queue.
 *
 * MMIO registers touched:
 *   UART_MIS  (0x040) read  — masked interrupt status.
 *   UART_FR   (0x018) read  — flags: RXFE = bit 4.
 *   UART_DR   (0x000) read  — data register; reading drains one byte from FIFO.
 *   UART_ICR  (0x044) write — interrupt clear register; write 1 to clear.
 *
 * Locking: none (see NOTE DRV-UART-01).  Called from IRQ context; must not
 *          sleep or acquire a sleeping lock.
 * IRQ context: YES — this is an IRQ handler registered with irq_register().
 *
 * NOTE(DRV-UART-01): rx_head and rx_buf[] are written here without a lock.
 * On SMP, concurrent readers of rx_tail (uart_getc / uart_getc_nonblock) and
 * a concurrent second IRQ handler invocation (if IRQ nesting were enabled)
 * would produce a data race.
 */
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
 * uart_init - configure and enable the PL011 UART.
 *
 * Sequence (PL011 TRM §3.3.6 — must disable UART before reprogramming):
 *   1. Clear UART_CR (disable UART, TX, RX) — required before changing line
 *      control or baud-rate registers.
 *   2. Write 0x7FF to UART_ICR to clear all pending interrupts.
 *   3. Program baud rate: IBRD=13, FBRD=1 for ~115200 at 24 MHz clock.
 *        Baud = clock / (16 * (IBRD + FBRD/64)) = 24e6 / (16*13.015625) ≈ 115273.
 *   4. Set LCR_H: 8 data bits (WLEN=11b), FIFO enable (FEN=1).
 *   5. Set IMSC: enable RX interrupt (RXIM=1); TX interrupt stays masked.
 *   6. Register uart_irq_handler for PLATFORM_IRQ_UART0 via irq_register().
 *   7. Enable UART, TX, RX via UART_CR (UARTEN=1, TXE=1, RXE=1).
 *
 * MMIO registers written:
 *   UART_CR    (0x030): control register.
 *   UART_ICR   (0x044): interrupt clear register.
 *   UART_IBRD  (0x024): integer baud-rate divisor.
 *   UART_FBRD  (0x028): fractional baud-rate divisor.
 *   UART_LCR_H (0x02C): line control register.
 *   UART_IMSC  (0x038): interrupt mask set/clear register.
 *
 * Locking: none; called once from boot CPU before SMP.
 * IRQ context: NO — called from early boot.
 */
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

/* uart_lock: spinlock protecting the TX path (UART_DR writes).
 * Held with IRQ save/restore so that a printk from an IRQ handler does not
 * deadlock against a printk already in progress on the same CPU. */
DEFINE_SPINLOCK(uart_lock);

/*
 * _uart_putc_unlocked - transmit one character; caller must hold uart_lock.
 *
 * @c: character to send.
 *
 * Busy-waits on UART_FR.TXFF (TX FIFO full, bit 5) before writing to
 * UART_DR (offset 0x000).  Expands '\n' to '\n'+'\r' for terminal emulators.
 *
 * MMIO registers touched:
 *   UART_FR (0x018) read  — flags; TXFF = bit 5.
 *   UART_DR (0x000) write — data register; writes one byte to TX FIFO.
 *
 * Locking: caller MUST hold uart_lock.
 * IRQ context: safe (called under uart_lock with IRQs saved).
 */
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
 * uart_putc - transmit one character (lock-protected).
 *
 * @c: character to transmit.
 *
 * Acquires uart_lock with IRQ save, delegates to _uart_putc_unlocked, then
 * releases the lock.  Safe to call from any context including IRQ handlers.
 *
 * Locking: acquires uart_lock (spinlock + IRQ save/restore).
 * IRQ context: safe.
 */
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
 * uart_putc_emergency - lock-free TX for fault context (kernel/fault.h).
 *
 * Bypasses uart_lock by design: a fault handler must never block on a lock
 * that another (possibly wedged) CPU holds.  Interleaving with concurrent
 * normal output is the accepted trade for guaranteed progress.
 */
void uart_putc_emergency(char c) { _uart_putc_unlocked(c); }

/*
 * uart_getc - receive one character (blocking).
 *
 * Spins calling arch_idle() (WFI) until rx_head != rx_tail, then reads one
 * byte from rx_buf[] and advances rx_tail.
 *
 * Returns: the received character.
 *
 * Locking: none (see NOTE DRV-UART-01); rx_tail modified without a lock.
 * IRQ context: NO — may sleep (WFI); must not be called from IRQ context.
 *
 * NOTE(DRV-UART-01): rx_tail is read and written without a lock; a concurrent
 * uart_getc() on another CPU would corrupt rx_tail.
 */
/*
 * Receive a character (blocking)
 */
char uart_getc(void) {
  /* Wait for data in buffer */
  while (rx_head == rx_tail) {
    /* Wait for interrupt (wfi/idle) to save power */
    arch_idle();
  }

  char c = rx_buf[rx_tail];
  rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
  return c;
}

/*
 * uart_getc_nonblock - receive one character without blocking.
 *
 * Returns the next character from the RX ring buffer, or -1 if the buffer
 * is empty (rx_head == rx_tail).
 *
 * Locking: none (see NOTE DRV-UART-01).
 * IRQ context: technically callable from IRQ context but unsafe on SMP due
 *              to unprotected rx_tail access (DRV-UART-01).
 */
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
 * uart_puts - transmit a NUL-terminated string (lock-protected).
 *
 * @s: NUL-terminated string to transmit.
 *
 * Acquires uart_lock once for the entire string, calling
 * _uart_putc_unlocked() per character.  More efficient than repeated
 * uart_putc() calls for multi-character output.
 *
 * Locking: acquires uart_lock (spinlock + IRQ save/restore).
 * IRQ context: safe.
 */
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
 * uart_puthex - transmit a 64-bit value in hexadecimal.
 *
 * @val: value to print.
 *
 * Formats @val as a fixed-width 16-digit hex string in a local buffer, then
 * calls uart_puts() (which acquires uart_lock).  Useful for early-boot
 * debugging before printf-style formatting is available.
 *
 * Locking: via uart_puts (acquires uart_lock).
 * IRQ context: safe.
 */
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

