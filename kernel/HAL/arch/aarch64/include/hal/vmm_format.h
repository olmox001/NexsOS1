#ifndef NXHAL_VMM_FORMAT_H
#define NXHAL_VMM_FORMAT_H

/* --- AArch64 Page Table Entry (PTE) Flags --- */
/* AArch64 descriptor format and bit details:
 * AArch64 uses descriptor format: bits [1:0] = 0b11 for page, 0b01 for block.
 * Attribute index (bits [4:2]) selects the MAIR_EL1 slot (Normal vs Device).
 * Shareability: bits [9:8]; Inner Shareable = 0b11.
 * Access flag (AF, bit 10): must be set or a permission fault occurs on first access.
 * AP bits [7:6]: EL0 read/write access control.
 * UXN (bit 54): unprivileged execute-never; PXN (bit 53): privileged execute-never. */
#define PTE_VALID (1UL << 0)
#define PTE_PAGE  (1UL << 1)
#define PTE_BLOCK (0UL << 1)

#define PTE_AP_EL1_RW (0UL << 6)
#define PTE_AP_EL1_RO (2UL << 6)
#define PTE_AP_EL0_RW (1UL << 6)
#define PTE_AP_EL0_RO (3UL << 6)

#define PTE_RW       PTE_AP_EL1_RW
#define PTE_USER     PTE_AP_EL0_RW
#define PTE_RO       PTE_AP_EL1_RO

#define PTE_TABLE    PTE_PAGE

/* PTE_IS_TABLE: does this VALID directory entry point to a next-level table?
 * AArch64: bits[1:0] == 0b11 → table; 0b01 → block. */
#define PTE_IS_TABLE(e) (((e) & 0x3UL) == 0x3UL)

#define PTE_INNER_SHARE (3UL << 8)
#define PTE_AF          (1UL << 10)

/* PAGE_KERNEL: kernel RW, not executable (PXN+UXN set).  This is the W^X
 * default for all RAM (MM-VMM-01 resolved); only the kernel text window is
 * mapped PAGE_KERNEL_RX by vmm_map_ram_wx(). */
#define PAGE_KERNEL \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RW | PTE_UXN | PTE_PXN)
/* PAGE_KERNEL_EXEC: kernel RW + executable.  W^X-violating by definition;
 * kept only for exceptional uses — no RAM-wide mapping uses it anymore. */
#define PAGE_KERNEL_EXEC \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RW | PTE_UXN)
/* PAGE_KERNEL_RX: kernel text — read-only at EL1, executable at EL1 only. */
#define PAGE_KERNEL_RX \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RO | PTE_UXN)
/* PAGE_KERNEL_RO: kernel rodata — read-only, never executable. */
#define PAGE_KERNEL_RO \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RO | PTE_UXN | PTE_PXN)
/* PAGE_DEVICE: strongly-ordered Device-nGnRnE memory, not executable. */
#define PAGE_DEVICE \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_DEVICE) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL1_RW | PTE_UXN | PTE_PXN)
/* PAGE_USER: EL0 (user) RW, not executable at EL1.  EL0-executable: use for
 * ELF code segments only. */
#define PAGE_USER \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL0_RW | PTE_PXN)
/* PAGE_USER_DATA: user RW data (stack/heap) — never executable (W^X). */
#define PAGE_USER_DATA (PAGE_USER | PTE_UXN)
/* PAGE_USER_RX: user read-only + EL0-executable (ELF text). PXN keeps it
 * non-executable at EL1 — the kernel must never execute a user page. */
#define PAGE_USER_RX \
    (PTE_VALID | PTE_PAGE | PTE_ATTR_INDX(PTE_ATTR_NORMAL) | PTE_INNER_SHARE | \
     PTE_AF | PTE_AP_EL0_RO | PTE_PXN)
/* PAGE_USER_RO: user read-only data (ELF rodata) — never executable (W^X). */
#define PAGE_USER_RO (PAGE_USER_RX | PTE_UXN)

#endif /* NXHAL_VMM_FORMAT_H */
