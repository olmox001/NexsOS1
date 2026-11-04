/*
 * Bounded physical FDT access and platform-facing memory-region adapter.
 *
 * The byte parser lives in fdt_parser.c so it can be host-tested with
 * sanitizers. This file owns physical-address validation, the explicit
 * pointer-only legacy adapter, and conversion to the PMM region contract.
 */
#include <kernel/arch.h>
#include <kernel/fdt.h>
#include <kernel/fdt_parser.h>
#include <kernel/memlayout.h>
#include <kernel/platform.h>
#include <kernel/printk.h>

#define FDT_LEGACY_MAX_SIZE FDT_PARSER_MAX_TOTAL_SIZE
#define FDT_LEGACY_SCAN_SIZE (1ULL << 30) /* QEMU virt's first RAM GiB */

uint64_t boot_fdt_ptr = 0;
uint64_t boot_fdt_size = 0;

#ifndef ARCH_AMD64

static struct fdt_blob_view active_fdt;
static int active_fdt_valid;

static uint32_t fdt_read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static int fdt_probe_physical_range(uint64_t base, uint64_t length) {
    if (length == 0 || base < ARCH_RAM_START ||
        base >= ARCH_PHYS_MAP_LIMIT ||
        length > ARCH_PHYS_MAP_LIMIT - base)
        return -1;

    uint64_t end = base + length;
    uint64_t page = base & ~((uint64_t)PAGE_SIZE - 1ULL);
    while (page < end) {
        uint32_t probe_value = 0;
        if (arch_platform_probe_read32(page, &probe_value) != 0)
            return -1;
        if (page > UINT64_MAX - PAGE_SIZE)
            return -1;
        page += PAGE_SIZE;
    }
    return 0;
}

int fdt_init(uintptr_t base, size_t available_length) {
    active_fdt_valid = 0;
    boot_fdt_ptr = 0;
    boot_fdt_size = 0;

    uint64_t physical = (uint64_t)base;
    if (physical == 0 || (physical & 7ULL) != 0 ||
        available_length == 0 ||
        physical >= ARCH_PHYS_MAP_LIMIT ||
        (uint64_t)available_length > ARCH_PHYS_MAP_LIMIT - physical ||
        available_length > FDT_LEGACY_MAX_SIZE) {
        pr_err("FDT: invalid physical span base=0x%lx length=0x%lx\n",
               physical, (uint64_t)available_length);
        return FDT_PARSER_INVALID;
    }

    if (fdt_probe_physical_range(physical, (uint64_t)available_length) != 0) {
        pr_err("FDT: physical span is not fully readable: 0x%lx + 0x%lx\n",
               physical, (uint64_t)available_length);
        return FDT_PARSER_INVALID;
    }

    const void *blob = phys_to_virt(physical);
    struct fdt_blob_view parsed;
    int status = fdt_parser_validate(blob, available_length, &parsed);
    if (status != FDT_PARSER_OK) {
        pr_err("FDT: bounded validation failed status=%d base=0x%lx span=0x%lx\n",
               status, physical, (uint64_t)available_length);
        return status;
    }

    active_fdt = parsed;
    active_fdt_valid = 1;
    boot_fdt_ptr = physical;
    boot_fdt_size = parsed.total_size;

    pr_info("FDT: validated base=0x%lx size=0x%lx version=%u\n",
            boot_fdt_ptr, boot_fdt_size, active_fdt.version);
    return FDT_PARSER_OK;
}

uintptr_t fdt_find_in_memory(uintptr_t start, uintptr_t end,
                             size_t *found_size) {
    if (found_size)
        *found_size = 0;

    uint64_t scan_start = (uint64_t)start;
    uint64_t scan_end = (uint64_t)end;
    if (scan_start < ARCH_RAM_START || scan_start >= scan_end ||
        scan_end - scan_start < 8ULL || scan_end > ARCH_PHYS_MAP_LIMIT)
        return 0;

    pr_warn("FDT: legacy page search [0x%lx, 0x%lx)\n",
            scan_start, scan_end);

    uint64_t page = scan_start & ~((uint64_t)PAGE_SIZE - 1ULL);
    while (page < scan_end) {
        uint32_t page_probe = 0;
        if (arch_platform_probe_read32(page, &page_probe) == 0) {
            uint64_t page_end = page + PAGE_SIZE;
            if (page_end > scan_end)
                page_end = scan_end;

            uint64_t candidate = scan_start > page ? scan_start : page;
            if ((candidate & 7ULL) != 0)
                candidate = (candidate + 7ULL) & ~7ULL;

            while (candidate < page_end &&
                   candidate <= page_end - 8ULL &&
                   candidate <= scan_end - 8ULL) {
                const uint8_t *candidate_blob = phys_to_virt(candidate);
                uint32_t magic = fdt_read_be32(candidate_blob);
                uint32_t total_size = fdt_read_be32(candidate_blob + 4U);

                if (magic == FDT_PARSER_MAGIC &&
                    total_size >= 40U &&
                    total_size <= FDT_LEGACY_MAX_SIZE &&
                    (uint64_t)total_size <= scan_end - candidate &&
                    fdt_probe_physical_range(candidate,
                                              (uint64_t)total_size) == 0) {
                    const void *blob = phys_to_virt(candidate);
                    struct fdt_blob_view parsed;
                    if (fdt_parser_validate(blob, total_size,
                                            &parsed) == FDT_PARSER_OK) {
                        if (found_size)
                            *found_size = parsed.total_size;
                        pr_info("FDT: legacy search found valid blob at 0x%lx size=0x%lx\n",
                                candidate, (uint64_t)parsed.total_size);
                        return (uintptr_t)candidate;
                    }
                }

                if (candidate > UINT64_MAX - 8ULL)
                    return 0;
                candidate += 8ULL;
            }
        }

        if (page > UINT64_MAX - PAGE_SIZE)
            break;
        page += PAGE_SIZE;

        if (((page - (scan_start & ~((uint64_t)PAGE_SIZE - 1ULL))) &
             ((16ULL * 1024ULL * 1024ULL) - 1ULL)) == 0) {
            pr_info("FDT: legacy page search reached 0x%lx\n", page);
        }
    }

    pr_warn("%s", "FDT: no valid DTB found in legacy search window\n");
    return 0;
}

int fdt_init_legacy(uintptr_t base, size_t *actual_length) {
    if (actual_length)
        *actual_length = 0;

    if (base == 0) {
        uint64_t scan_start = (uint64_t)ARCH_RAM_START;
        uint64_t scan_end = scan_start + FDT_LEGACY_SCAN_SIZE;
        if (scan_end < scan_start || scan_end > ARCH_PHYS_MAP_LIMIT)
            return FDT_PARSER_INVALID;

        size_t found_size = 0;
        uintptr_t found = fdt_find_in_memory((uintptr_t)scan_start,
                                             (uintptr_t)scan_end,
                                             &found_size);
        if (found == 0 || found_size == 0)
            return FDT_PARSER_INVALID;

        int status = fdt_init(found, found_size);
        if (status == FDT_PARSER_OK && actual_length)
            *actual_length = found_size;
        return status;
    }

    uint64_t physical = (uint64_t)base;
    if ((physical & 7ULL) != 0 ||
        physical >= ARCH_PHYS_MAP_LIMIT ||
        fdt_probe_physical_range(physical, 8U) != 0)
        return FDT_PARSER_INVALID;

    const uint8_t *header = phys_to_virt(physical);
    if (fdt_read_be32(header) != FDT_PARSER_MAGIC)
        return FDT_PARSER_INVALID;

    uint32_t total_size = fdt_read_be32(header + 4U);
    if (total_size < 36U || total_size > FDT_LEGACY_MAX_SIZE ||
        (uint64_t)total_size > ARCH_PHYS_MAP_LIMIT - physical)
        return FDT_PARSER_INVALID;

    pr_warn("FDT: legacy pointer-only adapter at 0x%lx; "
            "trusting loader allocation up to header totalsize 0x%x\n",
            physical, total_size);

    int status = fdt_init(base, total_size);
    if (status == FDT_PARSER_OK && actual_length)
        *actual_length = total_size;
    return status;
}

int fdt_get_mem_regions(struct mem_region *regions, size_t max_count,
                        size_t *count) {
    if (count)
        *count = 0;
    if (!active_fdt_valid || !regions || !count || max_count == 0)
        return FDT_PARSER_INVALID;

    struct fdt_parser_region parsed[768];
    size_t parsed_count = 0;
    int status = fdt_parser_collect_regions(&active_fdt, boot_fdt_ptr,
                                            parsed, 768U, &parsed_count);
    if (status != FDT_PARSER_OK && status != FDT_PARSER_NO_MEMORY)
        return status;
    if (parsed_count > max_count)
        return FDT_PARSER_NO_SPACE;

    /* A successful /memory parse must contain the DTB allocation itself.
     * Otherwise the bootloader handed us a blob outside described RAM and
     * reserving it in PMM would be meaningless. NO_MEMORY is handled by the
     * explicit QEMU-virt probe, which performs the same check after discovery. */
    if (status == FDT_PARSER_OK) {
        if (boot_fdt_size == 0 ||
            boot_fdt_ptr > UINT64_MAX - boot_fdt_size)
            return FDT_PARSER_INVALID;
        uint64_t dtb_end = boot_fdt_ptr + boot_fdt_size;
        int dtb_in_ram = 0;
        for (size_t i = 0; i < parsed_count; i++) {
            if (parsed[i].type != FDT_PARSER_REGION_USABLE ||
                parsed[i].base > UINT64_MAX - parsed[i].size)
                continue;
            uint64_t region_end = parsed[i].base + parsed[i].size;
            if (boot_fdt_ptr >= parsed[i].base && dtb_end <= region_end) {
                dtb_in_ram = 1;
                break;
            }
        }
        if (!dtb_in_ram)
            return FDT_PARSER_INVALID;
    }

    for (size_t i = 0; i < parsed_count; i++) {
        regions[i].base = parsed[i].base;
        regions[i].size = parsed[i].size;
        regions[i].type =
            parsed[i].type == FDT_PARSER_REGION_USABLE
                ? MEM_REGION_USABLE : MEM_REGION_RESERVED;
    }
    *count = parsed_count;
    return status;
}

uint32_t fdt_count_cpus(void) {
    if (!active_fdt_valid)
        return 0;
    return fdt_parser_count_cpus(&active_fdt);
}

#else

int fdt_init(uintptr_t base, size_t available_length) {
    (void)base;
    (void)available_length;
    return FDT_PARSER_INVALID;
}

int fdt_init_legacy(uintptr_t base, size_t *actual_length) {
    (void)base;
    if (actual_length)
        *actual_length = 0;
    return FDT_PARSER_INVALID;
}

int fdt_get_mem_regions(struct mem_region *regions, size_t max_count,
                        size_t *count) {
    (void)regions;
    (void)max_count;
    if (count)
        *count = 0;
    return FDT_PARSER_INVALID;
}

uint32_t fdt_count_cpus(void) {
    return 0;
}

uintptr_t fdt_find_in_memory(uintptr_t start, uintptr_t end,
                             size_t *found_size) {
    (void)start;
    (void)end;
    if (found_size)
        *found_size = 0;
    return 0;
}

#endif
