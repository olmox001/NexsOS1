/*
 * kernel/include/drivers/uart.h
 * UART driver interface
 */
#ifndef _DRIVERS_UART_H
#define _DRIVERS_UART_H

#include <kernel/types.h>

/* PL011 UART registers for QEMU virt machine */
#define UART0_BASE 0x09000000UL
#define UART0_IRQ 33

/* PL011 Register offsets */
#define UART_DR 0x00    /* Data Register */
#define UART_RSR 0x04   /* Receive Status Register */
#define UART_FR 0x18    /* Flag Register */
#define UART_ILPR 0x20  /* IrDA Low-Power Counter */
#define UART_IBRD 0x24  /* Integer Baud Rate */
#define UART_FBRD 0x28  /* Fractional Baud Rate */
#define UART_LCR_H 0x2C /* Line Control */
#define UART_CR 0x30    /* Control Register */
#define UART_IFLS 0x34  /* Interrupt FIFO Level Select */
#define UART_IMSC 0x38  /* Interrupt Mask Set/Clear */
#define UART_RIS 0x3C   /* Raw Interrupt Status */
#define UART_MIS 0x40   /* Masked Interrupt Status */
#define UART_ICR 0x44   /* Interrupt Clear */
#define UART_DMACR 0x48 /* DMA Control */

/* Flag Register bits */
#define UART_FR_TXFE BIT(7) /* TX FIFO Empty */
#define UART_FR_RXFF BIT(6) /* RX FIFO Full */
#define UART_FR_TXFF BIT(5) /* TX FIFO Full */
#define UART_FR_RXFE BIT(4) /* RX FIFO Empty */
#define UART_FR_BUSY BIT(3) /* UART Busy */

/* Control Register bits */
#define UART_CR_RXE BIT(9)    /* Receive Enable */
#define UART_CR_TXE BIT(8)    /* Transmit Enable */
#define UART_CR_UARTEN BIT(0) /* UART Enable */

/* Line Control bits */
#define UART_LCR_H_WLEN_8 (3 << 5) /* 8 bits */
#define UART_LCR_H_FEN BIT(4)      /* FIFO Enable */

/* Functions */
void uart_init(void);
void uart_putc(char c);
char uart_getc(void);
int uart_getc_nonblock(void);
void uart_puts(const char *s);
void uart_puthex(uint64_t val);

/* uart_putc_emergency - lock-free TX for fault context (see kernel/fault.h).
 * Identical output path to uart_putc but never takes a spinlock: usable from
 * an exception handler even when uart_lock is held by a wedged CPU.  May
 * interleave with concurrent normal output. */
void uart_putc_emergency(char c);

#endif /* _DRIVERS_UART_H */
