#ifndef NXHAL_PROVIDER_CONFIG_H
#define NXHAL_PROVIDER_CONFIG_H

/* Provider-selected constants consumed through the HAL vocabulary. */
#define HAL_RAM_START                    0x0UL
#define HAL_ALIAS_OFFSET                 0x0UL
#define HAL_PHYS_MAP_LIMIT               (256ULL << 30)
#define HAL_BOOTSTRAP_PHYS_MAP_LIMIT     (1ULL << 30)
#define HAL_DIRECT_MAP_BASE              0xFFFF800000000000UL
#define HAL_ELF_MACHINE                  62U
#define HAL_HAS_IOPORT                   1

#define HAL_BOOT_ABI_MULTIBOOT2         1
#define HAL_BOOT_ABI_FDT                0
#define HAL_HAS_FDT                     0
#define HAL_HAS_PHYS_PROBE              0
#define HAL_TEST_PCI_3G_HOLE            1
#define HAL_TEST_VMM_AP_RO              0
#define HAL_TEST_VMM_RW_BIT             1
#define HAL_PLATFORM_NAME               "amd64"

#endif /* NXHAL_PROVIDER_CONFIG_H */
