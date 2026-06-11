/*
 * kernel/lib/printk.c
 * Kernel printf implementation
 *
 * Purpose:
 *   Provides printk(), vprintk(), snprintf(), and panic() — the formatted output
 *   and fatal-error reporting interfaces for the entire kernel.  All kernel log
 *   output eventually flows through this file to the UART.
 *
 * Role:
 *   printk/vprintk are called from every subsystem (drivers, sched, mm, fs,
 *   irq, graphics).  snprintf is used for string formatting throughout the kernel.
 *   panic() is the last-resort halt path on unrecoverable errors.
 *
 * Architecture:
 *   - Formatted string production is delegated to vsnprintf() (vsnprintf.c).
 *   - Each CPU has a per-CPU buffer (cpu->printk_buf, 2048 bytes) to avoid
 *     sharing a global buffer across CPUs without a lock.
 *   - A global spinlock (uart_lock) serialises UART writes across all CPUs.
 *   - A per-CPU recursion guard (cpu->in_printk) detects recursive printk calls
 *     (e.g. from a fault handler that fires during printk).
 *   - panic() sets panic_flag atomically so other CPUs can halt via IPI, then
 *     prints and spins.
 *
 * Invariants:
 *   - uart_lock is always acquired with IRQs saved (spin_lock_irqsave) so that
 *     a timer IRQ cannot try to printk while uart_lock is already held.
 *   - in_printk is set AFTER the lock is taken to prevent a false-positive
 *     recursive-printk detection (see comment in vprintk for the exact race).
 *
 * Known issues:
 *   LIB-PRINTK-01  (W2 REFINE) cpu->printk_buf is 2048 bytes; the prefix
 *                  consumes 6 bytes.  Long messages are silently truncated by
 *                  vsnprintf with no dropped-message counter.
 */
#include <drivers/uart.h>
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/fault.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <stdarg.h>

/* console_loglevel: messages at or below this level are printed.
 * Initialized to KERN_INFO; may be adjusted at runtime. */
int console_loglevel = KERN_INFO;

/* Set by panic() to signal all CPUs to halt */
volatile int panic_flag = 0;

/* vsnprintf is now in vsnprintf.c */

/* kputc - write a single character to the UART.
 * Called only from kputs; not exported. */
static void kputc(char c) { uart_putc(c); }

/* kputs - write a NUL-terminated string to the UART byte by byte.
 * Called from vprintk after the formatted buffer is ready.
 * Not exported; callers outside this file use printk/vprintk. */
static void kputs(const char *s) {
  while (*s)
    kputc(*s++);
}

/*
 * snprintf - format a string into a fixed-size buffer (variadic wrapper).
 *
 * Delegates directly to vsnprintf().  Provided here so kernel code can call
 * snprintf() without linking against userland libc.
 *
 * Params:
 *   buf  - destination buffer of 'size' bytes.
 *   size - total capacity of buf (NUL included in the count).
 *   fmt  - printf-style format string.
 *   ...  - format arguments.
 * Returns: number of characters written (excluding NUL); may be < size - 1
 *          if the format string is shorter than the buffer.
 *          NOTE(LIB-VSNPRINTF-02): does NOT return the would-be length on
 *          truncation; callers cannot detect truncation from the return value.
 * Locking: none (vsnprintf is stateless).
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
 * vprintk - format and emit a log message from a va_list.
 *
 * This is the core printk implementation.  It:
 *   1. Acquires uart_lock with IRQ save BEFORE reading in_printk (see NOTE below).
 *   2. Checks for recursive printk; if detected, unlocks and emits a bare warning
 *      directly via uart_puts (which does not go through vprintk).
 *   3. Sets cpu->in_printk to block re-entry while the buffer is in use.
 *   4. Formats a "[C<id>] " prefix into cpu->printk_buf (6 bytes).
 *   5. Formats the message into cpu->printk_buf after the prefix using vsnprintf.
 *   6. Emits the buffer via kputs.
 *   7. Clears in_printk and releases uart_lock.
 *
 * Params:
 *   fmt  - printf-style format string.
 *   args - argument list; caller is responsible for va_start/va_end.
 * Returns: number of characters written by vsnprintf (message portion only;
 *          prefix length is not included).
 * Locking: acquires uart_lock with IRQ save/restore; NOT safe from NMI context.
 * Side effects: writes to uart; may be called from interrupt context (IRQ disabled
 *               on entry while lock is held).
 *
 * NOTE(LIB-PRINTK-01): cpu->printk_buf is 2048 bytes; the prefix consumes ~6.
 *   vsnprintf receives (2048 - pfx) bytes.  If the formatted message is longer,
 *   it is silently truncated at (2048 - pfx - 1) characters with no warning or
 *   counter increment.
 *
 * NOTE on in_printk ordering: uart_lock is acquired BEFORE checking in_printk.
 *   Without this ordering, the following SMP race can occur:
 *     CPU A: sets in_printk=1
 *     Timer IRQ fires, switches to task on CPU A
 *     CPU A (new task): reads in_printk=1 → false "[RECURSIVE PRINTK DETECTED]"
 *   By taking the lock first, only one CPU can inspect/modify in_printk at a time.
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
 * printk - kernel printf (variadic wrapper around vprintk).
 *
 * The primary log-output function for all kernel subsystems.  GCC checks
 * format/argument types at compile time via the __attribute__((format(printf,...)))
 * declared in printk.h.
 *
 * Params:
 *   fmt - printf-style format string.
 *   ... - format arguments.
 * Returns: characters written (from vsnprintf, message portion only).
 * Locking: inherits vprintk's uart_lock; NOT safe from NMI context.
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
 * panic - print a fatal message and halt all CPUs permanently.
 *
 * Sequence:
 *   1. Atomically increments panic_flag so other CPUs' spin loops can detect it.
 *   2. Sends an IPI (SGI0) to all other CPUs via irq_send_ipi_all(), which causes
 *      them to enter their halt handler.  This is done BEFORE printing so that no
 *      other CPU's printk can interleave with the panic message.
 *   3. Prints "*** KERNEL PANIC ***" banner.
 *   4. Formats and prints the caller-supplied message via vprintk.
 *   5. Prints "System halted."
 *   6. Disables all exceptions (arch_local_irq_save_all) and spins forever.
 *   7. Marks the spin as unreachable for the compiler.
 *
 * Params:
 *   fmt - printf-style format string describing the panic cause.
 *   ... - format arguments.
 * Returns: never.
 * Locking: calls printk/vprintk (which acquire uart_lock).  The function does
 *          NOT acquire uart_lock itself; if another CPU holds uart_lock when
 *          panic() is called, the first printk may spin briefly.
 * Side effects: sets panic_flag, sends IPI, halts all CPUs.
 */
void panic(const char *fmt, ...) {
  va_list args;

  /* Signal all CPUs to stop BEFORE printing so no interleaving after this */
  __sync_fetch_and_add(&panic_flag, 1);

  /* Fault context (kernel/fault.h): printk would take uart_lock (possibly
   * held by a wedged CPU) and needs get_cpu_info — on amd64 a LAPIC-MMIO read
   * that may itself fault.  Use the lock-free emergency path instead; print
   * FIRST, then attempt the quiesce IPI (its MMIO write may be the thing
   * that is broken). */
  if (fault_depth() > 0) {
    fault_printf("\n\n*** KERNEL PANIC (fault context) ***\n");
    va_start(args, fmt);
    fault_vprintf(fmt, args);
    va_end(args);
    fault_printf("\n");
    backtrace_here();
    fault_printf("\nSystem halted.\n");
    irq_send_ipi_all();

    uint64_t fflags;
    arch_local_irq_save_all(&fflags);
    while (1) {
      arch_idle();
    }
  }

  /* Send IPI (SGI0) to halt all other CPUs */
  irq_send_ipi_all();

  printk("\n\n*** KERNEL PANIC ***\n");

  va_start(args, fmt);
  vprintk(fmt, args);
  va_end(args);

  printk("\n");
  backtrace_here();
  printk("\nSystem halted.\n");

  /* Disable all exceptions and halt this CPU */
  uint64_t flags;
  arch_local_irq_save_all(&flags);
  while (1) {
    arch_idle();
  }

  __builtin_unreachable();
}
