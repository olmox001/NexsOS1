#ifndef _KERNEL_HAL_UACCESS_H
#define _KERNEL_HAL_UACCESS_H

#include <kernel/types.h>

/*
 * kernel/include/kernel/hal_uaccess.h
 * HAL contract for USER-MEMORY ACCESS.
 *
 * WHY THIS HEADER EXISTS
 * ----------------------
 * Copying to and from a user address space is architecture work — it manipulates
 * the address-space register, the TLB and the fault-fixup window — so under
 * ASTRA it is a HAL primitive with per-architecture providers, exactly like
 * `arch_bus_scan` / `arch_irq_init` in <kernel/hal.h>.
 *
 * It was not treated as one.  `arch_copy_string_from_user` had no HAL contract
 * at all: <kernel/vmm.h> macro-aliased it, and kernel/core/syscall_dispatch.c
 * re-declared it by hand as a local `extern`.  So the two implementations
 * (kernel/arch/aarch64/cpu/syscall.c, kernel/arch/amd64/mm/uaccess.c) had
 * nothing binding them to the same semantics, and they drifted apart in
 * precisely the way an unwritten contract always does.  Naming the contract is
 * the fix; the truncation problem below is what made the absence visible.
 *
 * THE TRUNCATION PROBLEM
 * ----------------------
 * Both providers SILENTLY TRUNCATE at max_len and return 0 — success.  Every
 * caller therefore proceeds with a string that is not the one the process
 * passed.  For a window title that is harmless.  For a path, a registry key or
 * an environment value it is a correctness bug that cannot be diagnosed from
 * the outside: the write "succeeds" and the matching read misses, because the
 * stored name is not the name that was asked for.
 *
 * The fix is NOT a bigger buffer.  Truncation-versus-fit is information the
 * copy loop already has and throws away; the contract just has to carry it.
 * So there are two wrappers over ONE provider:
 *
 *   hal_copy_string_from_user()         TOLERANT — truncation is success.
 *                                       For data where a shortened value is
 *                                       still usable (titles, diagnostics).
 *
 *   hal_copy_string_from_user_strict()  STRICT — truncation returns -E2BIG and
 *                                       the caller refuses the operation.  For
 *                                       data where a shortened value is a
 *                                       DIFFERENT value (paths, registry keys
 *                                       and values, environment variables).
 *
 * Which one applies is a property of the DATA, not of the architecture, so the
 * choice belongs at the call site and the mechanism belongs here.  Raising the
 * constants instead would be the same defect with a later failure date — see
 * Phase 18 (unbind the ceilings) in docs/PLAN-2026-07-17-STRATIFICATION.md,
 * which removes the fixed buffers entirely by moving these transfers
 * out-of-line, the way the execsvc request body already works.
 */

/* Provider contract — implemented once per architecture.
 *
 * arch_copy_string_from_user_n: copy a NUL-terminated string from user space.
 *   Returns 0 on success, -1 on a bad/unmapped source address.  When out_len is
 *   non-NULL it receives the number of bytes copied EXCLUDING the terminator,
 *   and *out_truncated is set to 1 if the source was longer than max_len - 1.
 *   Reporting both is what lets the two wrappers above share one provider. */
int arch_copy_string_from_user_n(char *dest, const char *src, size_t max_len,
                                 size_t *out_len, int *out_truncated);
int arch_copy_from_user(void *dest, const void *src, size_t n);
int arch_copy_to_user(void *dest, const void *src, size_t n);

/* Legacy spelling, kept because ~10 call sites use it and truncation is
 * acceptable for all of them; implemented over the provider above. */
int arch_copy_string_from_user(char *dest, const char *src, size_t max_len);

/* E2BIG reaches the kernel today from include/abi/posix_types.h, which the
 * layering gate counts as a violation (scripts/check-layering.sh).  The
 * fallback keeps this header self-contained once that include is removed, and
 * pins the value so the two sides cannot silently disagree in the meantime.
 * Errno numbering is precisely the "irreducible bootstrap contract" Phase 10a
 * has to place deliberately rather than inherit by accident. */
#ifndef E2BIG
#define E2BIG 7
#endif

/* Tolerant: a truncated result is accepted.  Identical to the historical
 * behaviour, so existing callers need no change. */
static inline int hal_copy_string_from_user(char *dest, const char *src,
                                            size_t max_len) {
  return arch_copy_string_from_user_n(dest, src, max_len, (size_t *)0,
                                      (int *)0);
}

/* Strict: a source that does not fit is REFUSED with -E2BIG rather than
 * silently shortened.  Use wherever a shortened string is a different string. */
static inline int hal_copy_string_from_user_strict(char *dest, const char *src,
                                                   size_t max_len) {
  int truncated = 0;
  int r = arch_copy_string_from_user_n(dest, src, max_len, (size_t *)0,
                                       &truncated);
  if (r != 0)
    return r;
  return truncated ? -E2BIG : 0;
}

#endif /* _KERNEL_HAL_UACCESS_H */
