#ifndef NXHAL_VMM_FORMAT_H
#define NXHAL_VMM_FORMAT_H

/* --- AMD64 Page Table Entry (PTE) Flags --- */
/* AMD64 bit layout details:
 * AMD64 uses a simpler flat bit layout; no MAIR equivalent.
 * PTE_NX (bit 63) requires IA32_EFER.NXE=1 (set by arch init).
 * NOTE(MM-VMM-01): PAGE_KERNEL_EXEC == PAGE_KERNEL on AMD64 (no NX bit set),
 * so no W^X enforcement exists on this arch either. */
#define PTE_VALID (1UL << 0) 
#define PTE_RW    (1UL << 1) 
#define PTE_USER  (1UL << 2) 
#define PTE_PWT   (1UL << 3) 
#define PTE_PCD   (1UL << 4) 
#define PTE_AF    (1UL << 5) 
#define PTE_DIRTY (1UL << 6) 
#define PTE_PS    (1UL << 7) 
#define PTE_NX    (1ULL << 63)

#define PTE_PAGE  (0UL)
#define PTE_RO    (0UL)

#define PTE_TABLE (PTE_RW | PTE_USER)

/* PTE_IS_TABLE: does this VALID directory entry point to a next-level table?
 * AMD64: PS (bit 7) clear → table; PS set → 2MB/1GB page.  (PTE_TABLE above
 * is NOT usable for this test — it is an RW|US flag combo for new tables.) */
#define PTE_IS_TABLE(e) (!((e) & PTE_PS))

/* W^X page profiles (MM-VMM-01/AMMU-01 resolved).  PTE_NX requires
 * IA32_EFER.NXE=1, set next to LME in start.S (BSP) and trampoline.S (APs);
 * CR0.WP is set at boot so the kernel honours read-only PTEs. */
#define PAGE_KERNEL      (PTE_VALID | PTE_RW | PTE_NX)
#define PAGE_KERNEL_EXEC (PTE_VALID | PTE_RW) /* W+X: no RAM-wide user left */
#define PAGE_KERNEL_RX   (PTE_VALID)               /* text: RO + executable */
#define PAGE_KERNEL_RO   (PTE_VALID | PTE_NX)      /* rodata: RO, no exec */
#define PAGE_DEVICE      (PTE_VALID | PTE_RW | PTE_PCD | PTE_PWT | PTE_NX)
#define PAGE_USER        (PTE_VALID | PTE_RW | PTE_USER)
/* PAGE_USER_DATA: user RW data (stack/heap) — never executable (W^X). */
#define PAGE_USER_DATA   (PAGE_USER | PTE_NX)
/* PAGE_USER_RX: user read-only + executable (ELF text). */
#define PAGE_USER_RX     (PTE_VALID | PTE_USER)
/* PAGE_USER_RO: user read-only data (ELF rodata) — never executable (W^X). */
#define PAGE_USER_RO     (PTE_VALID | PTE_USER | PTE_NX)

#endif /* NXHAL_VMM_FORMAT_H */
