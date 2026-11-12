/*
 * kernel/lib/stack_protector.c
 * Stack smashing protection (SSP) support
 *
 * Purpose:
 *   Provides the GCC/Clang SSP ABI symbols: __stack_chk_guard (the canary
 *   value) and __stack_chk_fail (the failure handler invoked when the compiler-
 *   inserted canary check detects a mismatch at function return).
 *
 * Role:
 *   The compiler emits a prologue/epilogue for every function compiled with
 *   -fstack-protector or -fstack-protector-strong:
 *     Prologue: load __stack_chk_guard; push copy onto the stack.
 *     Epilogue: compare stack copy with __stack_chk_guard; call
 *               __stack_chk_fail() if different.
 *   This file supplies both symbols.
 *
 * Canary value:
 *   A stack canary sits between local variables and the saved frame pointer /
 *   return address.  If a buffer overflow overwrites the return address, it
 *   typically also clobbers the canary.  On return, the compiler-emitted check
 *   detects the mismatch and calls __stack_chk_fail instead of returning to
 *   the corrupted address.
 *
 * Early boot reseeds the placeholder. Source provenance is reported without
 * exposing the guard; counter/address mixing has no qualified entropy bound.
 */
#include <kernel/entropy.h>
#include <kernel/cpu.h>
#include <kernel/fault.h>
#include <kernel/printk.h>
#include <kernel/ssp.h>
#include <stdint.h>

/* __stack_chk_guard - the canary value inserted by the compiler into protected
 * stack frames.  GCC/Clang load this symbol by name; it must be a global.
 * The compile-time value is a placeholder; stack_guard_init() reseeds it from a
 * hardware/jitter entropy source before deep boot (LIB-SSP-01 / #71). */
uintptr_t __stack_chk_guard = 0x595e9fbd94fda766;

/*
 * stack_guard_init - reseed the SSP canary from per-arch entropy.
 *
 * Replaces the compile-time constant with the available early source. The
 * fallback does not establish unpredictability. The low byte is forced to
 * 0x00 (the classic terminator-canary trick: a string-based overflow cannot
 * write past the NUL without supplying a terminator), and an all-zero result is
 * rejected so the guard is never trivially defeatable.
 *
 * Safety: this routine takes no address of a local and declares no array, so
 * -fstack-protector-strong inserts no canary in its own frame; it can therefore
 * mutate the global guard and return without tripping a stale-canary check.  It
 * must run before any still-executing caller that captured the OLD canary in
 * its prologue would reach its epilogue — see kernel/include/kernel/ssp.h.
 */
void stack_guard_init(void) {
  struct entropy_sample sample = entropy_sample_u64();
  uintptr_t g = (uintptr_t)sample.value;
  g &= ~(uintptr_t)0xff; /* terminator canary: NUL low byte */
  if (g == 0)
    g = 0x595e9fbd94fda700; /* never leave the guard all-zero */
  __stack_chk_guard = g;
  if (sample.source == ENTROPY_SOURCE_ARCH_RNG)
    pr_info("%s", "SSP: canary source=arch-rng\n");
  else if (sample.source == ENTROPY_SOURCE_COUNTER_MIX)
    pr_warn("%s", "SSP: canary source=counter-mix strength=unqualified\n");
  else
    pr_warn("%s", "SSP: canary source=address-mix strength=unqualified\n");
}

/* Forward declaration required before the definition because the compiler may
 * insert a call to __stack_chk_fail inside its own prolog/epilog for this
 * translation unit if it is also compiled with -fstack-protector. */
void __stack_chk_fail(void);

/*
 * __stack_chk_fail - called when the SSP canary check fails at function return.
 *
 * Captures the return address of the corrupted frame via __builtin_return_address(0)
 * (which is the return address from __stack_chk_fail's caller, i.e., the address
 * of the corrupted function's epilog), then calls panic() to print the site and
 * halt all CPUs.
 *
 * This function does not return.  The panic message includes the return address
 * to help identify which function was smashed.
 *
 * Locking: none; inherits panic()'s uart_lock acquisition.
 * Side effects: halts the entire system.
 */
void __stack_chk_fail(void) __attribute__((noinline));
void __stack_chk_fail(void) {
  uintptr_t ret_addr = (uintptr_t)__builtin_return_address(0);
  uintptr_t *self_fp = (uintptr_t *)__builtin_frame_address(0);
  uintptr_t caller_fp = self_fp ? self_fp[0] : 0;
  struct cpu_info *cpu = cpu_info_fault_safe();

  /* Do not dereference the caller frame here: on a real stack-corruption
   * event it may be invalid.  Preserve its address and the per-CPU ownership
   * pointers as raw values; the panic owner can correlate them with the
   * disassembly and scheduler trace without risking a nested data abort. */
  panic("SSP: Stack Smash ret=0x%lx caller_fp=0x%lx cpu=%u "
        "current_task=%p pending_stack_release=%p deferred_free=%p guard=0x%lx\n",
        (unsigned long)ret_addr, (unsigned long)caller_fp,
        cpu ? cpu->cpu_id : UINT32_MAX,
        cpu ? (void *)cpu->current_task : NULL,
        cpu ? (void *)cpu->pending_stack_release : NULL,
        cpu ? (void *)cpu->deferred_free_proc : NULL,
        (unsigned long)__stack_chk_guard);
}
