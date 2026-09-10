/*
 * kernel/lib/printk.c
 * Kernel printf implementation
 *
 * Purpose:
 *   Provides printk(), vprintk(), snprintf(), and panic() — the formatted
 * output and fatal-error reporting interfaces for the entire kernel.  All
 * kernel log output eventually flows through this file to the UART.
 *
 * Role:
 *   printk/vprintk are called from every subsystem (drivers, sched, mm, fs,
 *   irq, graphics).  snprintf is used for string formatting throughout the
 * kernel. panic() is the last-resort halt path on unrecoverable errors.
 *
 * Architecture:
 *   - Formatted string production is delegated to vsnprintf() (vsnprintf.c).
 *   - Each CPU has a per-CPU buffer (cpu->printk_buf, 2048 bytes) to avoid
 *     sharing a global buffer across CPUs without a lock.
 *   - printk_lock serialises in_printk + buffer fill across CPUs.
 *   - The DRIVER's uart_lock (inside uart_puts) serialises the actual TX of
 *     a whole message — one uart_puts call per vprintk, so SMP cannot
 *     interleave characters mid-line (FIX SMP-UART-01).
 *   - A per-CPU recursion guard (cpu->in_printk) detects recursive printk calls
 *     (e.g. from a fault handler that fires during printk).
 *   - panic() sets panic_flag atomically so other CPUs can halt via IPI, then
 *     prints and spins.
 *
 * Invariants:
 *   - printk_lock is always acquired with IRQs saved (spin_lock_irqsave) so
 *     that a timer IRQ cannot re-enter vprintk while in_printk is being
 *     updated.
 *   - in_printk is set AFTER the lock is taken to prevent a false-positive
 *     recursive-printk detection (see comment in vprintk for the exact race).
 *   - TX goes through uart_puts() only (never per-char uart_putc from here),
 *     so the driver's uart_lock covers the entire formatted line.
 *
 * Known issues:
 *   LIB-PRINTK-01  (W2 REFINE) cpu->printk_buf is 2048 bytes; the prefix
 *                  consumes 6 bytes.  Long messages are silently truncated by
 *                  vsnprintf with no dropped-message counter.
 */
#include <drivers/uart.h>
#include <kernel/arch.h>
#include <kernel/bootphase.h>
#include <kernel/cpu.h>
#include <kernel/fault.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/types.h>
#include <stdarg.h>

/* console_loglevel: messages at or below this level are printed.
 * Initialized to KERN_INFO; may be adjusted at runtime. */
int console_loglevel = KERN_INFO;

/* Set by panic() to signal all CPUs to halt */
volatile int panic_flag = 0;

/* vsnprintf is now in vsnprintf.c */

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
 *   1. Acquires printk_lock with IRQ save BEFORE reading in_printk.
 *   2. Checks for recursive printk; if detected, unlocks and emits a bare
 *      warning via uart_puts (does not go through vprintk).
 *   3. Sets cpu->in_printk to block re-entry while the buffer is in use.
 *   4. Formats a "[C<id>] " prefix into cpu->printk_buf.
 *   5. Formats the message into cpu->printk_buf after the prefix.
 *   6. Emits the buffer with a SINGLE uart_puts() so the driver's uart_lock
 *      covers the entire line (FIX SMP-UART-01: no per-char interleaving).
 *   7. Clears in_printk and releases printk_lock.
 *
 * Params:
 *   fmt  - printf-style format string.
 *   args - argument list; caller is responsible for va_start/va_end.
 * Returns: number of characters written by vsnprintf (message portion only;
 *          prefix length is not included).
 * Locking: acquires printk_lock with IRQ save/restore; TX serialised by the
 *          driver's uart_lock inside uart_puts.  NOT safe from NMI context.
 *
 * NOTE(LIB-PRINTK-01): cpu->printk_buf is 2048 bytes; the prefix consumes ~6.
 *   vsnprintf receives (2048 - pfx) bytes.  If the formatted message is longer,
 *   it is silently truncated at (2048 - pfx - 1) characters.
 *
 * NOTE on in_printk ordering: printk_lock is acquired BEFORE checking
 *   in_printk.  Without this ordering:
 *     CPU A: sets in_printk=1
 *     Timer IRQ fires, switches to task on CPU A
 *     CPU A (new task): reads in_printk=1 → false "[RECURSIVE PRINTK DETECTED]"
 *   By taking the lock first, only one context can inspect/modify in_printk
 *   at a time on this path.
 */
/* Serialises in_printk + buffer fill.  TX is serialised by driver's uart_lock.
 */
static spinlock_t printk_lock = SPINLOCK_INIT;

int vprintk(const char *fmt, va_list args) {
  struct cpu_info *cpu = get_cpu_info();
  uint64_t flags;
  int len;

  /* Early boot: no per-CPU info yet.  Format to a stack buffer and emit
   * through uart_puts (driver lock = whole-message atomicity). */
  if (!cpu) {
    char buf[512];
    len = vsnprintf(buf, sizeof(buf), fmt, args);
    uart_puts("[BOOT] ");
    uart_puts(buf);
    return len;
  }

  /* Acquire lock and disable IRQs BEFORE setting in_printk. */
  spin_lock_irqsave(&printk_lock, &flags);

  if (cpu->in_printk) {
    spin_unlock_irqrestore(&printk_lock, flags);
    uart_puts("\n[RECURSIVE PRINTK DETECTED]\n");
    return 0;
  }

  cpu->in_printk = 1;

  /* Prepend "[C%u] " CPU prefix for multi-CPU log disambiguation */
  int pfx = snprintf(cpu->printk_buf, 8, "[C%u] ", cpu->cpu_id);
  if (pfx < 0)
    pfx = 0;

  /* Format the actual message after the prefix */
  len = vsnprintf(cpu->printk_buf + pfx, sizeof(cpu->printk_buf) - pfx, fmt,
                  args);

  if (len >= (int)(sizeof(cpu->printk_buf) - pfx - 1)) {
    cpu->printk_dropped++;
    int t_len = 11;
    if (sizeof(cpu->printk_buf) > (size_t)t_len) {
      char *end = cpu->printk_buf + sizeof(cpu->printk_buf) - t_len - 1;
      memcpy(end, "...[TRUNC]\n", 12);
    }
  }

  /* One uart_puts for the whole line — driver's uart_lock is held for the
   * entire string, so other CPUs cannot interleave characters mid-message. */
  uart_puts(cpu->printk_buf);

  cpu->in_printk = 0;
  spin_unlock_irqrestore(&printk_lock, flags);

  return len;
}

/*
 * printk - kernel printf (variadic wrapper around vprintk).
 *
 * The primary log-output function for all kernel subsystems.  GCC checks
 * format/argument types at compile time via the
 * __attribute__((format(printf,...))) declared in printk.h.
 *
 * Params:
 *   fmt - printf-style format string.
 *   ... - format arguments.
 * Returns: characters written (from vsnprintf, message portion only).
 * Locking: inherits vprintk's printk_lock + driver uart_lock; NOT safe from NMI
 * context.
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
 *   1. Atomically increments panic_flag so other CPUs' spin loops can detect
 * it.
 *   2. Sends an IPI (SGI0) to all other CPUs via irq_send_ipi_all(), which
 * causes them to enter their halt handler.  This is done BEFORE printing so
 * that no other CPU's printk can interleave with the panic message.
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
 * Locking: calls printk/vprintk (which acquire printk_lock).  The function does
 *          NOT acquire printk_lock itself; if another CPU holds printk_lock
 * when panic() is called, the first printk may spin briefly. Side effects: sets
 * panic_flag, sends IPI, halts all CPUs.
 */
/* panic_reboot_after_grace - DIR-05 #139 watchdog: after the panic is shown
 * (UART
 * + red panic_screen), wait a readable grace (~10 s) then HARD-RESET so an
 * unattended machine self-recovers from a kernel fault.  IRQs stay masked; the
 * grace spins on the free-running hardware counter (it advances without
 * interrupts) — NOT arch_idle(), which would wfi/hlt forever with IRQs off.  If
 * the timer frequency is not up yet (very early boot) the grace is skipped and
 * we reset immediately.  arch_reboot() is __noreturn (resets, or halts if reset
 * does not take). */
static void panic_reboot_after_grace(void) __noreturn;
static void panic_reboot_after_grace(void) {
  uint64_t f;
  arch_local_irq_save_all(&f);
  uint64_t freq = arch_timer_get_freq();
  if (freq) {
    uint64_t end = arch_timer_get_count() + freq * 10ULL; /* ~10 seconds */
    while ((int64_t)(end - arch_timer_get_count()) > 0)
      __asm__ __volatile__("" ::: "memory");
  }
  arch_reboot();
}

void panic(const char *fmt, ...) {
  va_list args;

  /* Signal all CPUs to stop BEFORE printing so no interleaving after this */
  __sync_fetch_and_add(&panic_flag, 1);

  /* FIX(PANIC-LOCKFREE-01): panic() used to split into two paths — a
   * lock-free one (fault_printf/fault_vprintf) when reached from inside a
   * CPU exception handler, and an ordinary printk()/vprintk() path
   * otherwise, on the reasoning that only the fault-handler case needed to
   * survive a wedged CPU holding printk's locks.
   *
   * That reasoning does not hold: printk_lock and the driver's uart_lock
   * are global, cross-CPU locks (kernel/lib/printk.c, kernel/drivers/uart/
   * 16550.c). ANY CPU stuck while holding either — which is precisely the
   * failure mode this function exists to report, whatever ordinary
   * (non-exception) code path detects it and calls panic() — would block
   * the "non-fault" branch forever inside printk()/vprintk(), turning what
   * should have been a panic banner into a totally silent, undiagnosable
   * full-system hang: the exact symptom this driver/scheduler audit was
   * asked to rule out. fault_printf/fault_vprintf (kernel/fault.h) are
   * lock-free by contract for exactly this reason; panic() now always uses
   * them, regardless of whether it was reached from a CPU exception. Print
   * BEFORE the cross-CPU quiesce IPI (matching the former fault-context
   * ordering) so the banner still gets out even if the IPI mechanism
   * itself is what is broken; the accepted trade is that another CPU's
   * legitimate printk() output may interleave with it on the wire (see
   * fault.h), which is strictly better than no output at all. */
  fault_printf("\n\n*** KERNEL PANIC ***\n");
  fault_printf("[boot-phase: %s]\n", boot_phase_name(boot_phase_get()));
  va_start(args, fmt);
  fault_vprintf(fmt, args);
  va_end(args);
  fault_printf("\n");
  backtrace_here();
  fault_printf("\nSystem halted.\n");

  irq_send_ipi_all();

  /* DIR-05 #139: paint the panic reason on the framebuffer too (no-UART
   * machines). fault_text() is the full transcript just tee'd through
   * fault_printf/fault_vprintf above. */
  panic_screen(fault_text());

  /* DIR-05 #139 watchdog: ~10 s grace to read the panic, then hard reset. */
  panic_reboot_after_grace();

  __builtin_unreachable();
}