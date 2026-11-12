#ifndef NXHAL_PROVIDER_CONFIG_H
#define NXHAL_PROVIDER_CONFIG_H

/* Provider-selected constants consumed through the HAL vocabulary. */
#define HAL_RAM_START                    0x40000000UL
#define HAL_ALIAS_OFFSET                 0x40000000UL
#define HAL_PHYS_MAP_LIMIT               0x2000000000ULL
#define HAL_BOOTSTRAP_PHYS_MAP_LIMIT     HAL_PHYS_MAP_LIMIT
#define HAL_PAGE_SIZE                      4096UL
#define HAL_DIRECT_MAP_BASE              0xFFFF000000000000UL
#define HAL_ELF_MACHINE                  183U
#define HAL_HAS_IOPORT                   0

#define HAL_BOOT_ABI_MULTIBOOT2         0
#define HAL_BOOT_ABI_FDT                1
#define HAL_HAS_FDT                     1
#define HAL_HAS_PHYS_PROBE              1
#define HAL_TEST_PCI_3G_HOLE            0
#define HAL_TEST_VMM_AP_RO              1
#define HAL_TEST_VMM_RW_BIT             0
#define HAL_PLATFORM_NAME               "aarch64"

#endif /* NXHAL_PROVIDER_CONFIG_H */
