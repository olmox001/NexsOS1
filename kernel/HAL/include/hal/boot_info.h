#ifndef NXHAL_BOOT_INFO_H
#define NXHAL_BOOT_INFO_H

#define NXHAL_BOOT_INFO_MAGIC              0x4E584249
#define NXHAL_BOOT_INFO_VERSION            2
#define NXHAL_BOOT_INFO_SIZE               64

#define NXHAL_BOOT_INFO_FDT_PHYS_OFFSET    0
#define NXHAL_BOOT_INFO_FDT_SIZE_OFFSET    8
#define NXHAL_BOOT_INFO_MAGIC_OFFSET       16
#define NXHAL_BOOT_INFO_VERSION_OFFSET     20
#define NXHAL_BOOT_INFO_SIZE_FIELD_OFFSET  22
#define NXHAL_BOOT_INFO_SOURCE_OFFSET      24
#define NXHAL_BOOT_INFO_FLAGS_OFFSET       28
#define NXHAL_BOOT_INFO_FW_MAP_PHYS_OFFSET 32
#define NXHAL_BOOT_INFO_FW_MAP_COUNT_OFFSET 40
#define NXHAL_BOOT_INFO_FW_MAP_RECORD_SIZE_OFFSET 44
#define NXHAL_BOOT_INFO_EBS_ATTEMPTS_OFFSET 48
#define NXHAL_BOOT_INFO_MAP_GROWTHS_OFFSET 52
#define NXHAL_BOOT_INFO_RESERVED0_OFFSET   56

#define NXHAL_BOOT_SOURCE_UNKNOWN      0
#define NXHAL_BOOT_SOURCE_AARCH64_EFI  1
#define NXHAL_BOOT_SOURCE_LEGACY       2

#define NXHAL_BOOT_FLAG_FDT_SIZE_VALID       0x00000001
#define NXHAL_BOOT_FLAG_LEGACY_TRUSTED_INPUT 0x00000002
#define NXHAL_BOOT_FLAG_LEGACY_SEARCH         0x00000004
#define NXHAL_BOOT_FLAG_UEFI_MAP_VALID         0x00000008

#define NXHAL_UEFI_MEMORY_RECORD_SIZE 40
#define NXHAL_UEFI_EXIT_MAX_ATTEMPTS 3
#define NXHAL_UEFI_MAP_MAX_GROWTHS 4

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

struct nxhal_boot_info {
  uint64_t fdt_phys;
  uint64_t fdt_size;
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t source;
  uint32_t flags;
  uint64_t firmware_map_phys;
  uint32_t firmware_map_record_count;
  uint32_t firmware_map_record_size;
  uint32_t ebs_attempts;
  uint32_t map_growths;
  uint64_t reserved0;
};

_Static_assert(sizeof(struct nxhal_boot_info) == NXHAL_BOOT_INFO_SIZE,
               "NX-HAL boot-info ABI size mismatch");
_Static_assert(offsetof(struct nxhal_boot_info, fdt_phys) ==
                   NXHAL_BOOT_INFO_FDT_PHYS_OFFSET,
               "boot-info fdt_phys offset mismatch");
_Static_assert(offsetof(struct nxhal_boot_info, firmware_map_phys) ==
                   NXHAL_BOOT_INFO_FW_MAP_PHYS_OFFSET,
               "boot-info firmware_map_phys offset mismatch");
_Static_assert(offsetof(struct nxhal_boot_info, reserved0) ==
                   NXHAL_BOOT_INFO_RESERVED0_OFFSET,
               "boot-info reserved0 offset mismatch");

extern struct nxhal_boot_info nxhal_boot_info;
#endif

#endif /* NXHAL_BOOT_INFO_H */
