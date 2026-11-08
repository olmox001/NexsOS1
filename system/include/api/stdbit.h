/*
 * include/api/stdbit.h
 * C23 standard bit manipulation macros for NexsOS1.
 */

#ifndef _STDBIT_H
#define _STDBIT_H

#include <limits.h>

#ifndef ULLONG_WIDTH
#define ULLONG_WIDTH 64
#endif

#ifndef UINT_WIDTH
#define UINT_WIDTH 32
#endif

#define stdc_leading_zeros_ull(x) ((x) == 0 ? 64 : __builtin_clzll(x))
#define stdc_leading_zeros_ui(x)  ((x) == 0 ? 32 : __builtin_clz(x))
#define stdc_leading_zeros_ul(x)  ((x) == 0 ? (sizeof(unsigned long) * 8) : __builtin_clzl(x))
#define stdc_leading_zeros_us(x)  ((x) == 0 ? 16 : __builtin_clz((unsigned int)(x)) - (sizeof(unsigned int) * 8 - 16))
#define stdc_leading_zeros_uc(x)  ((x) == 0 ? 8 : __builtin_clz((unsigned int)(x)) - (sizeof(unsigned int) * 8 - 8))
#define stdc_leading_zeros(n)     ((sizeof(n) == 1) ? stdc_leading_zeros_uc((unsigned char)(n)) : \
                                  (sizeof(n) == sizeof(unsigned short)) ? stdc_leading_zeros_us((unsigned short)(n)) : \
                                  (sizeof(n) == sizeof(unsigned int)) ? stdc_leading_zeros_ui((unsigned int)(n)) : \
                                  (sizeof(n) == sizeof(unsigned long)) ? stdc_leading_zeros_ul((unsigned long)(n)) : \
                                  stdc_leading_zeros_ull((unsigned long long)(n)))
#define stdc_trailing_zeros_ull(x) ((x) == 0 ? 64 : __builtin_ctzll(x))
#define stdc_trailing_zeros_ui(x)  ((x) == 0 ? 32 : __builtin_ctz(x))
#define stdc_trailing_zeros_ul(x)  ((x) == 0 ? (sizeof(unsigned long) * 8) : __builtin_ctzl(x))
#define stdc_trailing_zeros_us(x)  ((x) == 0 ? 16 : __builtin_ctz((unsigned int)(x)))
#define stdc_trailing_zeros_uc(x)  ((x) == 0 ? 8 : __builtin_ctz((unsigned int)(x)))
#define stdc_trailing_zeros(n)     ((sizeof(n) == 1) ? stdc_trailing_zeros_uc((unsigned char)(n)) : \
                                  (sizeof(n) == sizeof(unsigned short)) ? stdc_trailing_zeros_us((unsigned short)(n)) : \
                                  (sizeof(n) == sizeof(unsigned int)) ? stdc_trailing_zeros_ui((unsigned int)(n)) : \
                                  (sizeof(n) == sizeof(unsigned long)) ? stdc_trailing_zeros_ul((unsigned long)(n)) : \
                                  stdc_trailing_zeros_ull((unsigned long long)(n)))
#define stdc_count_ones_ull(x)     __builtin_popcountll(x)
#define stdc_count_ones_ui(x)      __builtin_popcount(x)
#define stdc_bit_width_ull(x)      ((x) == 0 ? 0 : 64 - stdc_leading_zeros_ull(x))
#define stdc_bit_width_ui(x)       ((x) == 0 ? 0 : 32 - stdc_leading_zeros_ui(x))
#define stdc_bit_width_ul(x)       ((x) == 0 ? 0 : (sizeof(unsigned long) * 8) - stdc_leading_zeros_ul(x))
#define stdc_bit_width(x)          ((sizeof(x) == sizeof(unsigned int)) ? stdc_bit_width_ui((unsigned int)(x)) : \
                                   (sizeof(x) == sizeof(unsigned long)) ? stdc_bit_width_ul((unsigned long)(x)) : \
                                   stdc_bit_width_ull((unsigned long long)(x)))

static inline unsigned long long stdc_rotate_right_ull(unsigned long long x, unsigned int n) {
    n &= 63;
    return n == 0 ? x : (x >> n) | (x << (64 - n));
}

static inline unsigned long long stdc_rotate_left_ull(unsigned long long x, unsigned int n) {
    n &= 63;
    return n == 0 ? x : (x << n) | (x >> (64 - n));
}

#define stdc_rotate_right(x, n) stdc_rotate_right_ull((unsigned long long)(x), (n))
#define stdc_rotate_left(x, n)  stdc_rotate_left_ull((unsigned long long)(x), (n))

#endif /* _STDBIT_H */

