/*
 * kernel/lib/fdt.c
 * Flattened Device Tree (FDT / DTB) Parser
 *
 * Purpose:
 *   Parses the Flattened Device Tree blob (DTB) provided by the firmware
 *   (QEMU) at boot time to discover the system memory map and SMP CPU count.
 *   All DTB fields are big-endian; this file performs the necessary byte-swap.
 *
 * Role:
 *   On AArch64 this is on the critical boot path:
 *     kernel_main (x0 = DTB address) → fdt_init() → fdt_get_mem_regions()
 *                                                  → fdt_count_cpus()
 *   Results feed directly into pmm_init() (memory regions) and SMP bringup
 *   (CPU count).  On AMD64 all entry points return -1 / 0 unconditionally;
 *   memory is discovered via multiboot2 instead.
 *
 * Implementation scope:
 *   Minimal — implements only the two properties the kernel needs:
 *   1. Memory regions: walks FDT_BEGIN_NODE / FDT_PROP to find `memory@*`
 *      nodes and read their `reg` properties.
 *   2. CPU count: counts `cpu@*` child nodes under the `cpus` node.
 *   The memory reservation map, IRQ topology, `compatible` strings, and all
 *   other DT properties are not parsed.
 *
 * Byte-order:
 *   The DTB is big-endian on all architectures.  The kernel runs little-endian
 *   on both amd64 and aarch64.  fdt32_to_cpu() swaps 32-bit DTB values.
 *   fdt_read32() / fdt_read64() use volatile loads to prevent the compiler
 *   from emitting an unaligned LDR and then swap.
 *
 * Known issues:
 *   LIB-FDT-01  (W2 REFINE)   No bounds check of off_dt_struct or
 *               size_dt_struct against totalsize before parsing.  A malformed
 *               DTB can cause out-of-bounds pointer arithmetic.
 *   LIB-FDT-02  (W1 MISSING)  Memory reservation map (off_mem_rsvmap) is
 *               never parsed; firmware-reserved RAM may be overwritten by PMM.
 *   LIB-FDT-03  (W1 STUB)     AMD64 fdt_init() and fdt_find_in_memory() are
 *               explicit stubs; expected behaviour, guarded by #ifdef.
 *   LIB-FDT-04  (W2 SECURITY) fdt_find_in_memory() validates only the magic
 *               word; does not validate totalsize or structure offsets of the
 *               candidate DTB.  A crafted RAM layout could produce a false
 *               positive pointing into arbitrary memory.
 *   LIB-FDT-05  (W0 DOC)      Stale comment near uart_puts call; printk is
 *               fully available at the point fdt_init is called.
 */
#include <kernel/fdt.h>
#include <kernel/memlayout.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/pmm.h>
#include <drivers/uart.h>

/* fdt_ptr: pointer to the validated DTB header in memory.
 * NULL until fdt_init() succeeds.  Set once; never changed after init. */
static struct fdt_header *fdt_ptr = NULL;
/* boot_fdt_ptr: physical address of the DTB, exported for use by arch code.
 * Set by fdt_init(); 0 if no DTB was found or on AMD64. */
uint64_t boot_fdt_ptr = 0;

/*
 * fdt32_to_cpu - byte-swap a big-endian 32-bit DTB value to host byte order.
 *
 * The DTB specification (ePAPR) requires all header and structure fields to be
 * stored in big-endian byte order.  This kernel runs little-endian on both
 * aarch64 and amd64, so every 32-bit DTB word must be swapped before use.
 *
 * Params: val - raw 32-bit word read from the DTB.
 * Returns: val in host byte order.
 */
static uint32_t fdt32_to_cpu(uint32_t val) {
    return ((val & 0xFF000000) >> 24) |
           ((val & 0x00FF0000) >> 8) |
           ((val & 0x0000FF00) << 8) |
           ((val & 0x000000FF) << 24);
}



/*
 * fdt_init - validate and register the Device Tree Blob.
 *
 * AArch64 variant (compiled when ARCH_AMD64 is not defined):
 *   If fdt_addr is 0, attempts to locate the DTB by scanning RAM in the range
 *   [0x40000000, 0x80000000) via fdt_find_in_memory().
 *   Validates the FDT_MAGIC (0xd00dfeed) word.  On success, stores fdt_ptr and
 *   boot_fdt_ptr for use by fdt_get_mem_regions() and fdt_count_cpus().
 *
 * NOTE(LIB-FDT-05): The uart_puts("... just manual print ...") comment at the
 *   address-print site is stale — printk/snprintf are fully available at the
 *   point this function runs.  The address is never printed.
 * NOTE(LIB-FDT-01): totalsize and structure offsets are not validated before
 *   subsequent parsing calls.  A malformed DTB could produce out-of-bounds
 *   pointer arithmetic in fdt_get_mem_regions().
 *
 * AMD64 stub (compiled when ARCH_AMD64 is defined):
 *   Returns -1 unconditionally.  Memory discovery uses multiboot2.
 *   NOTE(LIB-FDT-03): This is an explicit, expected stub.
 *
 * Params: fdt_addr - physical address of the DTB, or 0 to trigger RAM scan.
 * Returns: 0 on success; -1 if no valid DTB is found.
 * Locking: none; called single-threaded during early boot.
 * Side effects: sets module-static fdt_ptr and boot_fdt_ptr.
 */
#ifndef ARCH_AMD64
int fdt_init(uintptr_t fdt_addr) {
    if (fdt_addr == 0) {
        uart_puts("FDT: No address provided, scanning RAM...\n");
        /* Scan up to 1GB of RAM */
        fdt_addr = fdt_find_in_memory(0x40000000, 0x80000000);
        if (fdt_addr == 0) {
            uart_puts("FDT: Scan failed, no DTB found.\n");
            return -1;
        }
        /* Simple hex to string for uart_puts */
        uart_puts("FDT: Found magic at 0x");
        // ... just manual print for now to avoid dependency on sprintf if not ready
        /* NOTE(LIB-FDT-05): above comment is stale; printk is available here. */
        uart_puts("... (scanning succeeded)\n");
    }

    uart_puts("FDT: Probing... \n");
    /* fdt_addr is the DTB's PHYSICAL address (x0 from QEMU); parse it
     * through the direct map (identity while KERNEL_VIRT_BASE == 0).
     * boot_fdt_ptr keeps exporting the physical address. */
    struct fdt_header *hdr = (struct fdt_header *)phys_to_virt(fdt_addr);
    uint32_t magic = fdt32_to_cpu(hdr->magic);
    if (magic != FDT_MAGIC) {
        uart_puts("FDT: Invalid magic!\n");
        return -1;
    }
    fdt_ptr = hdr;
    boot_fdt_ptr = fdt_addr;
    uart_puts("FDT: Successfully initialized\n");
    return 0;
}
#else
/* NOTE(LIB-FDT-03): AMD64 stub — DTB not used on AMD64. */
int fdt_init(uintptr_t fdt_addr) {
    (void)fdt_addr;
    return -1;
}
#endif

/*
 * fdt_find_in_memory - scan a RAM range for a DTB magic signature.
 *
 * AArch64 variant:
 *   Walks aligned 8-byte boundaries from 'start' to 'end', comparing the
 *   first 4 bytes of each candidate to the little-endian representation of
 *   the big-endian magic 0xd00dfeed, which is 0xedfe0dd0 in raw memory on a
 *   little-endian machine.
 *   Emits a '.' progress dot to UART every 16 MB (addr & 0xFFFFFF == 0).
 *
 * NOTE(LIB-FDT-04): Only the magic word is checked.  If a false positive is
 *   found (a non-DTB region that happens to start with 0xedfe0dd0), subsequent
 *   calls to fdt_get_mem_regions() will dereference arbitrary memory.  The
 *   totalsize and structure offsets of the candidate are NOT validated here.
 *
 * AMD64 stub: returns 0 unconditionally.
 * NOTE(LIB-FDT-03): expected stub on AMD64.
 *
 * Params:
 *   start - physical address to begin scanning (inclusive).
 *   end   - physical address to stop scanning (exclusive).
 * Returns: physical address of the first candidate DTB, or 0 if not found.
 * Locking: none; called from fdt_init() during single-threaded early boot.
 */
#ifndef ARCH_AMD64
uintptr_t fdt_find_in_memory(uintptr_t start, uintptr_t end) {
    /* Scan at 8-byte aligned boundaries */
    for (uintptr_t addr = start; addr < end; addr += 8) {
        if ((addr & 0xFFFFFF) == 0) {
            uart_puts("."); /* Progress indicator */
        }
        /* 'addr' iterates PHYSICAL addresses; deref via the direct map. */
        struct fdt_header *hdr = (struct fdt_header *)phys_to_virt(addr);
        /* Check magic without conversion first for speed.
         * 0xedfe0dd0 is the big-endian value 0xd00dfeed (FDT_MAGIC) stored in
         * little-endian byte order as it appears in raw memory. */
        if (hdr->magic == 0xedfe0dd0) { /* BE 0xd00dfeed on LE machine */
            uart_puts("\nFDT: Found candidate at 0x");
            /* NOTE(LIB-FDT-04): totalsize/structure offsets not validated here. */
            /* Success! */
            return addr;
        }
    }
    uart_puts("\n");
    return 0;
}
#else
/* NOTE(LIB-FDT-03): AMD64 stub — no DTB on AMD64. */
uintptr_t fdt_find_in_memory(uintptr_t start, uintptr_t end) {
    (void)start; (void)end;
    return 0;
}
#endif

/*
 * fdt_get_string - look up a string in the DTB strings block by offset.
 *
 * DTB property names are stored in a separate strings block whose base is
 * given by fdt_ptr->off_dt_strings (big-endian).  FDT_PROP tags carry an
 * offset into this block to identify the property name.
 *
 * NOTE(LIB-FDT-01): no bounds check against size_dt_strings; an out-of-range
 *   offset produces a pointer outside the DTB.
 *
 * Params: offset - byte offset into the strings block.
 * Returns: pointer to the NUL-terminated property name string.
 */
static const char *fdt_get_string(uint32_t offset) {
    return (const char *)((uintptr_t)fdt_ptr + fdt32_to_cpu(fdt_ptr->off_dt_strings) + offset);
}

/*
 * fdt_read32 - read a big-endian 32-bit value from a potentially misaligned
 *              DTB location, returning it in host byte order.
 *
 * Uses a volatile pointer to suppress compiler optimisation that might emit a
 * wider or differently-aligned load instruction.
 *
 * Params: p - pointer to a 32-bit big-endian word in the DTB structure block.
 * Returns: the value in host (little-endian) byte order.
 */
static uint32_t fdt_read32(uint32_t *p) {
    /* Force 32-bit load to avoid alignment issues if compiler tries to optimize */
    return fdt32_to_cpu(*(volatile uint32_t *)p);
}

/*
 * fdt_read64 - read a big-endian 64-bit value from the DTB as two 32-bit loads.
 *
 * AArch64 and AMD64 may generate a single 8-byte LDR for a 64-bit read, which
 * would be unaligned if the DTB structure is not 8-byte aligned.  Two separate
 * 32-bit volatile loads are always aligned to 4 bytes and avoid the hazard.
 * The DTB stores 64-bit values as (high32, low32) in big-endian order.
 *
 * Params: p - pointer to the first (high) 32-bit word of the 64-bit DTB value.
 * Returns: the assembled 64-bit value in host byte order.
 */
static uint64_t fdt_read64(uint32_t *p) {
    /* Force two 32-bit loads to avoid unaligned 64-bit load (LDR Xn) */
    volatile uint32_t *vp = (volatile uint32_t *)p;
    uint64_t high = fdt32_to_cpu(vp[0]);
    uint64_t low = fdt32_to_cpu(vp[1]);
    return (high << 32) | low;
}

/*
 * fdt_get_mem_regions - extract usable RAM regions from the DTB.
 *
 * Walks the DTB structure block, identifying `memory@*` (or `memory`) nodes
 * and reading their `reg` properties to discover physical memory regions.
 * Also reads `#address-cells` and `#size-cells` from the root node (depth 1)
 * to determine whether base/size values are 32-bit or 64-bit.
 *
 * Walk algorithm:
 *   - FDT_BEGIN_NODE: enter a node; track depth.  If the node name starts with
 *     "memory@" or equals "memory", set in_memory_node = current depth.
 *   - FDT_END_NODE: if leaving the memory node, clear in_memory_node; decrement
 *     depth.
 *   - FDT_PROP at depth 1: read #address-cells and #size-cells.
 *   - FDT_PROP in memory node with name "reg": decode base/size pairs.
 *     Each pair is (addr_cells × 4) + (size_cells × 4) bytes.
 *   - FDT_NOP: skip (continue).
 *   - FDT_END: stop.
 *
 * NOTE(LIB-FDT-01): `p` and `end` are computed from off_dt_struct and
 *   size_dt_struct without validating that those offsets fit within totalsize.
 *   A malformed DTB can produce `end` outside the DTB allocation.
 * NOTE(LIB-FDT-02): The memory reservation map (off_mem_rsvmap) is not parsed.
 *   Firmware-reserved RAM ranges are not communicated to the PMM.
 *
 * Params:
 *   regions   - output array of struct mem_region to fill.
 *   max_count - capacity of the regions array.
 *   count     - output; receives the number of regions written (may be NULL).
 * Returns: 0 on success; -1 if fdt_ptr is NULL (fdt_init not called or failed).
 * Locking: none; assumes fdt_ptr is stable (set once at boot, read-only after).
 */
int fdt_get_mem_regions(struct mem_region *regions, size_t max_count, size_t *count) {
    if (!fdt_ptr) return -1;

    /* NOTE(LIB-FDT-01): off_dt_struct / size_dt_struct not validated against totalsize. */
    uint32_t *p = (uint32_t *)((uintptr_t)fdt_ptr + fdt32_to_cpu(fdt_ptr->off_dt_struct));
    uint32_t *end = (uint32_t *)((uintptr_t)p + fdt32_to_cpu(fdt_ptr->size_dt_struct));

    int depth = 0;
    int in_memory_node = 0;   /* non-zero = depth at which the memory node began */
    size_t found_count = 0;

    /* Defaults from ePAPR; overridden by root-node properties at depth == 1 */
    uint32_t addr_cells = 2;  /* #address-cells: number of 32-bit words per address */
    uint32_t size_cells = 2;  /* #size-cells:    number of 32-bit words per size */

    while (p < end && found_count < max_count) {
        uint32_t tag = fdt32_to_cpu(*p++);

        if (tag == FDT_BEGIN_NODE) {
            /* Node name follows the tag as a NUL-terminated string, padded to 4-byte
             * boundary.  Advance p past the name. */
            const char *name = (const char *)p;
            size_t name_len = strlen(name);
            p += (name_len + 1 + 3) / 4;

            depth++;
            /* Detect memory nodes by name prefix or exact match */
            if (strncmp(name, "memory@", 7) == 0 || strcmp(name, "memory") == 0) {
                in_memory_node = depth;
            }
        } else if (tag == FDT_END_NODE) {
            if (in_memory_node == depth) in_memory_node = 0; /* leaving memory node */
            depth--;
        } else if (tag == FDT_PROP) {
            /* FDT_PROP layout: [len:u32][name_off:u32][data:len bytes, padded to 4] */
            uint32_t len = fdt32_to_cpu(*p++);
            uint32_t name_off = fdt32_to_cpu(*p++);
            const char *name = fdt_get_string(name_off);

            /* Root node properties (depth == 1) set the address/size cell widths */
            if (depth == 1) {
                if (strcmp(name, "#address-cells") == 0) addr_cells = fdt32_to_cpu(*p);
                if (strcmp(name, "#size-cells") == 0) size_cells = fdt32_to_cpu(*p);
            }

            if (in_memory_node && strcmp(name, "reg") == 0) {
                /* Parse RAM regions from the `reg` property.
                 * Each entry is (addr_cells + size_cells) × 4 bytes.
                 * Count of entries: len / ((addr_cells + size_cells) * 4). */
                uint32_t *reg_p = p;
                for (uint32_t i = 0; i < len / ((addr_cells + size_cells) * 4); i++) {
                    uint64_t base, size;
                    /* Read base address: 32-bit or 64-bit per addr_cells */
                    if (addr_cells == 2) {
                        base = fdt_read64(reg_p);
                        reg_p += 2;
                    } else {
                        base = fdt_read32(reg_p++);
                    }

                    /* Read region size: 32-bit or 64-bit per size_cells */
                    if (size_cells == 2) {
                        size = fdt_read64(reg_p);
                        reg_p += 2;
                    } else {
                        size = fdt_read32(reg_p++);
                    }

                    if (found_count < max_count) {
                        regions[found_count].base = base;
                        regions[found_count].size = size;
                        /* NOTE(LIB-FDT-02): all regions marked usable; firmware-
                         * reserved ranges from off_mem_rsvmap are not considered. */
                        regions[found_count].type = MEM_REGION_USABLE;
                        found_count++;
                    }
                }
            }
            /* Advance past property data (rounded up to 4-byte boundary) */
            p += (len + 3) / 4;
        } else if (tag == FDT_NOP) {
            continue;
        } else if (tag == FDT_END) {
            break;
        }
    }

    if (count) *count = found_count;
    return 0;
}

/*
 * fdt_count_cpus - count CPU entries in the DTB `cpus` node.
 *
 * Walks the DTB structure block looking for the `cpus` node, then counts
 * direct child nodes whose name starts with "cpu@" or equals "cpu".  The
 * result is used during SMP bringup to determine how many secondary CPUs
 * to start.
 *
 * Walk algorithm mirrors fdt_get_mem_regions():
 *   - Track depth; note depth at which `cpus` node begins (in_cpus_node).
 *   - Any direct child (depth == in_cpus_node + 1, i.e., depth - 1 == in_cpus_node)
 *     whose name matches "cpu@*" or "cpu" is counted as a CPU.
 *   - FDT_PROP entries are skipped entirely (only node structure matters here).
 *
 * NOTE(LIB-FDT-01): same as fdt_get_mem_regions — off_dt_struct/size_dt_struct
 *   are used without validating against totalsize.
 *
 * Params: none.
 * Returns: number of `cpu@*` / `cpu` nodes found; 0 if fdt_ptr is NULL.
 * Locking: none; fdt_ptr is read-only after fdt_init().
 */
uint32_t fdt_count_cpus(void) {
    if (!fdt_ptr) return 0;

    /* NOTE(LIB-FDT-01): structure block bounds not validated against totalsize. */
    uint32_t *p = (uint32_t *)((uintptr_t)fdt_ptr + fdt32_to_cpu(fdt_ptr->off_dt_struct));
    uint32_t *end = (uint32_t *)((uintptr_t)p + fdt32_to_cpu(fdt_ptr->size_dt_struct));

    int depth = 0;
    int in_cpus_node = 0;   /* depth at which the `cpus` node began; 0 = not inside */
    uint32_t cpu_count = 0;

    while (p < end) {
        uint32_t tag = fdt32_to_cpu(*p++);

        if (tag == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t name_len = strlen(name);
            p += (name_len + 1 + 3) / 4;

            depth++;
            if (strcmp(name, "cpus") == 0) {
                in_cpus_node = depth;  /* entered the `cpus` container node */
            } else if (in_cpus_node == depth - 1 && (strncmp(name, "cpu@", 4) == 0 || strcmp(name, "cpu") == 0)) {
                /* Direct child of `cpus` with a matching name = one CPU */
                cpu_count++;
            }
        } else if (tag == FDT_END_NODE) {
            if (in_cpus_node == depth) in_cpus_node = 0; /* left the cpus node */
            depth--;
        } else if (tag == FDT_PROP) {
            uint32_t len = fdt32_to_cpu(*p++);
            p++; /* skip name_off — property names not needed for CPU counting */
            p += (len + 3) / 4;
        } else if (tag == FDT_NOP) {
            continue;
        } else if (tag == FDT_END) {
            break;
        }
    }

    return cpu_count;
}
