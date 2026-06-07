/*
 * kernel/lib/ktest.c
 * Kernel Unit Test Runner
 *
 * Purpose:
 *   Provides ktest_run_all(), which iterates over all KTEST_CASE() entries
 *   registered in the `.ktests` ELF section and runs each test function.
 *   Called unconditionally from kernel/main.c:87 during boot.
 *
 * Role:
 *   The KTEST_CASE() macro (kernel/include/kernel/test.h) places a ktest_case_t
 *   descriptor into the `.ktests` ELF section:
 *     __attribute__((used, section(".ktests"))) static const ktest_case_t ...;
 *   The linker script collects all such descriptors between the symbols
 *   __ktests_start and __ktests_end.  ktest_run_all() walks this range.
 *
 * KASSERT behaviour:
 *   KASSERT(cond) in test.h expands to:
 *     if (!(cond)) { printk(...FAIL...); ktest_test_failed = 1; return; }
 *   When a KASSERT fires, the test function sets ktest_test_failed and returns
 *   early.  ktest_run_all() clears the flag before each test and checks it
 *   after test->func() to record a real PASS or FAIL.
 *
 * Fixed issues:
 *   LIB-KTEST-01  (W3 BUG, FIXED)  ktest_run_all() previously printed "PASS"
 *                and incremented `passed` after test->func() returned, regardless
 *                of whether the test exited via KASSERT.  Now KASSERT sets the
 *                ktest_test_failed flag (test.h); the runner clears it before
 *                each test and, after test->func(), records a real PASS or FAIL.
 *                The summary now reports accurate PASSED / FAILED counts.
 */
#include <kernel/test.h>
#include <kernel/printk.h>

/* __ktests_start / __ktests_end: linker-defined symbols bracketing the
 * .ktests ELF section that holds all KTEST_CASE() descriptors.
 * The pointer arithmetic (__ktests_end - __ktests_start) gives the test count. */
extern ktest_case_t __ktests_start[];
extern ktest_case_t __ktests_end[];

/* LIB-KTEST-01: a test sets this (via KASSERT in test.h) when an assertion
 * fails.  ktest_run_all() clears it before each test and checks it after. */
volatile int ktest_test_failed = 0;

/*
 * ktest_run_all - run every KTEST_CASE registered in the .ktests section.
 *
 * Iterates from __ktests_start to __ktests_end.  For each entry:
 *   1. Prints "[KTEST] Running: <name>... ".
 *   2. Clears ktest_test_failed, then calls test->func().
 *   3. Prints "PASS" or "FAIL" per the ktest_test_failed flag and counts it.
 * After all tests, prints the pass/fail summary.
 *
 * LIB-KTEST-01 (fixed): step 3 checks the ktest_test_failed flag set by KASSERT,
 *   so a test that returned early is counted as FAILED and the summary reports
 *   accurate PASSED / FAILED counts.
 *
 * No crash recovery: if a test triggers a kernel panic or fault, the machine
 * halts.  The comment "setjmp/longjmp" notes the direction for future hardening.
 *
 * Locking: none; called single-threaded from kernel/main.c before SMP starts.
 * Side effects: writes to UART via printk; calls all registered test functions.
 */
void ktest_run_all(void) {
    size_t count = __ktests_end - __ktests_start;
    size_t passed = 0;
    size_t failed = 0;

    printk("\n[KTEST] Starting Kernel Unit Tests (%d cases found)...\n", (int)count);

    for (ktest_case_t *test = __ktests_start; test < __ktests_end; test++) {
        printk("[KTEST] Running: %s... ", test->name);

        /* Note: This is a simplified runner. In a real system, we might want
         * to use setjmp/longjmp for crash recovery in tests.
         */
        ktest_test_failed = 0;
        test->func();

        /* LIB-KTEST-01: a test that returned early via KASSERT set
         * ktest_test_failed; count it as FAILED instead of an unconditional PASS. */
        if (ktest_test_failed) {
            printk("FAIL\n");
            failed++;
        } else {
            printk("PASS\n");
            passed++;
        }
    }

    printk("[KTEST] Completed. Summary: %d PASSED, %d FAILED\n\n",
           (int)passed, (int)failed);
}
