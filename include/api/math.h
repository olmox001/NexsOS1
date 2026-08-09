#ifndef _MATH_H
#define _MATH_H

#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * NexsOS1 Math Library — IEEE-754 single-precision (float) and double
 *
 * Built for bare-metal userland (-nostdlib -fno-builtin -ffreestanding).
 * No libgcc/libm dependency; all algorithms are self-contained.
 *
 * Coverage:
 *   - float:  sqrtf, fabsf, floorf, ceilf, fmodf, powf, expf, logf,
 *             sinf, cosf, tanf, asinf, acosf, atanf, atan2f,
 *             fminf, fmaxf, roundf, truncf
 *   - double: sqrt, fabs, floor, ceil, fmod, pow, exp, log,
 *             sin, cos, tan, asin, acos, atan, atan2,
 *             fmin, fmax, round, trunc
 *   - int:    abs, labs
 *   - fixed:  sin_fp, cos_fp, fixmul (legacy 16.16, kept for compat)
 * ============================================================================
 */

/* -------------------------------------------------------------------------- */
/* Constants                                                                  */
/* -------------------------------------------------------------------------- */
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_E 2.71828182845904523536
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402

/* -------------------------------------------------------------------------- */
/* Integer absolute value                                                     */
/* -------------------------------------------------------------------------- */
int abs(int x);
long labs(long j);

/* -------------------------------------------------------------------------- */
/* Fixed-point (legacy 16.16 — kept for Doom and existing code)              */
/* -------------------------------------------------------------------------- */
#define FP_SHIFT 16
#define FP_ONE (1 << FP_SHIFT)
#define FP_HALF (1 << (FP_SHIFT - 1))
#define FP_PI 205887  /* π × 2^16 */
#define FP_2PI 411775 /* 2π × 2^16 */
#define DEG_TO_FP_RAD(d) (((d) * 1144))

int32_t sin_fp(int32_t x);
int32_t cos_fp(int32_t x);
int32_t fixmul(int32_t a, int32_t b);

/* -------------------------------------------------------------------------- */
/* Float classification & manipulation                                        */
/* -------------------------------------------------------------------------- */
#define NAN __builtin_nanf("")
#define INFINITY __builtin_inff()
#define HUGE_VALF INFINITY
#define HUGE_VAL ((double)INFINITY)

int __float32_isnan(float x);
int __float32_isinf(float x);
int __float64_isnan(double x);
int __float64_isinf(double x);

#define isnan(x)                                                               \
  _Generic((x),                                                                \
      float: __float32_isnan,                                                  \
      double: __float64_isnan,                                                 \
      default: __float64_isnan)(x)
#define isinf(x)                                                               \
  _Generic((x),                                                                \
      float: __float32_isinf,                                                  \
      double: __float64_isinf,                                                 \
      default: __float64_isinf)(x)
#define isfinite(x) (!isinf(x) && !isnan(x))

/* -------------------------------------------------------------------------- */
/* Single-precision float (float)                                            */
/* -------------------------------------------------------------------------- */
float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float roundf(float x);
float truncf(float x);
float fmodf(float x, float y);

float powf(float x, float y);
float expf(float x);
float exp2f(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);

float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);

float fminf(float x, float y);
float fmaxf(float x, float y);
float copysignf(float x, float y);
float frexpf(float x, int *exp);
float ldexpf(float x, int exp);

/* -------------------------------------------------------------------------- */
/* Double-precision (double)                                                 */
/* -------------------------------------------------------------------------- */
double sqrt(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double trunc(double x);
double fmod(double x, double y);

double pow(double x, double y);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

double fmin(double x, double y);
double fmax(double x, double y);
double copysign(double x, double y);
double frexp(double x, int *exp);
double ldexp(double x, int exp);

/* -------------------------------------------------------------------------- */
/* Float/Double conversion                                                    */
/* -------------------------------------------------------------------------- */
float strtof(const char *nptr, char **endptr);
double strtod(const char *nptr, char **endptr);

#endif /* _MATH_H */