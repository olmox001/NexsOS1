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
 * Known issues:
 *   LIB-SSP-01  (W3 SECURITY)  __stack_chk_guard is a compile-time constant
 *               (0x595e9fbd94fda766).  It is never read from a hardware entropy
 *               source and never randomised at boot.  Any attacker who knows the
 *               binary (or can guess the constant) can craft an overflow that
 *               preserves the canary value, bypassing SSP entirely.  Fix: read
 *               RNDR (AArch64) or RDRAND (AMD64) during early init and XOR the
 *               result into __stack_chk_guard before any guarded function runs.
 */
#include <kernel/entropy.h>
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
 * Replaces the compile-time constant with arch_entropy_u64() so an attacker who
 * knows the binary image cannot predict the canary.  The low byte is forced to
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
  uintptr_t g = (uintptr_t)entropy_u64();
  g &= ~(uintptr_t)0xff; /* terminator canary: NUL low byte */
  if (g == 0)
    g = 0x595e9fbd94fda700; /* never leave the guard all-zero */
  __stack_chk_guard = g;
  pr_info("%s", "SSP: stack canary reseeded from arch entropy\n");
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
void __stack_chk_fail(void) {
  uint64_t ret_addr = (uint64_t)__builtin_return_address(0);
  panic("SSP: Stack Smash at 0x%lx\n", ret_addr);
}
