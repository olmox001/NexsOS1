#ifndef _KERNEL_SSP_H
#define _KERNEL_SSP_H

/*
 * Stack Smashing Protection (SSP) runtime support.
 *
 * stack_guard_init() reseeds the compiler's __stack_chk_guard canary from a
 * per-arch entropy source (arch_entropy_u64) early in boot, replacing the
 * compile-time constant.  See kernel/lib/stack_protector.c and issue
 * LIB-SSP-01 (#71).
 *
 * Contract: must be called exactly once, as early as possible in kernel_main
 * (before any function that runs to completion AFTER the call could have
 * captured the OLD canary in its prologue).  Because the seeding routine and
 * kernel_main never return through a canary-checked epilogue with a stale
 * value, reseeding the global mid-boot is safe.
 */
void stack_guard_init(void);

#endif /* _KERNEL_SSP_H */
