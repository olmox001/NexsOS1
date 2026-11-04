#ifndef _KERNEL_BOOT_INFO_H
#define _KERNEL_BOOT_INFO_H

/*
 * AArch64 boot handoff record.
 *
 * The first two fields are written by the position-independent entry code
 * before BSS is cleared. At entry, fdt_size contains the available span passed
 * in x1 (zero for legacy Image loaders); after validation C normalizes it to
 * the actual FDT totalsize. The record is the stable kernel-side boot contract.
 */
#define BOOT_INFO_MAGIC              0x4E584249 /* "NXBI" */
#define BOOT_INFO_VERSION            1
#define BOOT_INFO_SIZE               32

#define BOOT_INFO_FDT_PHYS_OFFSET    0
#define BOOT_INFO_FDT_SIZE_OFFSET    8
#define BOOT_INFO_MAGIC_OFFSET       16
#define BOOT_INFO_VERSION_OFFSET     20
#define BOOT_INFO_SIZE_FIELD_OFFSET  22
#define BOOT_INFO_SOURCE_OFFSET      24
#define BOOT_INFO_FLAGS_OFFSET       28

#define BOOT_INFO_SOURCE_UNKNOWN     0
#define BOOT_INFO_SOURCE_AARCH64_EFI 1
#define BOOT_INFO_SOURCE_LEGACY      2

#define BOOT_INFO_FLAG_FDT_SIZE_VALID       (1U << 0)
#define BOOT_INFO_FLAG_LEGACY_TRUSTED_INPUT (1U << 1)
#define BOOT_INFO_FLAG_LEGACY_SEARCH         (1U << 2)

#ifndef __ASSEMBLER__
#include <kernel/types.h>

struct boot_info {
    uint64_t fdt_phys;
    uint64_t fdt_size;
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t source;
    uint32_t flags;
};

_Static_assert(sizeof(struct boot_info) == BOOT_INFO_SIZE,
               "boot_info ABI size mismatch");
_Static_assert(__builtin_offsetof(struct boot_info, fdt_phys) ==
                   BOOT_INFO_FDT_PHYS_OFFSET,
               "boot_info.fdt_phys offset mismatch");
_Static_assert(__builtin_offsetof(struct boot_info, fdt_size) ==
                   BOOT_INFO_FDT_SIZE_OFFSET,
               "boot_info.fdt_size offset mismatch");
_Static_assert(__builtin_offsetof(struct boot_info, magic) ==
                   BOOT_INFO_MAGIC_OFFSET,
               "boot_info.magic offset mismatch");
_Static_assert(__builtin_offsetof(struct boot_info, version) ==
                   BOOT_INFO_VERSION_OFFSET,
               "boot_info.version offset mismatch");
_Static_assert(__builtin_offsetof(struct boot_info, size) ==
                   BOOT_INFO_SIZE_FIELD_OFFSET,
               "boot_info.size offset mismatch");
_Static_assert(__builtin_offsetof(struct boot_info, source) ==
                   BOOT_INFO_SOURCE_OFFSET,
               "boot_info.source offset mismatch");
_Static_assert(__builtin_offsetof(struct boot_info, flags) ==
                   BOOT_INFO_FLAGS_OFFSET,
               "boot_info.flags offset mismatch");

extern struct boot_info boot_info;
#endif

#endif /* _KERNEL_BOOT_INFO_H */
