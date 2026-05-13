#include <kernel/fdt.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/pmm.h>
#include <drivers/uart.h>

static struct fdt_header *fdt_ptr = NULL;
uint64_t boot_fdt_ptr = 0;

static uint32_t fdt32_to_cpu(uint32_t val) {
    return ((val & 0xFF000000) >> 24) |
           ((val & 0x00FF0000) >> 8) |
           ((val & 0x0000FF00) << 8) |
           ((val & 0x000000FF) << 24);
}



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
        uart_puts("... (scanning succeeded)\n");
    }

    uart_puts("FDT: Probing... \n");
    struct fdt_header *hdr = (struct fdt_header *)fdt_addr;
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
int fdt_init(uintptr_t fdt_addr) {
    (void)fdt_addr;
    return -1;
}
#endif

#ifndef ARCH_AMD64
uintptr_t fdt_find_in_memory(uintptr_t start, uintptr_t end) {
    /* Scan at 8-byte aligned boundaries */
    for (uintptr_t addr = start; addr < end; addr += 8) {
        if ((addr & 0xFFFFFF) == 0) {
            uart_puts("."); /* Progress indicator */
        }
        struct fdt_header *hdr = (struct fdt_header *)addr;
        /* Check magic without conversion first for speed */
        if (hdr->magic == 0xedfe0dd0) { /* BE 0xd00dfeed on LE machine */
            uart_puts("\nFDT: Found candidate at 0x");
            /* Success! */
            return addr;
        }
    }
    uart_puts("\n");
    return 0;
}
#else
uintptr_t fdt_find_in_memory(uintptr_t start, uintptr_t end) {
    (void)start; (void)end;
    return 0;
}
#endif

static const char *fdt_get_string(uint32_t offset) {
    return (const char *)((uintptr_t)fdt_ptr + fdt32_to_cpu(fdt_ptr->off_dt_strings) + offset);
}

static uint32_t fdt_read32(uint32_t *p) {
    /* Force 32-bit load to avoid alignment issues if compiler tries to optimize */
    return fdt32_to_cpu(*(volatile uint32_t *)p);
}

static uint64_t fdt_read64(uint32_t *p) {
    /* Force two 32-bit loads to avoid unaligned 64-bit load (LDR Xn) */
    volatile uint32_t *vp = (volatile uint32_t *)p;
    uint64_t high = fdt32_to_cpu(vp[0]);
    uint64_t low = fdt32_to_cpu(vp[1]);
    return (high << 32) | low;
}

int fdt_get_mem_regions(struct mem_region *regions, size_t max_count, size_t *count) {
    if (!fdt_ptr) return -1;

    uint32_t *p = (uint32_t *)((uintptr_t)fdt_ptr + fdt32_to_cpu(fdt_ptr->off_dt_struct));
    uint32_t *end = (uint32_t *)((uintptr_t)p + fdt32_to_cpu(fdt_ptr->size_dt_struct));
    
    int depth = 0;
    int in_memory_node = 0;
    size_t found_count = 0;

    uint32_t addr_cells = 2;
    uint32_t size_cells = 2;

    while (p < end && found_count < max_count) {
        uint32_t tag = fdt32_to_cpu(*p++);

        if (tag == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t name_len = strlen(name);
            p += (name_len + 1 + 3) / 4;
            
            depth++;
            if (strncmp(name, "memory@", 7) == 0 || strcmp(name, "memory") == 0) {
                in_memory_node = depth;
            }
        } else if (tag == FDT_END_NODE) {
            if (in_memory_node == depth) in_memory_node = 0;
            depth--;
        } else if (tag == FDT_PROP) {
            uint32_t len = fdt32_to_cpu(*p++);
            uint32_t name_off = fdt32_to_cpu(*p++);
            const char *name = fdt_get_string(name_off);

            if (depth == 1) {
                if (strcmp(name, "#address-cells") == 0) addr_cells = fdt32_to_cpu(*p);
                if (strcmp(name, "#size-cells") == 0) size_cells = fdt32_to_cpu(*p);
            }

            if (in_memory_node && strcmp(name, "reg") == 0) {
                /* Parse RAM regions */
                uint32_t *reg_p = p;
                for (uint32_t i = 0; i < len / ((addr_cells + size_cells) * 4); i++) {
                    uint64_t base, size;
                    if (addr_cells == 2) {
                        base = fdt_read64(reg_p);
                        reg_p += 2;
                    } else {
                        base = fdt_read32(reg_p++);
                    }

                    if (size_cells == 2) {
                        size = fdt_read64(reg_p);
                        reg_p += 2;
                    } else {
                        size = fdt_read32(reg_p++);
                    }

                    if (found_count < max_count) {
                        regions[found_count].base = base;
                        regions[found_count].size = size;
                        regions[found_count].type = MEM_REGION_USABLE;
                        found_count++;
                    }
                }
            }
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

uint32_t fdt_count_cpus(void) {
    if (!fdt_ptr) return 0;

    uint32_t *p = (uint32_t *)((uintptr_t)fdt_ptr + fdt32_to_cpu(fdt_ptr->off_dt_struct));
    uint32_t *end = (uint32_t *)((uintptr_t)p + fdt32_to_cpu(fdt_ptr->size_dt_struct));
    
    int depth = 0;
    int in_cpus_node = 0;
    uint32_t cpu_count = 0;

    while (p < end) {
        uint32_t tag = fdt32_to_cpu(*p++);

        if (tag == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            size_t name_len = strlen(name);
            p += (name_len + 1 + 3) / 4;
            
            depth++;
            if (strcmp(name, "cpus") == 0) {
                in_cpus_node = depth;
            } else if (in_cpus_node == depth - 1 && (strncmp(name, "cpu@", 4) == 0 || strcmp(name, "cpu") == 0)) {
                cpu_count++;
            }
        } else if (tag == FDT_END_NODE) {
            if (in_cpus_node == depth) in_cpus_node = 0;
            depth--;
        } else if (tag == FDT_PROP) {
            uint32_t len = fdt32_to_cpu(*p++);
            p++; /* skip name_off */
            p += (len + 3) / 4;
        } else if (tag == FDT_NOP) {
            continue;
        } else if (tag == FDT_END) {
            break;
        }
    }

    return cpu_count;
}
