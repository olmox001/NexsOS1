/*
 * kernel/lib/ktest_samples.c
 * Sample unit tests
 */
#include <kernel/test.h>
#include <kernel/string.h>

KTEST_CASE(test_string_length) {
    KASSERT_EQ(strlen("hello"), 5);
    KASSERT_EQ(strlen(""), 0);
}

KTEST_CASE(test_string_compare) {
    KASSERT_EQ(strcmp("abc", "abc"), 0);
    KASSERT(strcmp("abc", "abd") != 0);
}

KTEST_CASE(test_math_basic) {
    int a = 10;
    int b = 20;
    KASSERT_EQ(a + b, 30);
}
