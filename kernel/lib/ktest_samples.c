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
#include <kernel/gfx_chrome.h>
#include <kernel/gfx_surface.h>
#include <kernel/vfs.h>
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

/* test_gfx_chrome_contract - test the window-chrome primitives extracted from
 * the compositor: rounded-rect membership, per-type shadow margins, the
 * button geometry shared by paint and hit-test, and clip honouring of the
 * solid shadow painter on caller-owned memory. */
KTEST_CASE(test_gfx_chrome_contract) {
    /* Rounded-rect predicate: r<=0 always inside; the sharp corner pixel of a
     * rounded rect is outside while the centre stays inside. */
    KASSERT(gfx_rrect_contains(0, 0, 8, 8, 0));
    KASSERT(!gfx_rrect_contains(0, 0, 8, 8, 4));
    KASSERT(gfx_rrect_contains(4, 4, 8, 8, 4));

    /* Shadow margins are pure data per shadow_type; size 0 means no fringe. */
    gfx_chrome_margins_t m;
    gfx_chrome_shadow_margins(2, 4, &m); /* premium: 2*so around, 3*so below */
    KASSERT_EQ(m.left, 8);
    KASSERT_EQ(m.bottom, 12);
    gfx_chrome_shadow_margins(1, 4, &m); /* fast: symmetric spread */
    KASSERT_EQ(m.top, 4);
    gfx_chrome_shadow_margins(0, 0, &m);
    KASSERT_EQ(m.right, 0);

    /* Button geometry: right-side circle layout; hit-test and paint share it. */
    gfx_button_geometry_t g;
    gfx_chrome_button_geometry(100, 200, 50, 20, 0, 1, &g);
    KASSERT_EQ(g.size, 16);
    KASSERT_EQ(g.close_cx, 288); /* 100 + 200 - 4 - 16/2 */
    KASSERT_EQ(g.bg_cx, 266);    /* close_cx - size - gap */
    KASSERT_EQ(gfx_chrome_button_hit(&g, g.close_cx, g.top + 1),
               GFX_BUTTON_CLOSE);
    KASSERT_EQ(gfx_chrome_button_hit(&g, g.bg_cx, g.top + 1),
               GFX_BUTTON_BACKGROUND);
    KASSERT_EQ(gfx_chrome_button_hit(&g, 100, g.top + 1), GFX_BUTTON_NONE);

    /* Solid shadow honours the exclusive clip rect on caller-owned pixels. */
    uint32_t px[16] = {0};
    gfx_surface_t s = {.width = 4, .height = 4, .stride = 4,
                       .buffer = px, .alpha_mask = NULL};
    gfx_rect_t clip = {.x = 0, .y = 0, .width = 4, .height = 2};
    gfx_rect_t frame = {.x = 1, .y = 1, .width = 2, .height = 2};
    gfx_chrome_shadow_solid(&s, &clip, &frame, 0, 0xFF123456);
    KASSERT_EQ(px[5], 0xFF123456); /* (1,1): inside frame and clip */
    KASSERT_EQ(px[9], 0);          /* (1,2): inside frame, clipped out */
}

/* --- Synthetic RAM provider for the VFS lifecycle test.  One file slot
 * ("/f"), no allocation, no disk: exercises the vfs.c plumbing (mount_at,
 * resolve, create, write, read, stat, unlink, umount) end to end. --- */
static char tfs_data[64];
static uint32_t tfs_size;
static int tfs_exists;
static int tfs_umount_ran;

static int tfs_open(struct vfs_mount *mnt, const char *path,
                    struct vfs_node *out) {
    if (strcmp(path, "/") == 0) {
        out->mnt = mnt; out->id = 0; out->size = 0; out->type = VFS_TYPE_DIR;
        return 0;
    }
    if (tfs_exists && strcmp(path, "/f") == 0) {
        out->mnt = mnt; out->id = 1; out->size = tfs_size;
        out->type = VFS_TYPE_FILE;
        return 0;
    }
    return -1;
}
static int tfs_read(struct vfs_node *node, uint64_t offset, void *buf,
                    uint32_t size) {
    if (node->id != 1 || !tfs_exists || offset >= tfs_size)
        return 0;
    uint32_t n = tfs_size - (uint32_t)offset;
    if (n > size)
        n = size;
    memcpy(buf, tfs_data + offset, n);
    return (int)n;
}
static int tfs_write(struct vfs_mount *mnt, const char *path, uint64_t offset,
                     const void *buf, uint32_t size) {
    (void)mnt;
    if (!tfs_exists || strcmp(path, "/f") != 0)
        return -1;
    if (offset + size > sizeof(tfs_data))
        return -1;
    memcpy(tfs_data + offset, buf, size);
    if (offset + size > tfs_size)
        tfs_size = (uint32_t)(offset + size);
    return (int)size;
}
static int tfs_create(struct vfs_mount *mnt, const char *path,
                      uint32_t type) {
    (void)mnt;
    if (type != VFS_TYPE_FILE || strcmp(path, "/f") != 0)
        return -1;
    tfs_exists = 1;
    tfs_size = 0;
    return 0;
}
static int tfs_unlink(struct vfs_mount *mnt, const char *path) {
    (void)mnt;
    if (!tfs_exists || strcmp(path, "/f") != 0)
        return -1;
    tfs_exists = 0;
    tfs_size = 0;
    return 0;
}
static int tfs_umount(struct vfs_mount *mnt) {
    (void)mnt;
    tfs_umount_ran = 1;
    return 0;
}
static const struct fs_ops tfs_ops = {
    .name = "ktestfs",
    .open = tfs_open,
    .read = tfs_read,
    .write = tfs_write,
    .create = tfs_create,
    .unlink = tfs_unlink,
    .umount = tfs_umount,
};

/* test_vfs_mount_lifecycle - mount → create → write → read → stat → unlink
 * → umount over a synthetic provider: the full Plan 9-namespace lifecycle
 * the userland-port programme depends on (docs/userland-port PLAN phase 3).
 * Also proves umount retires the slot (resolution falls back to root and
 * fails) and that a second umount is rejected. */
KTEST_CASE(test_vfs_mount_lifecycle) {
    tfs_exists = 0;
    tfs_size = 0;
    tfs_umount_ran = 0;

    KASSERT_EQ(vfs_mount_at("/ktestfs", &tfs_ops, NULL), 0);
    KASSERT_EQ(vfs_create("/ktestfs/f", VFS_TYPE_FILE), 0);
    KASSERT_EQ(vfs_write_file("/ktestfs/f", "nexs", 4, 0), 4);

    char rb[8] = {0};
    KASSERT_EQ(vfs_read_file("/ktestfs/f", rb, sizeof(rb), 0), 4);
    KASSERT_EQ(memcmp(rb, "nexs", 4), 0);

    struct vfs_stat st;
    KASSERT_EQ(vfs_stat("/ktestfs/f", &st), 0);
    KASSERT_EQ(st.size, 4);
    KASSERT_EQ(st.type, VFS_TYPE_FILE);

    KASSERT_EQ(vfs_unlink("/ktestfs/f"), 0);
    KASSERT(vfs_stat("/ktestfs/f", &st) != 0);

    KASSERT_EQ(vfs_umount("/ktestfs"), 0);
    KASSERT_EQ(tfs_umount_ran, 1);
    KASSERT(vfs_stat("/ktestfs/f", &st) != 0); /* falls back to root: gone */
    KASSERT(vfs_umount("/ktestfs") != 0);      /* double umount rejected */
    KASSERT(vfs_umount("/") != 0);             /* root umount refused */
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
