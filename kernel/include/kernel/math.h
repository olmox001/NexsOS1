/*
 * kernel/include/kernel/math.h
 * Fixed-Point Math Functions for Kernel (No FPU)
 *
 * Uses 16.16 fixed-point representation.
 */
#ifndef _KERNEL_MATH_H
#define _KERNEL_MATH_H

#include <kernel/types.h>

/* Fixed-point constants (16.16) */
#define FP_SHIFT 16
#define FP_ONE (1 << FP_SHIFT)
#define FP_HALF (1 << (FP_SHIFT - 1))
#define FP_PI 411775 /* 3.14159 * 131072 */

/* Integer math */
uint32_t k_isqrt(uint32_t n);

/* Fixed-point math (16.16) */
int32_t k_sqrt_fp(int32_t x);
int32_t k_fixmul(int32_t a, int32_t b);
int32_t k_fixdiv(int32_t a, int32_t b);
int32_t k_int_to_fp(int32_t x);
int32_t k_fp_to_int(int32_t x);
int32_t k_fp_to_int_round(int32_t x);
int32_t k_fabs_fp(int32_t x);
int32_t k_floor_fp(int32_t x);
int32_t k_ceil_fp(int32_t x);
int32_t k_sin_fp(int32_t x);
int32_t k_cos_fp(int32_t x);
int32_t k_lerp_fp(int32_t a, int32_t b, int32_t t);

/* Aliases for shared usage */
int32_t fixmul(int32_t a, int32_t b);
int32_t sin_fp(int32_t x);
int32_t cos_fp(int32_t x);
int32_t lerp_fp(int32_t a, int32_t b, int32_t t);

#endif /* _KERNEL_MATH_H */
