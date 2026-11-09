/*
 * kernel/lib/ktest_samples.c
 * Sample unit tests
 *
 * Purpose:
 *   Provides core string/arithmetic, PMM memory-map, architecture-specific
 *   boot-reservation, graphics, VFS, allocator and VMM checks. They run at boot
 *   via ktest_run_all() (kernel/main.c).
 *
 * Role:
 *   - test_string_length: verifies strlen() for a non-empty and an empty string.
 *   - test_string_compare: verifies strcmp() for equal and unequal strings.
 *   - test_math_basic: verifies compiler arithmetic (integer add).
 *   - test_timer_rearm_cancel: verifies per-CPU timer re-arm, unlink and
 *     idempotent synchronous cancellation before callback data leaves scope.
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
#include <hal/boot_memory.h>
#include <kernel/test.h>
#include <kernel/arch.h>
#include <kernel/boot_memory.h>
#include <kernel/platform.h>
#include <kernel/memlayout.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/kmalloc.h>
#include <kernel/gfx_chrome.h>
#include <kernel/gfx_surface.h>
#include <kernel/vfs.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <drivers/input.h>
#include <drivers/timer.h>
#include <drivers/virtio_input.h>
#if HAL_HAS_FDT
#include <kernel/fdt.h>
#include <kernel/fdt_parser.h>
#endif

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

/* The callback data deliberately lives on this test's stack. The test queues,
 * re-arms and cancels before returning; timer_del must unlink the node and
 * leave no callback able to access the stack data after the test exits. */
static void ktest_timer_count_callback(void *data) {
    int *count = (int *)data;
    __atomic_add_fetch(count, 1, __ATOMIC_RELAXED);
}

KTEST_CASE(test_timer_rearm_cancel) {
    struct OS1libkernel_timer timer;
    int callback_count = 0;
    uint64_t now = OS1libkernel_time_monotonic_ns();

    OS1libkernel_timer_setup(&timer, ktest_timer_count_callback, &callback_count);
    OS1libkernel_timer_arm_deadline(&timer, now + UINT64_C(1000000000));
    bool first_pending = OS1libkernel_timer_pending(&timer);
    OS1libkernel_timer_arm_deadline(&timer, now + UINT64_C(2000000000));
    bool rearmed_pending = OS1libkernel_timer_pending(&timer);

    OS1libkernel_timer_cancel(&timer);
    bool cancelled = !OS1libkernel_timer_pending(&timer);
    int owner = __atomic_load_n(&timer.cpu, __ATOMIC_ACQUIRE);
    bool unlinked = list_empty(&timer.list);
    OS1libkernel_timer_cancel(&timer);

    KASSERT(first_pending);
    KASSERT(rearmed_pending);
    KASSERT(cancelled);
    KASSERT_EQ(owner, -1);
    KASSERT(unlinked);
    KASSERT_EQ(__atomic_load_n(&callback_count, __ATOMIC_ACQUIRE), 0);
}

KTEST_CASE(test_time_snapshot_cpu0) {
    struct os1_time_snapshot_v1 snapshot;
    KASSERT_EQ(OS1libkernel_time_snapshot(&snapshot), 0);
    KASSERT_EQ(snapshot.abi_version, OS1_TIME_ABI_VERSION);
    KASSERT(snapshot.cpu_count >= 1U);
    KASSERT(snapshot.cpu_count <= OS1_TIME_MAX_CPUS);
    KASSERT(snapshot.counter_frequency_hz != 0U);
    KASSERT_EQ(snapshot.scheduler_tick_hz, OS1LIBKERNEL_TIMER_HZ);
    KASSERT_EQ(snapshot.cpu[0].cpu_id, 0U);
    KASSERT((snapshot.cpu[0].flags & OS1_TIME_CPU_ARMED) != 0U);
}

KTEST_CASE(test_time_snapshot_monotonic) {
    struct os1_time_snapshot_v1 first;
    struct os1_time_snapshot_v1 second;
    KASSERT_EQ(OS1libkernel_time_snapshot(&first), 0);
    KASSERT_EQ(OS1libkernel_time_snapshot(&second), 0);
    KASSERT(second.sequence > first.sequence);
    KASSERT(second.monotonic_ns >= first.monotonic_ns);
    KASSERT_EQ(second.cpu_record_size, sizeof(struct os1_time_cpu_v1));
}

KTEST_CASE(test_sched_quantum_not_due) {
    KASSERT(!OS1libkernel_sched_quantum_due(UINT64_C(100), UINT64_C(99)));
    KASSERT(!OS1libkernel_sched_quantum_due(UINT64_MAX, UINT64_MAX - 1U));
    KASSERT(OS1LIBKERNEL_SCHED_QUANTUM_NS >
            OS1LIBKERNEL_NSEC_PER_TICK);
}

KTEST_CASE(test_sched_quantum_due) {
    KASSERT(OS1libkernel_sched_quantum_due(UINT64_C(100), UINT64_C(100)));
    KASSERT(OS1libkernel_sched_quantum_due(UINT64_C(100), UINT64_C(101)));
    KASSERT(OS1libkernel_sched_quantum_due(UINT64_C(0), UINT64_C(0)));
    KASSERT_EQ(OS1LIBKERNEL_SCHED_QUANTUM_NS,
               UINT64_C(30) * OS1LIBKERNEL_NSEC_PER_MSEC);
}

KTEST_CASE(test_input_keyboard_queue) {
    struct os1_raw_input_event_v1 event;

    OS1driver_input_core_init();
    OS1driver_input_publish(INPUT_PROVIDER_LEGACY, UINT8_C(0xF0),
                            EV_KEY, KEY_A, 1);
    OS1driver_input_publish(INPUT_PROVIDER_LEGACY, UINT8_C(0xF0),
                            EV_SYN, 0, 0);
    KASSERT_EQ(OS1libkernel_input_read_raw(
                   &event, OS1_INPUT_ROLE_KEYBOARD, OS1_INPUT_READ_TRY), 1);
    KASSERT_EQ(event.type, EV_KEY);
    KASSERT_EQ(event.code, KEY_A);
    KASSERT_EQ(event.value, 1);
    KASSERT_EQ(OS1libkernel_input_read_raw(
                   &event, OS1_INPUT_ROLE_KEYBOARD, OS1_INPUT_READ_TRY), 1);
    KASSERT_EQ(event.type, EV_SYN);
    KASSERT_EQ(OS1libkernel_input_read_raw(
                   &event, OS1_INPUT_ROLE_KEYBOARD, OS1_INPUT_READ_TRY), 0);
    KASSERT_EQ(OS1libkernel_input_read_raw(
                   &event, OS1_INPUT_ROLE_MOUSE, OS1_INPUT_READ_TRY), 0);
}

KTEST_CASE(test_input_mouse_queue) {
    struct os1_raw_input_event_v1 event;

    OS1driver_input_core_init();
    OS1driver_input_publish(INPUT_PROVIDER_LEGACY, UINT8_C(0xF1),
                            EV_REL, REL_X, 7);
    OS1driver_input_publish(INPUT_PROVIDER_LEGACY, UINT8_C(0xF1),
                            EV_SYN, 0, 0);
    KASSERT_EQ(OS1libkernel_input_read_raw(
                   &event, OS1_INPUT_ROLE_MOUSE, OS1_INPUT_READ_TRY), 1);
    KASSERT_EQ(event.type, EV_REL);
    KASSERT_EQ(event.code, REL_X);
    KASSERT_EQ(event.value, 7);
    KASSERT_EQ(OS1libkernel_input_read_raw(
                   &event, OS1_INPUT_ROLE_MOUSE, OS1_INPUT_READ_TRY), 1);
    KASSERT_EQ(event.type, EV_SYN);
    KASSERT_EQ(OS1libkernel_input_read_raw(
                   &event, OS1_INPUT_ROLE_MOUSE, OS1_INPUT_READ_TRY), 0);
    KASSERT_EQ(OS1libkernel_input_read_raw(
                   &event, OS1_INPUT_ROLE_KEYBOARD, OS1_INPUT_READ_TRY), 0);
    KASSERT_EQ(OS1libkernel_input_read_raw(
                   &event, UINT32_C(99), OS1_INPUT_READ_TRY), -EINVAL);
    KASSERT_EQ(OS1libkernel_input_read_raw(
                   &event, OS1_INPUT_ROLE_MOUSE, UINT32_C(99)), -EINVAL);
}

/* The boot map may be unsorted and contain overlapping usable descriptors.
 * The PMM must count the union of complete pages, not sum overlapping lengths
 * or treat RESERVED overlays as additional RAM. */
KTEST_CASE(test_pmm_region_union) {
    struct mem_region regions[] = {
        {.base = 0x2000, .size = 0x3000, .type = MEM_REGION_USABLE},
        {.base = 0x3000, .size = 0x2000, .type = MEM_REGION_RESERVED},
        {.base = 0x1000, .size = 0x3000, .type = MEM_REGION_USABLE},
        {.base = 0x9000, .size = 0x1000, .type = MEM_REGION_USABLE},
    };
    KASSERT_EQ(pmm_calculate_usable_bytes(regions, 4, 0x1000, 0xA000),
               0x5000);
    KASSERT_EQ(pmm_calculate_usable_bytes(regions, 4, 0x2000, 0xA000),
               0x4000);
    /* Metadata is touched before the dynamic VMM map is installed. */
    KASSERT(pmm_metadata_top() != 0);
    KASSERT(pmm_metadata_top() <= HAL_BOOTSTRAP_PHYS_MAP_LIMIT);
}

KTEST_CASE(test_boot_memory_snapshot_consistent) {
    size_t count = 0U;
    struct mem_region *regions = boot_memory_regions(&count);
    uint32_t hal_count = nxhal_boot_memory_count();
    int usable = 0;

    KASSERT(regions != NULL);
    KASSERT(count != 0U);
    KASSERT(count <= (size_t)NXHAL_BOOT_MEMORY_MAX_RECORDS + 1U);
    if (hal_count != 0U)
        KASSERT(count >= (size_t)hal_count);

    for (size_t index = 0U; index < count; ++index) {
        KASSERT(regions[index].size != 0U);
        KASSERT(regions[index].base <= UINT64_MAX - regions[index].size);
        if (regions[index].type == MEM_REGION_USABLE)
            usable = 1;
    }
    KASSERT(usable);

    for (uint32_t index = 0U; index < hal_count; ++index) {
        struct nxhal_boot_memory_region source;
        KASSERT_EQ(nxhal_boot_memory_get(index, &source),
                   NXHAL_BOOT_MEMORY_OK);
        KASSERT_EQ(regions[index].base, source.base);
        KASSERT_EQ(regions[index].size, source.length);
        if (source.type == (uint32_t)NXHAL_BOOT_MEMORY_USABLE)
            KASSERT_EQ(regions[index].type, MEM_REGION_USABLE);
        else
            KASSERT(regions[index].type != MEM_REGION_USABLE);
    }
}

#if HAL_TEST_PCI_3G_HOLE
/* The QEMU 5 GiB layout has the PCI/MMIO aperture at 3-4 GiB. If a different
 * machine reports RAM there, the check is not applicable and is logged; on
 * the QEMU test configuration every PFN in the hole must be RESERVED. */
KTEST_CASE(test_amd64_pci_hole_reserved) {
    size_t count = 0;
    struct mem_region *regions = boot_memory_regions(&count);
    KASSERT(regions != NULL);
    KASSERT(count != 0);

    const uint64_t hole_start = 0xC0000000ULL;
    const uint64_t hole_end = 0x100000000ULL;
    int usable_overlap = 0;
    for (size_t i = 0; i < count; i++) {
        if (regions[i].type != MEM_REGION_USABLE ||
            regions[i].size == 0 ||
            regions[i].base > UINT64_MAX - regions[i].size)
            continue;
        uint64_t region_end = regions[i].base + regions[i].size;
        if (regions[i].base < hole_end && region_end > hole_start) {
            usable_overlap = 1;
            break;
        }
    }

    if (usable_overlap ||
        pmm_phys_to_page(hole_start) == NULL ||
        pmm_phys_to_page(hole_end - PAGE_SIZE) == NULL) {
        pr_info("%s", "KTEST: AMD64 3-4 GiB PCI-hole assertion not applicable "
                      "for this memory map/span\n");
        return;
    }

    for (uint64_t physical = hole_start; physical < hole_end;
         physical += PAGE_SIZE) {
        struct page *descriptor = pmm_phys_to_page(physical);
        KASSERT(descriptor != NULL);
        KASSERT((descriptor->flags & PG_RESERVED) != 0);
    }

    for (unsigned i = 0; i < 32U; i++) {
        void *allocated = pmm_alloc_page();
        KASSERT(allocated != NULL);
        uint64_t physical = virt_to_phys(allocated);
        int in_hole = physical >= hole_start && physical < hole_end;
        pmm_free_page(allocated);
        KASSERT(!in_hole);
    }
}
#else
/* Exercise the physical page scanner against a valid in-kernel FDT fixture.
 * The invalid magic+oversized-length candidate in page 0 must be skipped;
 * the valid blob in page 1 must be found by the 4 KiB page walk. */
static uint8_t fdt_search_test_area[3U * PAGE_SIZE]
    __attribute__((aligned(PAGE_SIZE)));
static const uint8_t fdt_search_test_blob[72] = {
    [0] = 0xD0, [1] = 0x0D, [2] = 0xFE, [3] = 0xED,
    [4] = 0x00, [5] = 0x00, [6] = 0x00, [7] = 0x48,
    [8] = 0x00, [9] = 0x00, [10] = 0x00, [11] = 0x38,
    [12] = 0x00, [13] = 0x00, [14] = 0x00, [15] = 0x48,
    [16] = 0x00, [17] = 0x00, [18] = 0x00, [19] = 0x28,
    [20] = 0x00, [21] = 0x00, [22] = 0x00, [23] = 0x11,
    [24] = 0x00, [25] = 0x00, [26] = 0x00, [27] = 0x10,
    [36] = 0x00, [37] = 0x00, [38] = 0x00, [39] = 0x10,
    [56] = 0x00, [57] = 0x00, [58] = 0x00, [59] = 0x01,
    [60] = 0x00,
    [64] = 0x00, [65] = 0x00, [66] = 0x00, [67] = 0x02,
    [68] = 0x00, [69] = 0x00, [70] = 0x00, [71] = 0x09,
};

KTEST_CASE(test_fdt_page_by_page_search) {
    memset(fdt_search_test_area, 0, sizeof(fdt_search_test_area));

    /* A false-positive header must not stop the scan. */
    const uint8_t invalid_header[8] = {
        0xD0, 0x0D, 0xFE, 0xED, 0xFF, 0xFF, 0xFF, 0xFF
    };
    memcpy(fdt_search_test_area + 64U, invalid_header,
           sizeof(invalid_header));

    const size_t blob_offset = PAGE_SIZE + 128U;
    memcpy(fdt_search_test_area + blob_offset, fdt_search_test_blob,
           sizeof(fdt_search_test_blob));

    uint64_t physical = virt_to_phys(fdt_search_test_area);
    KASSERT((physical & ((uint64_t)PAGE_SIZE - 1ULL)) == 0);
    KASSERT(physical >= HAL_RAM_START);
    KASSERT(physical <= HAL_PHYS_MAP_LIMIT - sizeof(fdt_search_test_area));

    size_t found_size = 0;
    uintptr_t found = fdt_find_in_memory(
        (uintptr_t)physical,
        (uintptr_t)(physical + sizeof(fdt_search_test_area)),
        &found_size);
    KASSERT_EQ(found, physical + blob_offset);
    KASSERT_EQ(found_size, sizeof(fdt_search_test_blob));

    /* Exercise the complete x0=0 legacy adapter. It temporarily activates
     * the first valid DTB in the original 1 GiB search window; restore the
     * firmware-provided DTB before allowing later KTESTs/boot stages to run. */
    extern uint64_t boot_fdt_ptr;
    extern uint64_t boot_fdt_size;
    uint64_t original_ptr = boot_fdt_ptr;
    uint64_t original_size = boot_fdt_size;
    KASSERT(original_ptr != 0 && original_size >= 40U);

    /* Remove the valid synthetic blob before the global legacy scan: the
     * fixture lives in kernel BSS, which itself lies inside the scan window. */
    memset(fdt_search_test_area, 0, sizeof(fdt_search_test_area));

    size_t legacy_size = 0;
    int legacy_status = fdt_init_legacy(0, &legacy_size);
    uint64_t legacy_ptr = boot_fdt_ptr;
    uint64_t activated_size = boot_fdt_size;
    int restore_status = fdt_init((uintptr_t)original_ptr,
                                  (size_t)original_size);

    KASSERT_EQ(legacy_status, FDT_PARSER_OK);
    KASSERT(legacy_ptr >= HAL_RAM_START);
    KASSERT(legacy_ptr < HAL_RAM_START + (1ULL << 30));
    KASSERT(legacy_size != 0);
    KASSERT_EQ(legacy_size, activated_size);
    KASSERT_EQ(restore_status, FDT_PARSER_OK);
    KASSERT_EQ(boot_fdt_ptr, original_ptr);
    KASSERT_EQ(boot_fdt_size, original_size);
}

/* The FDT allocation is part of the memory map's RESERVED overlays. Check
 * every PFN touched by the validated blob after PMM construction, then sample
 * allocator output to ensure the live allocator never returns a DTB PFN. */
KTEST_CASE(test_fdt_pages_reserved_from_pmm) {
    extern uint64_t boot_fdt_ptr;
    extern uint64_t boot_fdt_size;
    KASSERT(boot_fdt_ptr != 0);
    KASSERT(boot_fdt_size >= 40U);
    KASSERT(boot_fdt_ptr <= UINT64_MAX - boot_fdt_size);

    uint64_t dtb_end = boot_fdt_ptr + boot_fdt_size;
    uint64_t page = boot_fdt_ptr & ~((uint64_t)PAGE_SIZE - 1ULL);
    KASSERT(dtb_end <= UINT64_MAX - (PAGE_SIZE - 1ULL));
    uint64_t page_end =
        (dtb_end + PAGE_SIZE - 1ULL) & ~((uint64_t)PAGE_SIZE - 1ULL);
    for (; page < page_end; page += PAGE_SIZE) {
        struct page *descriptor = pmm_phys_to_page(page);
        KASSERT(descriptor != NULL);
        KASSERT((descriptor->flags & PG_RESERVED) != 0);
    }

    for (unsigned i = 0; i < 32U; i++) {
        void *allocated = pmm_alloc_page();
        KASSERT(allocated != NULL);
        uint64_t physical = virt_to_phys(allocated);
        int overlaps_dtb = physical >=
                (boot_fdt_ptr & ~((uint64_t)PAGE_SIZE - 1ULL)) &&
            physical < page_end;
        pmm_free_page(allocated);
        KASSERT(!overlaps_dtb);
    }
}
#endif

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

KTEST_CASE(test_vmm_kernel_text_mapping) {
    extern uint64_t *kernel_pgd;
    extern char __text_start[];
    uint64_t virtual_address = (uint64_t)__text_start;
    uint64_t expected = virt_to_phys(__text_start);

    KASSERT(kernel_pgd != NULL);
    KASSERT_EQ(vmm_get_phys(kernel_pgd, virtual_address), expected);
}

KTEST_CASE(test_vmm_kernel_bss_mapping) {
    extern uint64_t *kernel_pgd;
    extern char __bss_start[];
    uint64_t virtual_address = (uint64_t)__bss_start;
    uint64_t expected = virt_to_phys(__bss_start);

    KASSERT(kernel_pgd != NULL);
    KASSERT_EQ(vmm_get_phys(kernel_pgd, virtual_address), expected);
}

static int ktest_boot_live_span(uint32_t kind,
                                struct nxhal_boot_live_span *out) {
    uint32_t count = nxhal_boot_live_span_count();

    if (count > (uint32_t)NXHAL_BOOT_LIVE_ROLE_COUNT)
        return 0;
    for (uint32_t index = 0U; index < count; ++index) {
        if (nxhal_boot_live_span_get(index, out) != NXHAL_BOOT_MEMORY_OK)
            return 0;
        if (out->kind == kind)
            return 1;
    }
    return 0;
}

KTEST_CASE(test_vmm_boot_fdt_mapping) {
    extern uint64_t *kernel_pgd;
    struct nxhal_boot_live_span span;

    if (!HAL_HAS_FDT)
        return;
    KASSERT(ktest_boot_live_span(NXHAL_BOOT_LIVE_FDT, &span));
    KASSERT(span.length != 0U);
    KASSERT_EQ(vmm_get_phys(kernel_pgd, (uint64_t)phys_to_virt(span.base)),
               span.base);
    KASSERT_EQ(vmm_get_phys(kernel_pgd,
                           (uint64_t)phys_to_virt(span.base + span.length - 1U)),
               span.base + span.length - 1U);
}

KTEST_CASE(test_vmm_boot_firmware_map_mapping) {
    extern uint64_t *kernel_pgd;
    struct nxhal_boot_live_span span;

    if (nxhal_boot_memory_count() == 0U)
        return;
    KASSERT(ktest_boot_live_span(NXHAL_BOOT_LIVE_FIRMWARE_MAP, &span));
    KASSERT(span.length != 0U);
    KASSERT_EQ(vmm_get_phys(kernel_pgd, (uint64_t)phys_to_virt(span.base)),
               span.base);
    KASSERT_EQ(vmm_get_phys(kernel_pgd,
                           (uint64_t)phys_to_virt(span.base + span.length - 1U)),
               span.base + span.length - 1U);
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
#if HAL_TEST_VMM_AP_RO
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
#if HAL_TEST_VMM_RW_BIT
    KASSERT_EQ(vmm_check_range(kernel_pgd, (uint64_t)page, 4096, PTE_RW), 0);
#endif
    page[1] = 0xCD;
    KASSERT_EQ(page[0], 0xAB);
    KASSERT_EQ(page[1], 0xCD);
    pmm_free_page(page);
}
