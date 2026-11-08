/*
 * include/api/stdckdint.h
 * C23 Checked integer arithmetic macros.
 */

#ifndef _STDCKDINT_H
#define _STDCKDINT_H

#define ckd_add(result, a, b) __builtin_add_overflow((a), (b), (result))
#define ckd_sub(result, a, b) __builtin_sub_overflow((a), (b), (result))
#define ckd_mul(result, a, b) __builtin_mul_overflow((a), (b), (result))

#endif /* _STDCKDINT_H */
