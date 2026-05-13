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

/* vsnprintf is now in vsnprintf.c */

/* Internal output function */
static void kputc(char c) { uart_putc(c); }

static void kputs(const char *s) {
  while (*s)
    kputc(*s++);
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
   * switch to another task, and that task's next printk sees a stale
   * in_printk=1 on this CPU → false "[RECURSIVE PRINTK DETECTED]" spam. */
  spin_lock_irqsave(&uart_lock, &flags);

  if (cpu->in_printk) {
    spin_unlock_irqrestore(&uart_lock, flags);
    uart_puts("\n[RECURSIVE PRINTK DETECTED]\n");
    return 0;
  }

  cpu->in_printk = 1;

  /* Prepend "[C%d] " CPU prefix for multi-CPU log disambiguation */
  int pfx = snprintf(cpu->printk_buf, 8, "[C%u] ", cpu->cpu_id);
  if (pfx < 0)
    pfx = 0;

  /* Format the actual message after the prefix */
  len = vsnprintf(cpu->printk_buf + pfx, sizeof(cpu->printk_buf) - pfx, fmt,
                  args);

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
  while (1) {
    arch_idle();
  }

  __builtin_unreachable();
}
