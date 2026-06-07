/*
 * kernel/lib/ktest_samples.c
 * Sample unit tests
 *
 * Purpose:
 *   Provides three minimal KTEST_CASE entries that exercise the string library
 *   and a trivial arithmetic sanity check.  These are proof-of-concept examples
 *   for the ktest framework and run unconditionally at every boot via
 *   ktest_run_all() (kernel/main.c:87).
 *
 * Role:
 *   - test_string_length: verifies strlen() for a non-empty and an empty string.
 *   - test_string_compare: verifies strcmp() for equal and unequal strings.
 *   - test_math_basic: verifies compiler arithmetic (integer add).
 *
 * KTEST_CASE expansion (from kernel/include/kernel/test.h):
 *   Each KTEST_CASE(name) macro emits a ktest_case_t descriptor into the
 *   `.ktests` ELF section and defines the test body as `void name(void)`.
 *   The descriptor is collected at link time between __ktests_start and
 *   __ktests_end for ktest_run_all() to iterate.
 *
 * KASSERT_EQ / KASSERT failure:
 *   If any assertion fails, the KASSERT macro prints a failure message, sets
 *   ktest_test_failed, and returns from the test function; the runner then
 *   counts the test as FAILED (LIB-KTEST-01 fixed).
 *
 * Known issues:
 *   None specific to this file.  Test reporting accuracy depends on the runner
 *   (see LIB-KTEST-01 in kernel/lib/ktest.c and docs/review/analysis/07-lib-headers.md).
 */
#include <kernel/test.h>
#include <kernel/string.h>

/* test_string_length - verify strlen() for a literal and an empty string.
 * Failure: KASSERT_EQ prints the mismatch and returns (see LIB-KTEST-01). */
KTEST_CASE(test_string_length) {
    KASSERT_EQ(strlen("hello"), 5);
    KASSERT_EQ(strlen(""), 0);
}

/* test_string_compare - verify strcmp() equality and inequality paths.
 * strcmp("abc","abc") must return 0; strcmp("abc","abd") must be non-zero. */
KTEST_CASE(test_string_compare) {
    KASSERT_EQ(strcmp("abc", "abc"), 0);
    KASSERT(strcmp("abc", "abd") != 0);
}

/* test_math_basic - sanity-check integer addition.
 * This test has no kernel-specific logic; it verifies the compiler and
 * runtime are not silently broken. */
KTEST_CASE(test_math_basic) {
    int a = 10;
    int b = 20;
    KASSERT_EQ(a + b, 30);
}
