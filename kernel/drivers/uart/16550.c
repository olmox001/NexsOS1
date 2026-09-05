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
 *   TX: uart_putc / uart_puts acquire uart_lock (spinlock + IRQ save) and
 *       busy-wait on LSR.THRE before writing to THR.  CR+LF expansion is
 *       done inside the locked path for whole-string atomicity on SMP.
 *
 *   RX: uart_getc busy-waits on LSR.DR (bit 0); uart_getc_nonblock polls it
 *       once.  Neither function holds a lock; safe only on single-core for
 *       concurrent readers (TX is independently locked).
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
 *   - All normal TX paths hold uart_lock; _uart_putc_unlocked is called only
 *     while uart_lock is held.  uart_putc_emergency is the sole lock-free TX.
 *
 * FIX(SMP-UART-01): previously TX had no lock ("safe on single-core QEMU,
 * a data race on SMP").  Under 4-core bring-up this produced character-level
 * interleaving on the serial console (e.g. "INIT-SIPI-SSIPI", "L[C0]",
 * Init/NXShell lines shredded).  Mirror the PL011 model: one global
 * uart_lock, whole-message holds in uart_puts, per-char hold in uart_putc.
 */
#include <arch/amd64_internal.h>
#include <drivers/uart.h>
#include <kernel/arch.h>
#include <kernel/io_poll.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

/* COM1_PORT: I/O base address of the first serial port (COM1) on x86 PC. */
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
#define LSR_DATA_READY 0x01
#define LSR_THRE 0x20 /* Transmit Holding Register Empty */

/*
 * uart_lock: spinlock protecting the TX path (THR writes).
 * Held with IRQ save/restore so that a printk from an IRQ handler does not
 * deadlock against a printk already in progress on the same CPU.
 * Non-static: printk.c and the PL011 driver share the same symbol name and
 * contract (one global TX lock for the console).
 */
DEFINE_SPINLOCK(uart_lock);

/*
 * uart_init - initialise the 16550A UART at COM1 (0x3F8).
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

  /* IRQs enabled, RTS/DSR set, OUT2 set (required for IRQ delivery) */
  outb(COM1_PORT + UART_MCR, 0x0B);

  /* Enable RX-data-available interrupt */
  outb(COM1_PORT + UART_IER, 0x01);
}

/*
 * _uart_putc_unlocked - transmit one character; caller must hold uart_lock.
 *
 * No CR+LF expansion here; uart_puts does that under the same lock so a
 * whole string (including inserted CRs) is atomic on the wire.
 */
static void _uart_putc_unlocked(char c) {
  /* Bounded: an absent/wedged UART must not hang printk — after the budget,
   * drop the byte instead of spinning forever (io_poll.h). */
  spin_until(inb(COM1_PORT + UART_LSR) & LSR_THRE, POLL_SPINS_DEFAULT);
  outb(COM1_PORT + UART_THR, (uint8_t)c);
}

/*
 * uart_putc - transmit one character (lock-protected).
 *
 * Locking: acquires uart_lock (spinlock + IRQ save/restore).
 * IRQ context: safe.
 */
void uart_putc(char c) {
  uint64_t flags;
  spin_lock_irqsave(&uart_lock, &flags);
  _uart_putc_unlocked(c);
  spin_unlock_irqrestore(&uart_lock, flags);
}

/*
 * uart_putc_emergency - fault-context TX (kernel/fault.h).
 *
 * Never takes uart_lock: usable from an exception handler even when the lock
 * is held by a wedged CPU.  May interleave with concurrent normal output.
 * Inserts CR before LF because fault_printf bypasses uart_puts expansion.
 */
void uart_putc_emergency(char c) {
  if (c == '\n') {
    spin_until(inb(COM1_PORT + UART_LSR) & LSR_THRE, POLL_SPINS_DEFAULT);
    outb(COM1_PORT + UART_THR, (uint8_t)'\r');
  }
  spin_until(inb(COM1_PORT + UART_LSR) & LSR_THRE, POLL_SPINS_DEFAULT);
  outb(COM1_PORT + UART_THR, (uint8_t)c);
}

/*
 * uart_puts - transmit a NUL-terminated string with CR+LF expansion.
 *
 * Acquires uart_lock ONCE for the entire string so concurrent printk/puts
 * from other CPUs cannot interleave characters mid-message (SMP-UART-01).
 *
 * Locking: acquires uart_lock (spinlock + IRQ save/restore).
 * IRQ context: safe.
 */
void uart_puts(const char *s) {
  uint64_t flags;
  spin_lock_irqsave(&uart_lock, &flags);
  while (*s) {
    if (*s == '\n')
      _uart_putc_unlocked('\r');
    _uart_putc_unlocked(*s++);
  }
  spin_unlock_irqrestore(&uart_lock, flags);
}

/*
 * uart_getc - receive one character (blocking, polled).
 *
 * Locking: none.
 * IRQ context: NO — calls arch_idle() (HLT).
 */
char uart_getc(void) {
  while ((inb(COM1_PORT + UART_LSR) & LSR_DATA_READY) == 0) {
    arch_idle();
  }
  return (char)inb(COM1_PORT + UART_RBR);
}

/*
 * uart_getc_nonblock - receive one character without blocking.
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
 * Locking: via uart_puts (acquires uart_lock for each of the two calls;
 *          early-boot only — acceptable).
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
