/*
 * kernel/include/kernel/test.h
 * Lightweight In-Kernel Unit Testing Framework
 */
#ifndef _KERNEL_TEST_H
#define _KERNEL_TEST_H

#include <kernel/types.h>
#include <kernel/printk.h>

typedef struct {
    const char *name;
    void (*func)(void);
} ktest_case_t;

/* LIB-KTEST-01: set to 1 by KASSERT when an assertion fails, so ktest_run_all()
 * can tell a real pass from an early return.  Defined in kernel/lib/ktest.c. */
extern volatile int ktest_test_failed;

/* 
 * Test Case Declaration Macro 
 * We use a special section to collect all test cases 
 */
#define KTEST_CASE(test_name) \
    void test_name(void); \
    __attribute__((used, section(".ktests"))) \
    static const ktest_case_t _test_##test_name = { #test_name, test_name }; \
    void test_name(void)

/* Assertions */
/* KASSERT — wrapped in do{}while(0) because the bare `if` it used to be is a
 * dangling-else trap: `if (c) KASSERT(x); else y;` bound the else to KASSERT's
 * own if, silently inverting the test.  The compiler cannot warn about a macro
 * that expands to a syntactically valid wrong thing, which is exactly the class
 * nx_contract.h exists for. */
#define KASSERT(cond) \
    do { \
        if (!(cond)) { \
            printk("[KTEST] FAIL: %s:%d: Assertion failed: %s\n", __FILE__, \
                   __LINE__, #cond); \
            ktest_test_failed = 1; \
            return; \
        } \
    } while (0)

#define KASSERT_EQ(a, b) KASSERT((a) == (b))

/* Runner API */
void ktest_run_all(void);

#endif /* _KERNEL_TEST_H */
