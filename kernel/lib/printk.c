/*
 * kernel/lib/printk.c
 * Kernel printf implementation
 */
#include <drivers/uart.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <stdarg.h>

int console_loglevel = KERN_INFO;

/* Set by panic() to signal all CPUs to halt */
volatile int panic_flag = 0;

/* Internal output function */
static void kputc(char c) { uart_putc(c); }

static void kputs(const char *s) {
  while (*s)
    kputc(*s++);
}

/* Number conversion */
static int print_num(char *buf, size_t size, uint64_t num, int base, int width,
                     int flags) {
#define FLAG_ZEROPAD 0x01
#define FLAG_SIGN 0x02
#define FLAG_PLUS 0x04
#define FLAG_SPACE 0x08
#define FLAG_LEFT 0x10
#define FLAG_SPECIAL 0x20
#define FLAG_UPPERCASE 0x40

  static const char digits_lower[] = "0123456789abcdef";
  static const char digits_upper[] = "0123456789ABCDEF";
  const char *digits = (flags & FLAG_UPPERCASE) ? digits_upper : digits_lower;

  char tmp[66];
  int i = 0;
  int written = 0;

  if (num == 0) {
    tmp[i++] = '0';
  } else {
    while (num != 0) {
      tmp[i++] = digits[num % base];
      num /= base;
    }
  }

  /* Padding */
  while (i < width && written < (int)size - 1) {
    buf[written++] = (flags & FLAG_ZEROPAD) ? '0' : ' ';
    width--;
  }

  /* Digits (reversed) */
  while (i > 0 && written < (int)size - 1) {
    buf[written++] = tmp[--i];
  }

  return written;
}

/*
 * vsnprintf - format string to buffer
 */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
  int written = 0;
  int width;
  int flags;
  int64_t num;
  uint64_t unum;
  const char *s;
  char c;

  if (size == 0)
    return 0;

  while (*fmt && written < (int)size - 1) {
    if (*fmt != '%') {
      buf[written++] = *fmt++;
      continue;
    }

    fmt++; /* Skip '%' */

    /* Parse flags */
    flags = 0;
    width = 0;

    /* Zero padding */
    if (*fmt == '0') {
      flags |= FLAG_ZEROPAD;
      fmt++;
    }

    /* Width */
    while (*fmt >= '0' && *fmt <= '9') {
      width = width * 10 + (*fmt - '0');
      fmt++;
    }

    /* Length modifiers */
    int is_long = 0;
    int is_longlong = 0;
    if (*fmt == 'l') {
      fmt++;
      is_long = 1;
      if (*fmt == 'l') {
        fmt++;
        is_longlong = 1;
      }
    }

    /* Conversion specifier */
    switch (*fmt) {
    case 'c':
      c = (char)va_arg(args, int);
      buf[written++] = c;
      break;

    case 's':
      s = va_arg(args, const char *);
      if (!s)
        s = "(null)";
      while (*s && written < (int)size - 1)
        buf[written++] = *s++;
      break;

    case 'd':
    case 'i':
      if (is_longlong)
        num = va_arg(args, int64_t);
      else if (is_long)
        num = va_arg(args, long);
      else
        num = va_arg(args, int);

      if (num < 0) {
        buf[written++] = '-';
        unum = -num;
      } else {
        unum = num;
      }
      written +=
          print_num(buf + written, size - written, unum, 10, width, flags);
      break;

    case 'u':
      if (is_longlong)
        unum = va_arg(args, uint64_t);
      else if (is_long)
        unum = va_arg(args, unsigned long);
      else
        unum = va_arg(args, unsigned int);
      written +=
          print_num(buf + written, size - written, unum, 10, width, flags);
      break;

    case 'x':
      if (is_longlong)
        unum = va_arg(args, uint64_t);
      else if (is_long)
        unum = va_arg(args, unsigned long);
      else
        unum = va_arg(args, unsigned int);
      written +=
          print_num(buf + written, size - written, unum, 16, width, flags);
      break;

    case 'X':
      if (is_longlong)
        unum = va_arg(args, uint64_t);
      else if (is_long)
        unum = va_arg(args, unsigned long);
      else
        unum = va_arg(args, unsigned int);
      flags |= FLAG_UPPERCASE;
      written +=
          print_num(buf + written, size - written, unum, 16, width, flags);
      break;

    case 'p':
      unum = (uint64_t)va_arg(args, void *);
      buf[written++] = '0';
      if (written < (int)size - 1)
        buf[written++] = 'x';
      written +=
          print_num(buf + written, size - written, unum, 16, 16, FLAG_ZEROPAD);
      break;

    case '%':
      buf[written++] = '%';
      break;

    default:
      buf[written++] = '%';
      if (written < (int)size - 1)
        buf[written++] = *fmt;
      break;
    }

    fmt++;
  }

  buf[written] = '\0';
  return written;
}

/*
 * snprintf - format string to buffer
 */
int snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list args;
  int ret;

  va_start(args, fmt);
  ret = vsnprintf(buf, size, fmt, args);
  va_end(args);

  return ret;
}

/*
 * vprintk - print to console from va_list
 */
/* Global UART lock for synchronized serial output */
static spinlock_t uart_lock = SPINLOCK_INIT;

int vprintk(const char *fmt, va_list args) {
  struct cpu_info *cpu = get_cpu_info();
  uint64_t flags;
  int len;

  /* Acquire lock and disable IRQs BEFORE setting in_printk.
   * Without this, the timer IRQ can preempt between in_printk=1 and the lock,
   * switch to another task, and that task's next printk sees a stale in_printk=1
   * on this CPU → false "[RECURSIVE PRINTK DETECTED]" spam. */
  spin_lock_irqsave(&uart_lock, &flags);

  if (cpu->in_printk) {
    spin_unlock_irqrestore(&uart_lock, flags);
    uart_puts("\n[RECURSIVE PRINTK DETECTED]\n");
    return 0;
  }

  cpu->in_printk = 1;

  /* Prepend "[C%d] " CPU prefix for multi-CPU log disambiguation */
  int pfx = snprintf(cpu->printk_buf, 8, "[C%u] ", cpu->cpu_id);
  if (pfx < 0) pfx = 0;

  /* Format the actual message after the prefix */
  len = vsnprintf(cpu->printk_buf + pfx,
                  sizeof(cpu->printk_buf) - pfx, fmt, args);

  kputs(cpu->printk_buf);

  cpu->in_printk = 0;
  spin_unlock_irqrestore(&uart_lock, flags);

  return len;
}

/*
 * printk - kernel printf
 */
int printk(const char *fmt, ...) {
  va_list args;
  int ret;

  va_start(args, fmt);
  ret = vprintk(fmt, args);
  va_end(args);

  return ret;
}

/*
 * panic - halt ALL CPUs with message
 */
void panic(const char *fmt, ...) {
  va_list args;

  /* Signal all CPUs to stop BEFORE printing so no interleaving after this */
  __sync_fetch_and_add(&panic_flag, 1);

  /* Send IPI (SGI0) to halt all other CPUs */
  irq_send_ipi_all();

  printk("\n\n*** KERNEL PANIC ***\n");

  va_start(args, fmt);
  vprintk(fmt, args);
  va_end(args);

  printk("\n\nSystem halted.\n");

  /* Disable all exceptions and halt this CPU */
  uint64_t flags;
  arch_local_irq_save_all(&flags);
  while (1) { arch_idle(); }

  __builtin_unreachable();
}
