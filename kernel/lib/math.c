/*
 * kernel/lib/math.c
 * Fixed-Point Math Functions for Kernel (No FPU)
 *
 * Uses 16.16 fixed-point representation where possible.
 * All operations are integer-only to comply with -mgeneral-regs-only.
 *
 * Purpose:
 *   Provides integer square root, fixed-point arithmetic (multiply, divide,
 *   convert, abs, floor, ceil), trigonometric approximations (sin, cos), and
 *   linear interpolation.  All arithmetic avoids the FPU; the kernel is compiled
 *   with -mgeneral-regs-only (AArch64) or equivalent.
 *
 * Role:
 *   The `k_*` prefixed variants are kernel-only (guarded by `#ifdef KERNEL`).
 *   The unprefixed `fixmul`, `sin_fp`, `cos_fp`, and `lerp_fp` are shared with
 *   userland (compiled without KERNEL defined, using os1.h instead of kernel
 *   headers).  Both sets are exposed via kernel/include/kernel/math.h.
 *
 * 16.16 Fixed-Point Convention:
 *   All fixed-point values use 16 integer bits and 16 fractional bits.
 *   FP_ONE = 65536 (1.0 in 16.16).  FP_HALF = 32768 (0.5 in 16.16).
 *   FP_PI  = correct π × 2^16 ≈ 205887 (declared in THIS file's fallback).
 *
 * Known issues:
 *   LIB-MATH-01  (W2 BUG latent)  In kernel builds, <kernel/math.h> defines
 *                FP_PI = 411775 (≈ 2π × 2^16, mislabeled "π × 131072").
 *                This shadows the correct fallback `#ifndef FP_PI / 205887`
 *                below.  As a result, in kernel builds FP_PI == FP_2PI == 411775,
 *                which makes sin_fp's range-reduction loop `while (x > FP_PI)`
 *                trigger only for x > 2π, and the reflection `x = FP_PI - x`
 *                uses the wrong pole.  sin_fp/cos_fp produce wrong results for
 *                all angles in (π/2, 2π].  LATENT: the only kernel caller,
 *                kernel/graphics/draw3d.c, is not in the Makefile build; no live
 *                compiled code calls sin_fp/cos_fp in the kernel today.  Fix:
 *                change kernel/include/kernel/math.h:16 to `FP_PI 205887`.
 *   LIB-MATH-03  (W1 REFINE)  sin_fp range-reduces by subtracting FP_2PI in
 *                a while loop.  For very large inputs this is O(n) rather than
 *                a single modulo operation.
 */
#ifdef KERNEL
#include <kernel/math.h>
#include <kernel/types.h>
#else
#include <os1.h>
#endif

/* Fixed-point constants (16.16) */
/* FP_SHIFT: number of fractional bits; 16.16 → shift = 16. */
#ifndef FP_SHIFT
#define FP_SHIFT 16
#endif /* FP_SHIFT */
/* FP_ONE: 1.0 in 16.16 fixed-point (65536). */
#ifndef FP_ONE
#define FP_ONE (1 << FP_SHIFT)
#endif /* FP_ONE */
/* FP_HALF: 0.5 in 16.16 fixed-point (32768). */
#ifndef FP_HALF
#define FP_HALF (1 << (FP_SHIFT - 1))
#endif /* FP_HALF */
/* FP_PI: π × 2^16 ≈ 205887.  This fallback is correct.
 * NOTE(LIB-MATH-01): In kernel builds, <kernel/math.h> already defined FP_PI
 * as 411775 (≈ 2π × 2^16) before this file is compiled, so this #ifndef branch
 * is never taken.  The compiled kernel has FP_PI == FP_2PI == 411775, breaking
 * sin_fp for angles in (π/2, 2π].  The fix is in kernel/include/kernel/math.h.
 * Userland builds (no KERNEL) use os1.h which defines FP_PI 205887 (correct). */
#ifndef FP_PI
#define FP_PI 205887
#endif /* FP_PI */
/* FP_2PI: 2π × 2^16 ≈ 411775 (correct in both builds). */
#define FP_2PI 411775

/*
 * k_isqrt - integer square root using Newton-Raphson iteration.
 *
 * Computes floor(sqrt(n)) using the recurrence y = (x + n/x) / 2, which
 * converges quadratically.  Terminates when the estimate stops decreasing.
 *
 * Params:
 *   n - unsigned 32-bit input value.
 * Returns: floor(sqrt(n)); 0 for n == 0.
 * Locking: none (stateless).
 * Side effects: none.
 */
#ifdef KERNEL
uint32_t k_isqrt(uint32_t n) {
  if (n == 0)
    return 0;

  uint32_t x = n;
  uint32_t y = (x + 1) >> 1;

  while (y < x) {
    x = y;
    y = (x + n / x) >> 1;
  }
  return x;
}
#endif

/*
 * k_sqrt_fp - fixed-point square root in 16.16 format.
 *
 * Approach: to compute sqrt(x) in 16.16, note that x is already scaled by 2^16.
 *   We want result r such that r × 2^-16 = sqrt(x × 2^-16).
 *   Shift x left 16 bits (giving x × 2^16 = x << 16 as a 64-bit value).
 *   The integer sqrt of that 64-bit value is sqrt(x × 2^16) = sqrt(x) × 2^8.
 *   Shift the result left 8 more bits to get sqrt(x) × 2^16 — the 16.16 result.
 *
 * Params:
 *   x - 16.16 fixed-point input; must be >= 0.
 * Returns: floor(sqrt(x)) in 16.16 fixed-point; 0 for x <= 0.
 * Locking: none.
 */
#ifdef KERNEL
int32_t k_sqrt_fp(int32_t x) {
  if (x <= 0)
    return 0;

  /* Shift to get more precision before sqrt */
  /* sqrt(x * 2^16) = sqrt(x) * 2^8 */
  /* We need sqrt(x) * 2^16, so multiply result by 2^8 */
  uint64_t n = (uint64_t)x << 16;               /* x * 2^32 */
  uint64_t root = k_isqrt((uint32_t)(n >> 16)); /* sqrt of upper 32 bits */

  /* Adjust: we computed sqrt(x * 2^16), need sqrt(x) * 2^16 */
  return (int32_t)(root << 8);
}
#endif

/*
 * fixmul - multiply two 16.16 fixed-point values, returning a 16.16 result.
 *
 * Computes (a × b) >> 16 using a 64-bit intermediate to avoid overflow.
 * Both arguments and the result are 16.16 signed fixed-point.
 *
 * Params:
 *   a, b - 16.16 fixed-point operands.
 * Returns: (a × b) in 16.16 fixed-point (truncated, not rounded).
 * Locking: none.
 */
int32_t fixmul(int32_t a, int32_t b) {
  int64_t result = (int64_t)a * b;
  return (int32_t)(result >> FP_SHIFT);
}

#ifdef KERNEL
int32_t k_fixmul(int32_t a, int32_t b) { return fixmul(a, b); }
#endif

/*
 * k_fixdiv - divide two 16.16 fixed-point values, returning a 16.16 result.
 *
 * Computes (a << 16) / b using a 64-bit intermediate to avoid overflow.
 * Returns 0 if b == 0 (no trap; silent division-by-zero suppression).
 *
 * Params:
 *   a - dividend in 16.16.
 *   b - divisor in 16.16; if 0, returns 0.
 * Returns: (a / b) in 16.16 fixed-point.
 * Locking: none.
 */
#ifdef KERNEL
int32_t k_fixdiv(int32_t a, int32_t b) {
  if (b == 0)
    return 0;
  int64_t result = ((int64_t)a << FP_SHIFT) / b;
  return (int32_t)result;
}

/* k_int_to_fp - convert a signed integer to 16.16 fixed-point (x × 2^16). */
int32_t k_int_to_fp(int32_t x) { return x << FP_SHIFT; }

/* k_fp_to_int - truncate a 16.16 fixed-point value to integer (floor for +ve). */
int32_t k_fp_to_int(int32_t x) { return x >> FP_SHIFT; }

/* k_fp_to_int_round - round a 16.16 fixed-point value to the nearest integer.
 * Adds FP_HALF (0.5) before right-shifting to implement round-half-up. */
int32_t k_fp_to_int_round(int32_t x) { return (x + FP_HALF) >> FP_SHIFT; }

/* k_fabs_fp - absolute value of a 16.16 fixed-point number. */
int32_t k_fabs_fp(int32_t x) { return (x < 0) ? -x : x; }

/* k_floor_fp - round a 16.16 fixed-point value down to the nearest whole number.
 * Clears the 16 fractional bits by ANDing with the integer-part mask. */
int32_t k_floor_fp(int32_t x) { return x & ~((1 << FP_SHIFT) - 1); }

/*
 * k_ceil_fp - round a 16.16 fixed-point value up to the nearest whole number.
 *
 * If the fractional part is zero, x is already an integer — return unchanged.
 * For positive values: clear fractional bits and add FP_ONE (round up).
 * For negative values: clearing the fractional bits already moves toward zero
 * (which is ceiling direction for negative numbers).
 */
int32_t k_ceil_fp(int32_t x) {
  int32_t frac = x & ((1 << FP_SHIFT) - 1);
  if (frac == 0)
    return x;

  if (x >= 0) {
    return (x & ~((1 << FP_SHIFT) - 1)) + FP_ONE;
  } else {
    return x & ~((1 << FP_SHIFT) - 1);
  }
}
#endif

/*
 * sin_fp - sine approximation in 16.16 fixed-point.
 *
 * Input: angle in radians, 16.16 fixed-point.
 * Output: sin(x), 16.16 fixed-point (range approximately [-65536, 65536]).
 *
 * Algorithm:
 *   1. Range-reduce to [-FP_PI, FP_PI] by repeated addition/subtraction of FP_2PI.
 *      NOTE(LIB-MATH-03): this is an O(n) loop; large inputs are slow.
 *   2. Reflect into [-π/2, π/2] using sin(x) = sin(π - x) for x > π/2 and
 *      sin(x) = sin(-π - x) for x < -π/2.
 *   3. Evaluate the 5th-order Taylor series: sin(x) ≈ x - x³/6 + x⁵/120.
 *      Coefficients in 16.16: 1/6 ≈ 10923, 1/120 ≈ 546.
 *
 * NOTE(LIB-MATH-01): In kernel builds, FP_PI is 411775 (== FP_2PI) due to
 *   the shadowing in <kernel/math.h>.  The range-reduction loop condition
 *   `x > FP_PI` is equivalent to `x > FP_2PI`, so inputs in (π/2, 2π] pass
 *   through unreduced.  The reflection at step 2 then uses the wrong pole
 *   (FP_PI = 2π instead of π), producing incorrect results for three-quarters
 *   of the domain.  No live compiled kernel code calls sin_fp today (latent).
 *
 * Locking: none (stateless).
 */
int32_t sin_fp(int32_t x) {
  /* Reduce to -pi to pi range */
  /* NOTE(LIB-MATH-01 + LIB-MATH-03): loop range-reduction; see file header.
   * In correct builds (FP_PI=205887), reduces x into (-π, π].
   * In kernel builds (FP_PI=411775=FP_2PI), effectively reduces to (-2π, 2π]. */
  while (x > FP_PI)
    x -= FP_2PI;
  while (x < -FP_PI)
    x += FP_2PI;

  /* Reflect domain to -pi/2 to pi/2 range.
   * Taylor series approximation accuracy diminishes rapidly beyond |x| > pi/2.
   */
  /* half_pi: π/2 in 16.16 fixed-point ≈ 1.5708 × 65536 = 102944.
   * NOTE(LIB-MATH-01): with FP_PI=411775 this reflection uses the wrong pole. */
  int32_t half_pi = 102944; /* PI / 2 represented in 16.16 fixed point */
  if (x > half_pi) {
    x = FP_PI - x;
  } else if (x < -half_pi) {
    x = -FP_PI - x;
  }

  /* Taylor series: sin(x) ≈ x - x³/6 + x⁵/120 */
  int32_t x2 = fixmul(x, x);
  int32_t x3 = fixmul(x2, x);
  int32_t x5 = fixmul(x3, x2);

  /* 1/6 ≈ 10923 in 16.16 */
  /* 1/120 ≈ 546 in 16.16 */
  /* (65536/6 = 10922.67 rounded; 65536/120 = 546.13 truncated) */
  int32_t term1 = x;
  int32_t term2 = fixmul(x3, 10923); /* x³/6 */
  int32_t term3 = fixmul(x5, 546);   /* x⁵/120 */

  return term1 - term2 + term3;
}

#ifdef KERNEL
int32_t k_sin_fp(int32_t x) { return sin_fp(x); }
#endif

/*
 * cos_fp - cosine approximation in 16.16 fixed-point.
 *
 * Uses the identity cos(x) = sin(x + π/2).  Adds 102944 (≈ π/2 in 16.16) to
 * the argument and delegates to sin_fp.  Inherits all properties and caveats
 * of sin_fp, including NOTE(LIB-MATH-01).
 *
 * Params:
 *   x - angle in radians, 16.16 fixed-point.
 * Returns: cos(x) in 16.16 fixed-point.
 * Locking: none.
 */
int32_t cos_fp(int32_t x) {
  /* PI/2 ≈ 102944 in 16.16 */
  return sin_fp(x + 102944);
}

#ifdef KERNEL
int32_t k_cos_fp(int32_t x) { return cos_fp(x); }
#endif

/*
 * lerp_fp - linear interpolation between two 16.16 fixed-point values.
 *
 * Computes a + t × (b - a), where t is a 16.16 fraction in [0, FP_ONE].
 * t == 0 returns a; t == FP_ONE returns b.
 *
 * Params:
 *   a, b - 16.16 fixed-point endpoints.
 *   t    - 16.16 interpolation factor; typically in [0, 65536].
 * Returns: interpolated 16.16 value.
 * Locking: none.
 */
int32_t lerp_fp(int32_t a, int32_t b, int32_t t) {
  return a + fixmul(t, b - a);
}

#ifdef KERNEL
int32_t k_lerp_fp(int32_t a, int32_t b, int32_t t) { return lerp_fp(a, b, t); }
#endif

/* ---------------------------------------------------------------------------
 * 128-bit integer runtime support (__int128 ABI helpers)
 *
 * Both the kernel and userland link -nostdlib with NO libgcc, so the compiler
 * intrinsics it emits for __int128 division/multiplication (__udivti3,
 * __umodti3, __multi3) would be undefined symbols at link time. Providing them
 * here — in the shared math library compiled into both the kernel and lib.o —
 * makes __uint128_t a first-class type across the codebase (e.g. the real-time
 * clock mono_ns() in kernel/core/timer.c can use the natural
 * (__uint128_t)delta * NSEC_PER_SEC / freq form). Add/sub/shift/compare on
 * __int128 are emitted inline by the compiler and need no helper.
 *
 * All three are implemented with only 64-bit operations so they are themselves
 * self-contained (no recursive intrinsic emission).
 * ------------------------------------------------------------------------- */

/* Use the GCC predefined 128-bit typedefs (__uint128_t / __int128_t) rather
 * than the bare `__int128` keyword: the keyword trips -Wpedantic/-Werror in
 * this ISO-C build, the predefined typedefs do not (the same reason pt_regs.h
 * and mono_ns() use __uint128_t). The libgcc ABI is identical either way. */

/* u128_divmod - unsigned 128-bit division with remainder (binary long division,
 * O(128)). Returns the quotient; *rem (if non-NULL) receives the remainder.
 * Division by zero returns all-ones / zero remainder rather than trapping. */
static __uint128_t u128_divmod(__uint128_t num, __uint128_t den,
                               __uint128_t *rem) {
  if (den == 0) { /* mirror hardware-less UB choice: avoid a fault */
    if (rem)
      *rem = 0;
    return ~(__uint128_t)0;
  }
  __uint128_t q = 0, r = 0;
  for (int i = 127; i >= 0; i--) {
    r = (r << 1) | ((num >> i) & 1);
    if (r >= den) {
      r -= den;
      q |= ((__uint128_t)1 << i);
    }
  }
  if (rem)
    *rem = r;
  return q;
}

/* libgcc-compatible signatures. Declared before definition to satisfy
 * -Wmissing-prototypes; the compiler emits calls to these by name. */
__uint128_t __udivti3(__uint128_t a, __uint128_t b);
__uint128_t __umodti3(__uint128_t a, __uint128_t b);
__int128_t __multi3(__int128_t a, __int128_t b);

__uint128_t __udivti3(__uint128_t a, __uint128_t b) {
  return u128_divmod(a, b, 0);
}

__uint128_t __umodti3(__uint128_t a, __uint128_t b) {
  __uint128_t r;
  u128_divmod(a, b, &r);
  return r;
}

__int128_t __multi3(__int128_t a, __int128_t b) {
  /* Split into 64-bit halves; each partial product is a 64x64→128 widening
   * multiply, which the compiler does inline (no __multi3 recursion). Only the
   * low 128 bits are defined, which is all the ABI requires. */
  __uint128_t ua = (__uint128_t)a, ub = (__uint128_t)b;
  uint64_t alo = (uint64_t)ua, ahi = (uint64_t)(ua >> 64);
  uint64_t blo = (uint64_t)ub, bhi = (uint64_t)(ub >> 64);
  __uint128_t lo = (__uint128_t)alo * blo;
  __uint128_t mid = (__uint128_t)alo * bhi + (__uint128_t)ahi * blo;
  return (__int128_t)(lo + (mid << 64));
}