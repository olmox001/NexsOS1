#ifndef _KERNEL_MULTIBOOT2_H
#define _KERNEL_MULTIBOOT2_H

#include <kernel/types.h>

#define MB1_MAGIC 0x2BADB002
#define MB2_MAGIC 0x36d76289
#define PVH_MAGIC 0x336ec578

#define MB2_TAG_TYPE_END 0
#define MB2_TAG_TYPE_MMAP 6
#define MB2_TAG_TYPE_BASIC_MEMINFO 4
#define MB2_TAG_TYPE_FRAMEBUFFER 8

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};

struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[];
};

/* Custom structure passed from our bootloader stage2 to kernel */
struct kernel_multiboot_info_struct {
    uint64_t flags;
    uint64_t mmap_ptr;
    uint64_t boot_device;
    uint64_t cmdline;
    uint64_t mods_count;
    uint64_t mods_addr;
    uint64_t elf_sections;
    uint64_t mmap_len;
    uint64_t boot_loader_name;
    uint64_t apm_table;
    uint64_t vbe_info;
};

#endif
