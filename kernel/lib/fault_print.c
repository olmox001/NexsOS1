/*
 * kernel/lib/fault_print.c
 * Fault-safe emergency print + per-CPU fault recursion guard (Phase A).
 *
 * See kernel/fault.h for the contract.  This file deliberately avoids:
 *   - spinlocks (uart_lock may be held by a wedged CPU),
 *   - get_cpu_info() (on amd64 it reads LAPIC MMIO, which may be unmapped
 *     when the faulting address space is compromised),
 *   - heap or per-CPU buffers (a 256-byte stack buffer is enough and works
 *     on the dedicated 16KB fault stacks).
 *
 * vsnprintf is stateless and lock-free (see printk.c), so formatting here is
 * re-entrant by construction.
 */
#include <drivers/uart.h>
#include <kernel/cpu.h>
#include <kernel/fault.h>
#include <kernel/printk.h>
#include <kernel/types.h>
#include <stdarg.h>

/* Fallback depth counter for when arch_cpu_info_fault_safe() returns NULL
 * (early boot, corrupted GS base).  Global — a false "nested" positive across
 * two CPUs faulting simultaneously pre-init is acceptable: both halt cleanly. */
static volatile uint32_t fault_depth_fallback;

/* DIR-05 / #139 — on-screen panic.  Tee every byte of fault output into a plain
 * buffer so panic() can paint it on the framebuffer when no UART is attached
 * (the only place a kernel fault is otherwise visible).  Lock-free single-writer
 * by construction: fault output is serialised by the fault recursion guard +
 * panic's IPI quiesce. */
#define FAULT_TEXT_CAP 8192
static char fault_text_buf[FAULT_TEXT_CAP];
static volatile uint32_t fault_text_used;

static inline void fault_text_putc(char c) {
  uint32_t n = fault_text_used;
  if (n < FAULT_TEXT_CAP - 1) {
    fault_text_buf[n] = c;
    fault_text_buf[n + 1] = '\0';
    fault_text_used = n + 1;
  }
}

/* The captured fault transcript (NUL-terminated), oldest first.  Returned to
 * panic_screen(); the leading lines are the arch register dump + fault class,
 * which is exactly what a maintainer reads off a screen with no serial. */
const char *fault_text(void) { return fault_text_buf; }

void fault_vprintf(const char *fmt, va_list args) {
  char buf[256];
  vsnprintf(buf, sizeof(buf), fmt, args);
  for (const char *p = buf; *p; p++) {
    uart_putc_emergency(*p);
    fault_text_putc(*p);
  }
}

void fault_printf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  fault_vprintf(fmt, args);
  va_end(args);
}

unsigned fault_enter(void) {
  struct cpu_info *ci = arch_cpu_info_fault_safe();
  if (ci)
    return ++ci->in_fault;
  return __sync_add_and_fetch(&fault_depth_fallback, 1);
}

void fault_exit(void) {
  struct cpu_info *ci = arch_cpu_info_fault_safe();
  if (ci) {
    if (ci->in_fault)
      ci->in_fault--;
  } else {
    uint32_t prev = __sync_sub_and_fetch(&fault_depth_fallback, 1);
    if (prev == UINT32_MAX) __sync_add_and_fetch(&fault_depth_fallback, 1);
  }
}

unsigned fault_depth(void) {
  struct cpu_info *ci = arch_cpu_info_fault_safe();
  return ci ? ci->in_fault : fault_depth_fallback;
}
