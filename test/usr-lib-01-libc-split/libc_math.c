/*
 * user/sys/lib/libc_math.c
 * Userland fixed-point math (16.16) and __int128 runtime helpers
 *
 * Purpose:
 *   Standalone userland implementation of the fixed-point trig/interpolation
 *   helpers declared in include/api/math.h and os1.h (fixmul, sin_fp, cos_fp,
 *   lerp_fp), plus the __int128 libgcc-ABI helpers the compiler emits for
 *   128-bit arithmetic (__udivti3/__umodti3/__multi3) — needed because both
 *   the kernel and userland link -nostdlib with no libgcc.
 *
 * History (NOTE USR-LIB-01, fixed):
 *   user/sys/lib/lib.c used to `#include "../../kernel/lib/math.c"` directly.
 *   This file is the userland-owned replacement: a separate object built
 *   from a separate source under user/, kept in sync with math.h/os1.h by
 *   hand. kernel/lib/math.c is untouched and still carries the kernel-only
 *   k_* variants (k_isqrt, k_sqrt_fp, k_fixdiv, ...) that userland never
 *   needs.
 *
 * 16.16 Fixed-Point Convention:
 *   16 integer bits, 16 fractional bits. FP_ONE = 65536 (1.0). FP_PI =
 *   205887 (correct pi * 2^16).
 */
#include <os1.h>

#ifndef FP_SHIFT
#define FP_SHIFT 16
#endif
#ifndef FP_PI
#define FP_PI 205887
#endif
#define FP_2PI 411775

/*
 * fixmul - multiply two 16.16 fixed-point values, returning a 16.16 result.
 * Uses a 64-bit intermediate to avoid overflow.
 */
int32_t fixmul(int32_t a, int32_t b) {
  int64_t result = (int64_t)a * b;
  return (int32_t)(result >> FP_SHIFT);
}

/*
 * sin_fp - sine approximation in 16.16 fixed-point.
 * Range-reduces to [-pi, pi], reflects into [-pi/2, pi/2], then evaluates
 * the 5th-order Taylor series: sin(x) ~= x - x^3/6 + x^5/120.
 */
int32_t sin_fp(int32_t x) {
  while (x > FP_PI)
    x -= FP_2PI;
  while (x < -FP_PI)
    x += FP_2PI;

  int32_t half_pi = 102944; /* pi/2 in 16.16 */
  if (x > half_pi) {
    x = FP_PI - x;
  } else if (x < -half_pi) {
    x = -FP_PI - x;
  }

  int32_t x2 = fixmul(x, x);
  int32_t x3 = fixmul(x2, x);
  int32_t x5 = fixmul(x3, x2);

  int32_t term1 = x;
  int32_t term2 = fixmul(x3, 10923); /* x^3/6 */
  int32_t term3 = fixmul(x5, 546);   /* x^5/120 */

  return term1 - term2 + term3;
}

/* cos_fp - cos(x) = sin(x + pi/2). */
int32_t cos_fp(int32_t x) { return sin_fp(x + 102944); }

/*
 * lerp_fp - linear interpolation between two 16.16 fixed-point values.
 * a + t * (b - a); t == 0 returns a, t == FP_ONE returns b.
 */
int32_t lerp_fp(int32_t a, int32_t b, int32_t t) {
  return a + fixmul(t, b - a);
}

/* ---------------------------------------------------------------------------
 * __int128 ABI helpers — see kernel/lib/math.c's header for the full
 * rationale; identical implementation, independent copy for userland.
 * ------------------------------------------------------------------------- */
static __uint128_t u128_divmod(__uint128_t num, __uint128_t den,
                               __uint128_t *rem) {
  if (den == 0) {
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
  __uint128_t ua = (__uint128_t)a, ub = (__uint128_t)b;
  uint64_t alo = (uint64_t)ua, ahi = (uint64_t)(ua >> 64);
  uint64_t blo = (uint64_t)ub, bhi = (uint64_t)(ub >> 64);
  __uint128_t lo = (__uint128_t)alo * blo;
  __uint128_t mid = (__uint128_t)alo * bhi + (__uint128_t)ahi * blo;
  return (__int128_t)(lo + (mid << 64));
}
