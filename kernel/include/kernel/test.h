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
#define KASSERT(cond) \
    if (!(cond)) { \
        printk("[KTEST] FAIL: %s:%d: Assertion failed: %s\n", __FILE__, __LINE__, #cond); \
        return; \
    }

#define KASSERT_EQ(a, b) KASSERT((a) == (b))

/* Runner API */
void ktest_run_all(void);

#endif /* _KERNEL_TEST_H */
