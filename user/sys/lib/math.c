/*
 * user/sys/lib/math.c
 * NexsOS1 Bare-Metal Math Library — IEEE-754 float/double
 *
 * Self-contained implementation for -nostdlib -fno-builtin -ffreestanding.
 * No libgcc/libm dependency. Algorithms derived from musl and fdlibm
 * (Sun Microsystems), adapted for single-file bare-metal use.
 *
 * Architecture: both AMD64 and AArch64 (pure C, no inline asm, no FPU
 * intrinsics — the compiler's soft-float or hard-float codegen handles
 * the ABI).
 *
 * Precision targets:
 *   - float:  <= 1 ulp for elementary functions, <= 2 ulp for transcendentals
 *   - double: <= 1 ulp for elementary functions, <= 3 ulp for transcendentals
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

/* ============================================================================
 * Internal helpers — bit manipulation
 * ============================================================================
 */

static inline uint32_t __f32_bits(float x) {
  union {
    float f;
    uint32_t u;
  } v = {x};
  return v.u;
}

static inline float __f32_from_bits(uint32_t u) {
  union {
    uint32_t u;
    float f;
  } v = {u};
  return v.f;
}

static inline uint64_t __f64_bits(double x) {
  union {
    double f;
    uint64_t u;
  } v = {x};
  return v.u;
}

static inline double __f64_from_bits(uint64_t u) {
  union {
    uint64_t u;
    double f;
  } v = {u};
  return v.f;
}

static inline int __f32_sign(float x) { return (int)(__f32_bits(x) >> 31); }

static inline int __f64_sign(double x) { return (int)(__f64_bits(x) >> 63); }

static inline uint32_t __f32_abs_bits(float x) {
  return __f32_bits(x) & 0x7fffffffu;
}

static inline uint64_t __f64_abs_bits(double x) {
  return __f64_bits(x) & 0x7fffffffffffffffULL;
}

/* ============================================================================
 * Classification
 * ============================================================================
 */

int __float32_isnan(float x) { return (__f32_abs_bits(x) > 0x7f800000u); }

int __float32_isinf(float x) { return (__f32_abs_bits(x) == 0x7f800000u); }

int __float64_isnan(double x) {
  return (__f64_abs_bits(x) > 0x7ff0000000000000ULL);
}

int __float64_isinf(double x) {
  return (__f64_abs_bits(x) == 0x7ff0000000000000ULL);
}

/* ============================================================================
 * Integer absolute value
 * ============================================================================
 */

int abs(int x) { return x < 0 ? -x : x; }

/* ============================================================================
 * Float absolute value
 * ============================================================================
 */

float fabsf(float x) { return __f32_from_bits(__f32_abs_bits(x)); }

double fabs(double x) { return __f64_from_bits(__f64_abs_bits(x)); }

/* ============================================================================
 * Square root — Newton-Raphson iteration
 * ============================================================================
 */

float sqrtf(float x) {
  if (__f32_bits(x) == 0 || __f32_bits(x) == 0x80000000u)
    return x;
  if (__f32_abs_bits(x) > 0x7f800000u)
    return x;
  if (__f32_bits(x) >> 31)
    return __f32_from_bits(0x7fc00000u);
  if (__f32_abs_bits(x) == 0x7f800000u)
    return x;

  uint32_t ix = __f32_bits(x);
  uint32_t est = (ix >> 1) + 0x1fbb67a8u;
  float y = __f32_from_bits(est);

  y = 0.5f * (y + x / y);
  y = 0.5f * (y + x / y);
  y = 0.5f * (y + x / y);
  return y;
}

double sqrt(double x) {
  if (__f64_bits(x) == 0 || __f64_bits(x) == 0x8000000000000000ULL)
    return x;
  if (__f64_abs_bits(x) > 0x7ff0000000000000ULL)
    return x;
  if (__f64_bits(x) >> 63)
    return __f64_from_bits(0x7ff8000000000000ULL);
  if (__f64_abs_bits(x) == 0x7ff0000000000000ULL)
    return x;

  uint64_t ix = __f64_bits(x);
  uint64_t est = (ix >> 1) + 0x1ff7a7bea2a2a2a2ULL;
  double y = __f64_from_bits(est);

  y = 0.5 * (y + x / y);
  y = 0.5 * (y + x / y);
  y = 0.5 * (y + x / y);
  y = 0.5 * (y + x / y);
  return y;
}

/* ============================================================================
 * Floor / ceil / round / trunc
 * ============================================================================
 */

float floorf(float x) {
  uint32_t ix = __f32_bits(x);
  int e = (int)((ix >> 23) & 0xff) - 0x7f;
  uint32_t m;

  if (e >= 23)
    return x;
  if (e < 0) {
    if (ix >> 31)
      return -1.0f;
    return 0.0f * x;
  }

  m = 0x007fffffu >> e;
  if ((ix & m) == 0)
    return x;

  if (ix >> 31)
    ix += m;
  ix &= ~m;
  return __f32_from_bits(ix);
}

float ceilf(float x) {
  uint32_t ix = __f32_bits(x);
  int e = (int)((ix >> 23) & 0xff) - 0x7f;
  uint32_t m;

  if (e >= 23)
    return x;
  if (e < 0) {
    if (!(ix >> 31))
      return 1.0f;
    return -0.0f;
  }

  m = 0x007fffffu >> e;
  if ((ix & m) == 0)
    return x;

  if (!(ix >> 31))
    ix += m;
  ix &= ~m;
  return __f32_from_bits(ix);
}

float truncf(float x) {
  uint32_t ix = __f32_bits(x);
  int e = (int)((ix >> 23) & 0xff) - 0x7f;
  uint32_t m;

  if (e >= 23)
    return x;
  if (e < 0)
    return __f32_from_bits(ix & 0x80000000u);

  m = 0x007fffffu >> e;
  return __f32_from_bits(ix & ~m);
}

float roundf(float x) {
  uint32_t ix = __f32_bits(x);
  int e = (int)((ix >> 23) & 0xff) - 0x7f;
  uint32_t m;

  if (e >= 23)
    return x;
  if (e < -1)
    return 0.0f * x;

  m = 0x007fffffu >> e;
  if ((ix & m) == 0)
    return x;

  ix += 0x00400000u >> e;
  ix &= ~m;
  return __f32_from_bits(ix);
}

double floor(double x) {
  uint64_t ix = __f64_bits(x);
  int e = (int)((ix >> 52) & 0x7ff) - 0x3ff;
  uint64_t m;

  if (e >= 52)
    return x;
  if (e < 0) {
    if (ix >> 63)
      return -1.0;
    return 0.0 * x;
  }

  m = 0x000fffffffffffffULL >> e;
  if ((ix & m) == 0)
    return x;

  if (ix >> 63)
    ix += m;
  ix &= ~m;
  return __f64_from_bits(ix);
}

double ceil(double x) {
  uint64_t ix = __f64_bits(x);
  int e = (int)((ix >> 52) & 0x7ff) - 0x3ff;
  uint64_t m;

  if (e >= 52)
    return x;
  if (e < 0) {
    if (!(ix >> 63))
      return 1.0;
    return -0.0;
  }

  m = 0x000fffffffffffffULL >> e;
  if ((ix & m) == 0)
    return x;

  if (!(ix >> 63))
    ix += m;
  ix &= ~m;
  return __f64_from_bits(ix);
}

double trunc(double x) {
  uint64_t ix = __f64_bits(x);
  int e = (int)((ix >> 52) & 0x7ff) - 0x3ff;
  uint64_t m;

  if (e >= 52)
    return x;
  if (e < 0)
    return __f64_from_bits(ix & 0x8000000000000000ULL);

  m = 0x000fffffffffffffULL >> e;
  return __f64_from_bits(ix & ~m);
}

double round(double x) {
  uint64_t ix = __f64_bits(x);
  int e = (int)((ix >> 52) & 0x7ff) - 0x3ff;
  uint64_t m;

  if (e >= 52)
    return x;
  if (e < -1)
    return 0.0 * x;

  m = 0x000fffffffffffffULL >> e;
  if ((ix & m) == 0)
    return x;

  ix += 0x0008000000000000ULL >> e;
  ix &= ~m;
  return __f64_from_bits(ix);
}

/* ============================================================================
 * Fmod — floating-point remainder
 * ============================================================================
 */

float fmodf(float x, float y) {
  uint32_t ix = __f32_bits(x), iy = __f32_bits(y);
  int ex, ey;
  uint32_t mx, my;

  if (iy << 1 == 0 || (iy & 0x7f800000u) == 0x7f800000u ||
      (ix & 0x7f800000u) == 0x7f800000u)
    return (x * y) / (x * y);

  ex = (int)((ix >> 23) & 0xff) - 0x7f;
  ey = (int)((iy >> 23) & 0xff) - 0x7f;
  mx = (ix & 0x007fffffu) | 0x00800000u;
  my = (iy & 0x007fffffu) | 0x00800000u;

  if (ex < ey)
    return x;

  while (ex > ey) {
    if (mx >= my)
      mx -= my;
    mx <<= 1;
    ex--;
  }
  if (mx >= my)
    mx -= my;

  if (mx == 0)
    return __f32_from_bits(ix & 0x80000000u);

  while ((mx & 0x00800000u) == 0) {
    mx <<= 1;
    ex--;
  }

  ix =
      (ix & 0x80000000u) | (((uint32_t)(ex + 0x7f)) << 23) | (mx & 0x007fffffu);
  return __f32_from_bits(ix);
}

double fmod(double x, double y) {
  uint64_t ix = __f64_bits(x), iy = __f64_bits(y);
  int ex, ey;
  uint64_t mx, my;

  if (iy << 1 == 0 || (iy & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL ||
      (ix & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL)
    return (x * y) / (x * y);

  ex = (int)((ix >> 52) & 0x7ff) - 0x3ff;
  ey = (int)((iy >> 52) & 0x7ff) - 0x3ff;
  mx = (ix & 0x000fffffffffffffULL) | 0x0010000000000000ULL;
  my = (iy & 0x000fffffffffffffULL) | 0x0010000000000000ULL;

  if (ex < ey)
    return x;

  while (ex > ey) {
    if (mx >= my)
      mx -= my;
    mx <<= 1;
    ex--;
  }
  if (mx >= my)
    mx -= my;

  if (mx == 0)
    return __f64_from_bits(ix & 0x8000000000000000ULL);

  while ((mx & 0x0010000000000000ULL) == 0) {
    mx <<= 1;
    ex--;
  }

  ix = (ix & 0x8000000000000000ULL) | (((uint64_t)(ex + 0x3ff)) << 52) |
       (mx & 0x000fffffffffffffULL);
  return __f64_from_bits(ix);
}

/* ============================================================================
 * Min / max / copysign / frexp / ldexp
 * ============================================================================
 */

float fminf(float x, float y) {
  if (__float32_isnan(x))
    return y;
  if (__float32_isnan(y))
    return x;
  if (__f32_bits(x) == 0 && __f32_bits(y) == 0x80000000u)
    return y;
  if (__f32_bits(y) == 0 && __f32_bits(x) == 0x80000000u)
    return x;
  return x < y ? x : y;
}

float fmaxf(float x, float y) {
  if (__float32_isnan(x))
    return y;
  if (__float32_isnan(y))
    return x;
  if (__f32_bits(x) == 0 && __f32_bits(y) == 0x80000000u)
    return x;
  if (__f32_bits(y) == 0 && __f32_bits(x) == 0x80000000u)
    return y;
  return x > y ? x : y;
}

float copysignf(float x, float y) {
  return __f32_from_bits((__f32_bits(x) & 0x7fffffffu) |
                         (__f32_bits(y) & 0x80000000u));
}

float frexpf(float x, int *exp) {
  uint32_t ix = __f32_bits(x);
  int e = (int)((ix >> 23) & 0xff);

  if (e == 0) {
    if ((ix & 0x007fffffu) == 0) {
      if (exp)
        *exp = 0;
      return x;
    }
    x *= 0x1.0p64f;
    ix = __f32_bits(x);
    e = (int)((ix >> 23) & 0xff) - 64;
  } else if (e == 0xff) {
    if (exp)
      *exp = 0;
    return x;
  }

  if (exp)
    *exp = e - 0x7e;
  ix = (ix & 0x807fffffu) | 0x3f000000u;
  return __f32_from_bits(ix);
}

float ldexpf(float x, int exp) {
  uint32_t ix = __f32_bits(x);
  int e = (int)((ix >> 23) & 0xff);

  if (e == 0 || e == 0xff)
    return x;

  e += exp;
  if (e >= 0xff)
    return __f32_from_bits((ix & 0x80000000u) | 0x7f800000u);
  if (e <= 0) {
    if (e <= -24)
      return __f32_from_bits(ix & 0x80000000u);
    ix = (ix & 0x807fffffu) | ((uint32_t)(1) << 23);
    while (e < 0) {
      ix >>= 1;
      e++;
    }
    ix = (ix & 0x807fffffu) | ((uint32_t)(e) << 23);
    return __f32_from_bits(ix);
  }
  ix = (ix & 0x807fffffu) | ((uint32_t)(e) << 23);
  return __f32_from_bits(ix);
}

double fmin(double x, double y) {
  if (__float64_isnan(x))
    return y;
  if (__float64_isnan(y))
    return x;
  if (__f64_bits(x) == 0 && __f64_bits(y) == 0x8000000000000000ULL)
    return y;
  if (__f64_bits(y) == 0 && __f64_bits(x) == 0x8000000000000000ULL)
    return x;
  return x < y ? x : y;
}

double fmax(double x, double y) {
  if (__float64_isnan(x))
    return y;
  if (__float64_isnan(y))
    return x;
  if (__f64_bits(x) == 0 && __f64_bits(y) == 0x8000000000000000ULL)
    return x;
  if (__f64_bits(y) == 0 && __f64_bits(x) == 0x8000000000000000ULL)
    return y;
  return x > y ? x : y;
}

double copysign(double x, double y) {
  return __f64_from_bits((__f64_bits(x) & 0x7fffffffffffffffULL) |
                         (__f64_bits(y) & 0x8000000000000000ULL));
}

double frexp(double x, int *exp) {
  uint64_t ix = __f64_bits(x);
  int e = (int)((ix >> 52) & 0x7ff);

  if (e == 0) {
    if ((ix & 0x000fffffffffffffULL) == 0) {
      if (exp)
        *exp = 0;
      return x;
    }
    x *= 0x1.0p64;
    ix = __f64_bits(x);
    e = (int)((ix >> 52) & 0x7ff) - 64;
  } else if (e == 0x7ff) {
    if (exp)
      *exp = 0;
    return x;
  }

  if (exp)
    *exp = e - 0x3fe;
  ix = (ix & 0x800fffffffffffffULL) | 0x3fe0000000000000ULL;
  return __f64_from_bits(ix);
}

double ldexp(double x, int exp) {
  uint64_t ix = __f64_bits(x);
  int e = (int)((ix >> 52) & 0x7ff);

  if (e == 0 || e == 0x7ff)
    return x;

  e += exp;
  if (e >= 0x7ff)
    return __f64_from_bits((ix & 0x8000000000000000ULL) |
                           0x7ff0000000000000ULL);
  if (e <= 0) {
    if (e <= -53)
      return __f64_from_bits(ix & 0x8000000000000000ULL);
    ix = (ix & 0x800fffffffffffffULL) | 0x0010000000000000ULL;
    while (e < 0) {
      ix >>= 1;
      e++;
    }
    ix = (ix & 0x800fffffffffffffULL) | ((uint64_t)(e) << 52);
    return __f64_from_bits(ix);
  }
  ix = (ix & 0x800fffffffffffffULL) | ((uint64_t)(e) << 52);
  return __f64_from_bits(ix);
}

/* ============================================================================
 * Exponential and logarithm
 * ============================================================================
 */

static const float __expf_c[6] = {1.0f,
                                  0.5f,
                                  0.16666667163372039794921875f,
                                  0.041666485369205474853515625f,
                                  0.00833336085736751556396484375f,
                                  0.00137857165321695804595947265625f};

float expf(float x) {
  float y, r;
  int n;

  if (__f32_bits(x) >= 0x42b17218u) {
    if (__f32_bits(x) >= 0x7f800000u) {
      if (__f32_bits(x) == 0x7f800000u)
        return x;
      return x;
    }
    return 0x1.fffffep+127f * 0x1.fffffep+127f;
  }
  if (__f32_bits(x) >= 0xc2d20000u)
    return 0.0f;

  n = (int)(x * 1.44269504088896340736f + (__f32_sign(x) ? -0.5f : 0.5f));
  r = x - (float)n * 0.69314718055994530942f;

  y = __expf_c[0] +
      r * (__expf_c[1] +
           r * (__expf_c[2] +
                r * (__expf_c[3] + r * (__expf_c[4] + r * __expf_c[5]))));

  return ldexpf(y, n);
}

float exp2f(float x) { return expf(x * 0.69314718055994530942f); }

float logf(float x) {
  uint32_t ix = __f32_bits(x);
  int e;
  float y, z;

  if ((ix & 0x7f800000u) == 0) {
    if (ix << 1 == 0)
      return -INFINITY;
    return __f32_from_bits(0x7fc00000u);
  }
  if (ix >= 0x7f800000u)
    return x;

  e = (int)((ix >> 23) & 0xff) - 0x7f;
  ix = (ix & 0x007fffffu) | 0x3f800000u;
  x = __f32_from_bits(ix);

  y = (x - 1.0f) / (x + 1.0f);
  z = y * y;
  y = y * (2.0f + z * (0.666666686534881591796875f + z * 0.4f));

  return y + (float)e * 0.69314718055994530942f;
}

float log2f(float x) { return logf(x) * 1.44269504088896340736f; }

float log10f(float x) { return logf(x) * 0.43429448190325182765f; }

static const double __exp_c[8] = {1.0,
                                  0.5,
                                  0.16666666666666665741,
                                  0.041666666666666664353,
                                  0.0083333333333333332177,
                                  0.0013888888888888889419,
                                  0.0001984126984126984127,
                                  0.000024801587301587301587};

double exp(double x) {
  double y, r;
  int n;

  if (__f64_bits(x) >= 0x40862e42fefa39efULL) {
    if (__f64_bits(x) >= 0x7ff0000000000000ULL) {
      if (__f64_bits(x) == 0x7ff0000000000000ULL)
        return x;
      return x;
    }
    return 0x1.fffffffffffffp+1023 * 0x1.fffffffffffffp+1023;
  }
  if (__f64_bits(x) >= 0xc0874910d52d3051ULL)
    return 0.0;

  n = (int)(x * 1.44269504088896340736 + (__f64_sign(x) ? -0.5 : 0.5));
  r = x - (double)n * 0.69314718055994530942;

  y = __exp_c[0] +
      r * (__exp_c[1] +
           r * (__exp_c[2] +
                r * (__exp_c[3] +
                     r * (__exp_c[4] +
                          r * (__exp_c[5] +
                               r * (__exp_c[6] + r * __exp_c[7]))))));

  return ldexp(y, n);
}

double exp2(double x) { return exp(x * 0.69314718055994530942); }

double log(double x) {
  uint64_t ix = __f64_bits(x);
  int e;
  double y, z;

  if ((ix & 0x7ff0000000000000ULL) == 0) {
    if (ix << 1 == 0)
      return -INFINITY;
    return __f64_from_bits(0x7ff8000000000000ULL);
  }
  if (ix >= 0x7ff0000000000000ULL)
    return x;

  e = (int)((ix >> 52) & 0x7ff) - 0x3ff;
  ix = (ix & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
  x = __f64_from_bits(ix);

  y = (x - 1.0) / (x + 1.0);
  z = y * y;
  y = y * (2.0 + z * (0.66666666666666662966 +
                      z * (0.4 + z * 0.28571428571428569843)));

  return y + (double)e * 0.69314718055994530942;
}

double log2(double x) { return log(x) * 1.44269504088896340736; }

double log10(double x) { return log(x) * 0.43429448190325182765; }

/* ============================================================================
 * Power function
 * ============================================================================
 */

float powf(float x, float y) {
  uint32_t ix = __f32_bits(x), iy = __f32_bits(y);
  float r;

  if (iy == 0 || x == 1.0f)
    return 1.0f;
  if (__float32_isnan(x) || __float32_isnan(y))
    return x + y;
  if (x == 0.0f) {
    if ((iy & 0x7fffffffu) > 0x7f800000u)
      return x + y;
    if (y > 0.0f)
      return (iy >> 31) ? INFINITY : 0.0f;
    if (y < 0.0f)
      return (iy >> 31) ? 0.0f : INFINITY;
    return 1.0f;
  }
  if (__float32_isinf(y)) {
    if (fabsf(x) == 1.0f)
      return 1.0f;
    if ((ix >> 31) == 0) {
      if (y > 0.0f)
        return (fabsf(x) > 1.0f) ? INFINITY : 0.0f;
      return (fabsf(x) > 1.0f) ? 0.0f : INFINITY;
    }
  }

  r = logf(fabsf(x));
  r = expf(y * r);

  if (ix >> 31) {
    float fy = floorf(y);
    if (y == fy) {
      uint32_t iy_int = (uint32_t)(int)fy;
      if (iy_int & 1)
        r = -r;
    } else {
      return __f32_from_bits(0x7fc00000u);
    }
  }
  return r;
}

double pow(double x, double y) {
  uint64_t ix = __f64_bits(x), iy = __f64_bits(y);
  double r;

  if (iy == 0 || x == 1.0)
    return 1.0;
  if (__float64_isnan(x) || __float64_isnan(y))
    return x + y;
  if (x == 0.0) {
    if ((iy & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL)
      return x + y;
    if (y > 0.0)
      return (iy >> 63) ? INFINITY : 0.0;
    if (y < 0.0)
      return (iy >> 63) ? 0.0 : INFINITY;
    return 1.0;
  }
  if (__float64_isinf(y)) {
    if (fabs(x) == 1.0)
      return 1.0;
    if ((ix >> 63) == 0) {
      if (y > 0.0)
        return (fabs(x) > 1.0) ? INFINITY : 0.0;
      return (fabs(x) > 1.0) ? 0.0 : INFINITY;
    }
  }

  /* Fast path: integer exponent (common case in Lua) */
  {
    double fy = floor(y);
    if (y == fy) {
      long long n = (long long)fy;
      int neg = 0;
      if (n < 0) {
        neg = 1;
        n = -n;
      }
      double r_fast = 1.0;
      double b = x;
      while (n) {
        if (n & 1)
          r_fast *= b;
        b *= b;
        n >>= 1;
      }
      return neg ? 1.0 / r_fast : r_fast;
    }
  }

  r = log(fabs(x));
  r = exp(y * r);

  if (ix >> 63) {
    double fy = floor(y);
    if (y == fy) {
      uint64_t iy_int = (uint64_t)(long long)fy;
      if (iy_int & 1)
        r = -r;
    } else {
      return __f64_from_bits(0x7ff8000000000000ULL);
    }
  }
  return r;
}

/* ============================================================================
 * Trigonometric functions — minimax polynomial with range reduction
 * ============================================================================
 */

static const float __sinf_c[4] = {
    0.99999999999999991673f, -0.16666666666666665741f,
    0.0083333333333333332177f, -0.0001984126984126984127f};

static const float __cosf_c[4] = {1.0f, -0.5f, 0.041666666666666664353f,
                                  -0.0013888888888888889419f};

/* Range-reduce x into [-pi/4, pi/4], return the quadrant (0..3) */
static int __sinf_reduce(float *x) {
  const float pi = 3.14159265358979323846f;
  const float two_over_pi = 0.63661977236758134308f;
  int q;
  float y;

  if (fabsf(*x) < 1.0e6f) {
    q = (int)(*x * two_over_pi + (__f32_sign(*x) ? -0.5f : 0.5f));
    y = *x - (float)q * (pi / 2.0f);
  } else {
    float t = *x * two_over_pi;
    q = (int)(t + (__f32_sign(*x) ? -0.5f : 0.5f));
    y = *x - (float)q * 1.57079632679489661923f;
    y = y - (float)q * 6.12323399573676588613e-17f;
  }
  *x = y;
  return q & 3;
}

float sinf(float x) {
  float z, s;
  int q;

  if (__f32_abs_bits(x) >= 0x7f800000u)
    return x - x;

  q = __sinf_reduce(&x);
  z = x * x;

  s = x *
      (__sinf_c[0] + z * (__sinf_c[1] + z * (__sinf_c[2] + z * __sinf_c[3])));

  if (q & 1)
    s = copysignf(1.0f, s) - s;
  if (q & 2)
    s = -s;

  return s;
}

float cosf(float x) {
  float z, c;
  int q;

  if (__f32_abs_bits(x) >= 0x7f800000u)
    return x - x;

  q = __sinf_reduce(&x);
  q = (q + 1) & 3;

  z = x * x;
  c = __cosf_c[0] + z * (__cosf_c[1] + z * (__cosf_c[2] + z * __cosf_c[3]));

  if (q & 1)
    c = copysignf(1.0f, c) - c;
  if (q & 2)
    c = -c;

  return c;
}

float tanf(float x) {
  float s, c;
  if (__f32_abs_bits(x) >= 0x7f800000u)
    return x - x;
  s = sinf(x);
  c = cosf(x);
  if (c == 0.0f)
    return copysignf(INFINITY, s);
  return s / c;
}

float asinf(float x) {
  float z, r;
  uint32_t ix = __f32_bits(x);

  if (ix >= 0x7f800000u)
    return x - x;
  if (ix >= 0x3f800000u) {
    if (ix == 0x3f800000u)
      return (ix >> 31) ? -M_PI_2 : M_PI_2;
    if (ix > 0x3f800000u)
      return __f32_from_bits(0x7fc00000u);
  }

  z = x * x;
  r = x * (1.0f + z * (0.16666667163372039794921875f +
                       z * (0.07500000298023223876953125f +
                            z * 0.0446428574621677398681640625f)));
  return r;
}

float acosf(float x) { return M_PI_2 - asinf(x); }

float atanf(float x) {
  float z, r;
  uint32_t ix = __f32_bits(x);
  int sign = ix >> 31;

  if (ix >= 0x7f800000u) {
    if (ix == 0x7f800000u)
      return sign ? -M_PI_2 : M_PI_2;
    return x - x;
  }

  x = fabsf(x);

  if (x > 1.0f) {
    x = 1.0f / x;
    z = x * x;
    r = M_PI_2 -
        x * (1.0f + z * (-0.3333333432674407958984375f +
                         z * (0.199999988079071044921875f +
                              z * (-0.14285714924335479736328125f +
                                   z * 0.111111111938953399658203125f))));
    return sign ? -r : r;
  }

  z = x * x;
  r = x * (1.0f + z * (-0.3333333432674407958984375f +
                       z * (0.199999988079071044921875f +
                            z * (-0.14285714924335479736328125f +
                                 z * 0.111111111938953399658203125f))));
  return sign ? -r : r;
}

float atan2f(float y, float x) {
  uint32_t ix = __f32_bits(x), iy = __f32_bits(y);

  if (__float32_isnan(x) || __float32_isnan(y))
    return x + y;
  if (iy == 0)
    return (ix >> 31) ? (ix == 0x80000000u ? M_PI : 0.0f) : 0.0f;
  if (ix == 0)
    return (iy >> 31) ? -M_PI_2 : M_PI_2;

  if (__float32_isinf(x)) {
    if (__float32_isinf(y)) {
      return (ix >> 31) ? ((iy >> 31) ? -2.35619449019234492885f
                                      : 2.35619449019234492885f)
                        : ((iy >> 31) ? -0.78539816339744830962f
                                      : 0.78539816339744830962f);
    }
    return (ix >> 31) ? ((iy >> 31) ? -M_PI : M_PI)
                      : ((iy >> 31) ? -0.0f : 0.0f);
  }
  if (__float32_isinf(y))
    return (iy >> 31) ? -M_PI_2 : M_PI_2;

  return atanf(y / x) + ((x < 0.0f) ? ((y < 0.0f) ? -M_PI : M_PI) : 0.0f);
}

/* Double-precision trig */
static const double __sin_c[5] = {
    1.0, -0.16666666666666665741, 0.0083333333333333332177,
    -0.0001984126984126984127, 0.0000027557319223985890653};

static const double __cos_c[5] = {1.0, -0.5, 0.041666666666666664353,
                                  -0.0013888888888888889419,
                                  0.000024801587301587301587};

static int __sin_reduce(double *x) {
  const double two_over_pi = 0.63661977236758134308;
  int q;
  double y;

  if (fabs(*x) < 1.0e14) {
    q = (int)(*x * two_over_pi + (__f64_sign(*x) ? -0.5 : 0.5));
    y = *x - (double)q * (M_PI / 2.0);
  } else {
    double t = *x * two_over_pi;
    q = (int)(t + (__f64_sign(*x) ? -0.5 : 0.5));
    y = *x - (double)q * 1.57079632679489661923;
    y = y - (double)q * 6.12323399573676588613e-17;
  }
  *x = y;
  return q & 3;
}

double sin(double x) {
  double z, s;
  int q;

  if (__f64_abs_bits(x) >= 0x7ff0000000000000ULL)
    return x - x;

  q = __sin_reduce(&x);
  z = x * x;

  s = x *
      (__sin_c[0] +
       z * (__sin_c[1] + z * (__sin_c[2] + z * (__sin_c[3] + z * __sin_c[4]))));

  if (q & 1)
    s = copysign(1.0, s) - s;
  if (q & 2)
    s = -s;

  return s;
}

double cos(double x) {
  double z, c;
  int q;

  if (__f64_abs_bits(x) >= 0x7ff0000000000000ULL)
    return x - x;

  q = __sin_reduce(&x);
  q = (q + 1) & 3;

  z = x * x;
  c = __cos_c[0] +
      z * (__cos_c[1] + z * (__cos_c[2] + z * (__cos_c[3] + z * __cos_c[4])));

  if (q & 1)
    c = copysign(1.0, c) - c;
  if (q & 2)
    c = -c;

  return c;
}

double tan(double x) {
  double s, c;
  if (__f64_abs_bits(x) >= 0x7ff0000000000000ULL)
    return x - x;
  s = sin(x);
  c = cos(x);
  if (c == 0.0)
    return copysign(INFINITY, s);
  return s / c;
}

double asin(double x) {
  double z, r;
  uint64_t ix = __f64_bits(x);

  if (ix >= 0x7ff0000000000000ULL)
    return x - x;
  if (ix >= 0x3ff0000000000000ULL) {
    if (ix == 0x3ff0000000000000ULL)
      return (ix >> 63) ? -M_PI_2 : M_PI_2;
    if (ix > 0x3ff0000000000000ULL)
      return __f64_from_bits(0x7ff8000000000000ULL);
  }

  z = x * x;
  r = x * (1.0 + z * (0.16666666666666665741 + z * (0.075000000000000000000 +
                                                    z * 0.044642857142857143)));
  return r;
}

double acos(double x) { return M_PI_2 - asin(x); }

double atan(double x) {
  double z, r;
  uint64_t ix = __f64_bits(x);
  int sign = (int)(ix >> 63);

  if (ix >= 0x7ff0000000000000ULL) {
    if (ix == 0x7ff0000000000000ULL)
      return sign ? -M_PI_2 : M_PI_2;
    return x - x;
  }

  x = fabs(x);

  if (x > 1.0) {
    x = 1.0 / x;
    z = x * x;
    r = M_PI_2 - x * (1.0 + z * (-0.33333333333333331483 +
                                 z * (0.19999999999999995559 +
                                      z * (-0.14285714285714287654 +
                                           z * 0.11111111111111110494))));
    return sign ? -r : r;
  }

  z = x * x;
  r = x *
      (1.0 +
       z * (-0.33333333333333331483 +
            z * (0.19999999999999995559 +
                 z * (-0.14285714285714287654 + z * 0.11111111111111110494))));
  return sign ? -r : r;
}

double atan2(double y, double x) {
  uint64_t ix = __f64_bits(x), iy = __f64_bits(y);

  if (__float64_isnan(x) || __float64_isnan(y))
    return x + y;
  if (iy == 0)
    return (ix >> 63) ? (ix == 0x8000000000000000ULL ? M_PI : 0.0) : 0.0;
  if (ix == 0)
    return (iy >> 63) ? -M_PI_2 : M_PI_2;

  if (__float64_isinf(x)) {
    if (__float64_isinf(y)) {
      return (ix >> 63) ? ((iy >> 63) ? -2.35619449019234492885
                                      : 2.35619449019234492885)
                        : ((iy >> 63) ? -0.78539816339744830962
                                      : 0.78539816339744830962);
    }
    return (ix >> 63) ? ((iy >> 63) ? -M_PI : M_PI) : ((iy >> 63) ? -0.0 : 0.0);
  }
  if (__float64_isinf(y))
    return (iy >> 63) ? -M_PI_2 : M_PI_2;

  return atan(y / x) + ((x < 0.0) ? ((y < 0.0) ? -M_PI : M_PI) : 0.0);
}

/* ============================================================================
 * String-to-float conversion
 * ============================================================================
 */

float strtof(const char *nptr, char **endptr) {
  return (float)strtod(nptr, endptr);
}

double strtod(const char *nptr, char **endptr) {
  const char *p = nptr;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' ||
         *p == '\v') {
    p++;
  }

  double sign = 1.0;
  if (*p == '-') {
    sign = -1.0;
    p++;
  } else if (*p == '+') {
    p++;
  }

  /* INF/INFINITY */
  if (strncasecmp(p, "infinity", 8) == 0) {
    if (endptr) *endptr = (char *)(p + 8);
    return sign * HUGE_VAL;
  }
  if (strncasecmp(p, "inf", 3) == 0) {
    if (endptr) *endptr = (char *)(p + 3);
    return sign * HUGE_VAL;
  }

  /* NAN */
  if (strncasecmp(p, "nan", 3) == 0) {
    p += 3;
    if (*p == '(') {
      const char *q = p + 1;
      while ((*q >= '0' && *q <= '9') ||
             (*q >= 'a' && *q <= 'z') ||
             (*q >= 'A' && *q <= 'Z') ||
             *q == '_') {
        q++;
      }
      if (*q == ')') {
        p = q + 1;
      }
    }
    if (endptr) *endptr = (char *)p;
    return sign < 0 ? -NAN : NAN;
  }

  /* Hexadecimal Float */
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    const char *hstart = p;
    const char *q = p + 2;
    double mant = 0.0;
    int any_digits = 0;

    while (1) {
      char c = *q;
      int d;
      if (c >= '0' && c <= '9')
        d = c - '0';
      else if (c >= 'a' && c <= 'f')
        d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        d = c - 'A' + 10;
      else
        break;
      mant = mant * 16.0 + (double)d;
      q++;
      any_digits = 1;
    }
    if (*q == '.') {
      q++;
      double scale = 1.0 / 16.0;
      while (1) {
        char c = *q;
        int d;
        if (c >= '0' && c <= '9')
          d = c - '0';
        else if (c >= 'a' && c <= 'f')
          d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
          d = c - 'A' + 10;
        else
          break;
        mant += (double)d * scale;
        scale /= 16.0;
        q++;
        any_digits = 1;
      }
    }

    if (any_digits) {
      int exp = 0;
      if (*q == 'p' || *q == 'P') {
        q++;
        int exp_sign = 1;
        if (*q == '-') {
          exp_sign = -1;
          q++;
        } else if (*q == '+') {
          q++;
        }
        int e = 0;
        while (*q >= '0' && *q <= '9') {
          e = e * 10 + (*q - '0');
          q++;
        }
        exp = exp_sign * e;
      }
      double result = ldexp(mant, exp);
      if (endptr)
        *endptr = (char *)q;
      return sign * result;
    }
    /* '0x' with no hex digits after it: not a valid hex-float. Fall back
     * to decimal parsing below, which will read the leading '0'. */
    p = hstart;
  }

  /* Decimal Path */
  unsigned long long mant = 0ULL;
  int mant_digits = 0;
  int frac_digits = 0;
  int seen_dot = 0;
  int has_digits = 0;
  while (1) {
    char c = *p;
    if (c >= '0' && c <= '9') {
      has_digits = 1;
      if (mant_digits < 19) {
        mant = mant * 10ULL + (unsigned long long)(c - '0');
        mant_digits++;
        if (seen_dot)
          frac_digits++;
      } else if (!seen_dot) {
        frac_digits--;
      }
      p++;
    } else if (c == '.' && !seen_dot) {
      seen_dot = 1;
      p++;
    } else {
      break;
    }
  }

  if (!has_digits) {
    if (endptr)
      *endptr = (char *)nptr;
    return 0.0;
  }

  int exp10 = 0;
  if (*p == 'e' || *p == 'E') {
    const char *estart = p;
    p++;
    int exp_sign = 1;
    if (*p == '-') {
      exp_sign = -1;
      p++;
    } else if (*p == '+') {
      p++;
    }
    if (*p >= '0' && *p <= '9') {
      int e = 0;
      while (*p >= '0' && *p <= '9') {
        e = e * 10 + (*p - '0');
        p++;
      }
      exp10 = exp_sign * e;
    } else {
      p = estart;
    }
  }

  double val = (double)mant;
  int total_exp = exp10 - frac_digits;
  {
    static const double p10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                 1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    int e = total_exp;
    if (e > 0) {
      while (e > 22) {
        val *= 1e22;
        e -= 22;
      }
      val *= p10[e];
    } else if (e < 0) {
      e = -e;
      while (e > 22) {
        val /= 1e22;
        e -= 22;
      }
      val /= p10[e];
    }
  }

  if (endptr) {
    *endptr = (char *)p;
  }

  return sign * val;
}

/* ============================================================================
 * Fixed-point (legacy — kept for Doom compat)
 * ============================================================================
 */

int32_t fixmul(int32_t a, int32_t b) {
  int64_t result = (int64_t)a * b;
  return (int32_t)(result >> FP_SHIFT);
}

int32_t sin_fp(int32_t x) {
  while (x > FP_PI)
    x -= FP_2PI;
  while (x < -FP_PI)
    x += FP_2PI;

  int32_t half_pi = 102944;
  if (x > half_pi)
    x = FP_PI - x;
  else if (x < -half_pi)
    x = -FP_PI - x;

  int32_t x2 = fixmul(x, x);
  int32_t x3 = fixmul(x2, x);
  int32_t x5 = fixmul(x3, x2);

  int32_t term1 = x;
  int32_t term2 = fixmul(x3, 10923);
  int32_t term3 = fixmul(x5, 546);

  return term1 - term2 + term3;
}

int32_t cos_fp(int32_t x) { return sin_fp(x + 102944); }