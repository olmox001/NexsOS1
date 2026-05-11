/*
 * kernel/lib/ktest.c
 * Kernel Unit Test Runner
 */
#include <kernel/test.h>
#include <kernel/printk.h>

extern ktest_case_t __ktests_start[];
extern ktest_case_t __ktests_end[];

void ktest_run_all(void) {
    size_t count = __ktests_end - __ktests_start;
    size_t passed = 0;

    printk("\n[KTEST] Starting Kernel Unit Tests (%d cases found)...\n", (int)count);

    for (ktest_case_t *test = __ktests_start; test < __ktests_end; test++) {
        printk("[KTEST] Running: %s... ", test->name);
        
        /* Note: This is a simplified runner. In a real system, we might want 
         * to use setjmp/longjmp for crash recovery in tests. 
         */
        test->func();
        
        printk("PASS\n");
        passed++;
    }

    printk("[KTEST] Completed. Summary: %d PASSED, %d FAILED\n\n", 
           (int)passed, (int)(count - passed));
}
