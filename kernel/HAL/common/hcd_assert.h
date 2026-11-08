#ifndef NXEC_HCD_ASSERT_H
#define NXEC_HCD_ASSERT_H

/*
 * Freestanding invariant assertion. The expression must be side-effect free.
 * Clang and GCC lower __builtin_trap without a hosted C runtime.
 */
#define HCD_ASSERT(expression) \
    do { \
        if (!(expression)) { \
            __builtin_trap(); \
        } \
    } while (0)

#endif
