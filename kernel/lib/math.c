/*
 * kernel/lib/math.c
 * Fixed-Point Math Functions for Kernel (No FPU)
 *
 * Uses 16.16 fixed-point representation where possible.
 * All operations are integer-only to comply with -mgeneral-regs-only.
 */
#ifdef KERNEL
#include <kernel/math.h>
#include <kernel/types.h>
#else
#include <os1.h>
#endif

/* Fixed-point constants (16.16) */
#define FP_SHIFT 16
#define FP_ONE   (1 << FP_SHIFT)
#define FP_HALF  (1 << (FP_SHIFT - 1))
#define FP_PI    205887
#define FP_2PI   411775

/*
 * Integer Square Root (Newton-Raphson)
 * Returns floor(sqrt(n))
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
 * Fixed-point Square Root (16.16)
 * Input and output are 16.16 fixed-point
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
 * Fixed-point multiplication (16.16 * 16.16 -> 16.16)
 */
int32_t fixmul(int32_t a, int32_t b) {
  int64_t result = (int64_t)a * b;
  return (int32_t)(result >> FP_SHIFT);
}

#ifdef KERNEL
int32_t k_fixmul(int32_t a, int32_t b) { return fixmul(a, b); }
#endif

/*
 * Fixed-point division (16.16 / 16.16 -> 16.16)
 */
#ifdef KERNEL
int32_t k_fixdiv(int32_t a, int32_t b) {
  if (b == 0)
    return 0;
  int64_t result = ((int64_t)a << FP_SHIFT) / b;
  return (int32_t)result;
}

int32_t k_int_to_fp(int32_t x) { return x << FP_SHIFT; }

int32_t k_fp_to_int(int32_t x) { return x >> FP_SHIFT; }

int32_t k_fp_to_int_round(int32_t x) { return (x + FP_HALF) >> FP_SHIFT; }

int32_t k_fabs_fp(int32_t x) { return (x < 0) ? -x : x; }

int32_t k_floor_fp(int32_t x) { return x & ~((1 << FP_SHIFT) - 1); }

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
 * Sine approximation using Taylor series (fixed-point)
 * Input: angle in radians as 16.16 fixed-point
 * Uses: sin(x) ≈ x - x³/6 + x⁵/120
 */
int32_t sin_fp(int32_t x) {
  /* Reduce to -pi to pi range */
  while (x > FP_PI)
    x -= FP_2PI;
  while (x < -FP_PI)
    x += FP_2PI;

  /* Taylor series: sin(x) ≈ x - x³/6 + x⁵/120 */
  int32_t x2 = fixmul(x, x);
  int32_t x3 = fixmul(x2, x);
  int32_t x5 = fixmul(x3, x2);

  /* 1/6 ≈ 10923 in 16.16 */
  /* 1/120 ≈ 546 in 16.16 */
  int32_t term1 = x;
  int32_t term2 = fixmul(x3, 10923); /* x³/6 */
  int32_t term3 = fixmul(x5, 546);   /* x⁵/120 */

  return term1 - term2 + term3;
}

#ifdef KERNEL
int32_t k_sin_fp(int32_t x) { return sin_fp(x); }
#endif

/*
 * Cosine approximation (fixed-point)
 * cos(x) = sin(x + π/2)
 */
int32_t cos_fp(int32_t x) {
  /* PI/2 ≈ 102944 in 16.16 */
  return sin_fp(x + 102944);
}

#ifdef KERNEL
int32_t k_cos_fp(int32_t x) { return cos_fp(x); }
#endif

int32_t lerp_fp(int32_t a, int32_t b, int32_t t) {
  return a + fixmul(t, b - a);
}

#ifdef KERNEL
int32_t k_lerp_fp(int32_t a, int32_t b, int32_t t) { return lerp_fp(a, b, t); }
#endif
