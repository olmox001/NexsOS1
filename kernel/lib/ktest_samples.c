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
#include <kernel/kmalloc.h>
#include <kernel/gfx_surface.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>

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

/* test_gfx_surface_contract - test the provider-neutral ARGB8888 facade using
 * caller-owned memory: clipping, source-over composition and ABGR conversion
 * all stay independent of the active GPU provider. */
KTEST_CASE(test_gfx_surface_contract) {
    uint32_t dst_pixels[16] = {0};
    uint32_t src_pixels[1] = {0x80FF0000};
    gfx_surface_t dst = {.width = 4, .height = 4, .stride = 4,
                         .buffer = dst_pixels, .alpha_mask = NULL};
    gfx_surface_t src = {.width = 1, .height = 1, .stride = 1,
                         .buffer = src_pixels, .alpha_mask = NULL};
    gfx_rect_t clipped = {.x = -1, .y = 1, .width = 3, .height = 1};

    KASSERT(gfx_surface_valid(&dst));
    gfx_surface_clear(&dst, 0xFF000000);
    gfx_surface_fill(&dst, &clipped, 0xFF00FF00);
    KASSERT_EQ(dst_pixels[4], 0xFF00FF00);
    KASSERT_EQ(dst_pixels[5], 0xFF00FF00);
    KASSERT_EQ(dst_pixels[6], 0xFF000000);

    gfx_surface_composite_over(&dst, &src, 3, 3);
    KASSERT_EQ(dst_pixels[15], 0xFF800000);

    dst_pixels[0] = 0xAA112233; /* ABGR: A=AA, B=11, G=22, R=33 */
    gl_swizzle_bgr(&dst);
    KASSERT_EQ(dst_pixels[0], 0xAA332211);
}

/* test_kmalloc_growth - prove the small-object pool grows past one chunk
 * (MM-KM-01).  1100 blocks in the 4096-byte bucket exceed a 4 MB chunk
 * regardless of how full the active chunk already is, so at least one
 * growth must succeed for every allocation to come back non-NULL.  Each
 * block is touched (write+readback) and everything is freed to the bucket
 * lists afterwards, where it stays reusable. */
#define KMG_BLOCKS 1100
static void *kmg_ptrs[KMG_BLOCKS];
KTEST_CASE(test_kmalloc_growth) {
    int i;
    for (i = 0; i < KMG_BLOCKS; i++) {
        kmg_ptrs[i] = kmalloc(4000);
        if (!kmg_ptrs[i]) break;
        ((uint8_t *)kmg_ptrs[i])[0] = (uint8_t)i;
        ((uint8_t *)kmg_ptrs[i])[3999] = (uint8_t)(i ^ 0xFF);
    }
    int allocated = i;
    int corrupt = 0;
    for (i = 0; i < allocated; i++) {
        if (((uint8_t *)kmg_ptrs[i])[0] != (uint8_t)i ||
            ((uint8_t *)kmg_ptrs[i])[3999] != (uint8_t)(i ^ 0xFF))
            corrupt++;
        kfree(kmg_ptrs[i]);
    }
    KASSERT_EQ(allocated, KMG_BLOCKS);
    KASSERT_EQ(corrupt, 0);
}

/* test_vmm_protect - prove arch_vmm_protect rewrites live PTEs (AMMU-02).
 * Takes a fresh PMM page (mapped RW+NX inside a 2MB kernel RAM block),
 * flips it to PAGE_KERNEL_RO and back, and checks via PTE readback
 * (vmm_check_range) and PA readback (vmm_get_phys) that only the attribute
 * bits changed.  Exercises the 2MB→4KB block split on both arches and the
 * unmapped-hole error path on the NULL page.  Runs on the BSP before SMP,
 * so splitting the live kernel map here is single-threaded. */
KTEST_CASE(test_vmm_protect) {
    extern uint64_t *kernel_pgd;
    uint8_t *page = (uint8_t *)pmm_alloc_page();
    KASSERT(page != NULL);
    page[0] = 0xAB; /* RW works while PAGE_KERNEL */
    uint64_t pa_before = vmm_get_phys(kernel_pgd, (uint64_t)page);
    KASSERT(pa_before != 0);

    /* Hole detection: a high user-half VA no map ever touches (the NULL
     * page would not do — amd64 identity-maps the low 1MB for the SMP
     * trampoline, so VA 0 is mapped there). */
    KASSERT(vmm_protect(kernel_pgd, 0x700000000000UL, 4096, PAGE_KERNEL_RO) != 0);

    /* RW+NX → RO+NX */
    KASSERT_EQ(vmm_protect(kernel_pgd, (uint64_t)page, 4096, PAGE_KERNEL_RO), 0);
#ifdef ARCH_AARCH64
    /* AP[7:6] = 0b10 (EL1 read-only) must now be set */
    KASSERT_EQ(vmm_check_range(kernel_pgd, (uint64_t)page, 4096, PTE_AP_EL1_RO), 0);
#else
    /* NX must be set, and the RW bit must be GONE */
    KASSERT_EQ(vmm_check_range(kernel_pgd, (uint64_t)page, 4096, PTE_NX), 0);
    KASSERT(vmm_check_range(kernel_pgd, (uint64_t)page, 4096, PTE_RW) != 0);
#endif
    /* Frame address preserved; content still readable */
    KASSERT_EQ(vmm_get_phys(kernel_pgd, (uint64_t)page), pa_before);
    KASSERT_EQ(page[0], 0xAB);

    /* back to RW+NX, prove writes work again */
    KASSERT_EQ(vmm_protect(kernel_pgd, (uint64_t)page, 4096, PAGE_KERNEL), 0);
#ifndef ARCH_AARCH64
    KASSERT_EQ(vmm_check_range(kernel_pgd, (uint64_t)page, 4096, PTE_RW), 0);
#endif
    page[1] = 0xCD;
    KASSERT_EQ(page[0], 0xAB);
    KASSERT_EQ(page[1], 0xCD);
    pmm_free_page(page);
}
