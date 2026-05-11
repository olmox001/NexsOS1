/*
 * kernel/arch/amd64/cpu/msr.c
 * Configurazione MSR per l'istruzione syscall (fast path) x86-64
 */
#include <kernel/types.h>
#include <arch/arch.h>
#include <arch/amd64_internal.h>
#include <kernel/printk.h>

#define IA32_EFER       0xC0000080
#define IA32_STAR       0xC0000081
#define IA32_LSTAR      0xC0000082
#define IA32_FMASK      0xC0000084

#define EFER_SCE        0x01       /* System Call Enable */

/* GDT Selectors (Must match gdt.c) */
#define GDT_KERN_CODE   0x08
#define GDT_KERN_DATA   0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20

extern void syscall_entry(void);

void amd64_syscall_init(void) {
  uint64_t star;

  /* IA32_EFER.SCE must be 1 to enable syscall/sysret */
  uint64_t efer = rdmsr(IA32_EFER);
  wrmsr(IA32_EFER, efer | EFER_SCE);

  /* IA32_STAR (Bits 63:48 user CS/SS, Bits 47:32 kernel CS/SS)
   * Sysret computes User CS from Bits 63:48 + 16, and User SS from + 8.
   * Thus we put GDT_USER_DATA (0x18) there, so it yields User CS = 0x28, SS = 0x20??
   * Wait, Linux convention is:
   * SYSRET loads CS from STAR[63:48] + 16.
   * We want CS = 0x20 (GDT_USER_CODE). So STAR[63:48] must be 0x10 (GDT_KERN_DATA)? No.
   * If STAR[63:48] = 0x10.
   * CS = 0x10 + 16 = 0x20. (matches GDT_USER_CODE)
   * SS = 0x10 + 8 = 0x18.  (matches GDT_USER_DATA)
   *
   * SYSCALL loads CS from STAR[47:32].
   * We want CS = 0x08 (GDT_KERN_CODE).
   * SS = 0x08 + 8 = 0x10 (GDT_KERN_DATA).
   *
   * So STAR = (0x10ULL << 48) | (0x08ULL << 32)
   */
  star = ((uint64_t)GDT_KERN_DATA << 48) | ((uint64_t)GDT_KERN_CODE << 32);
  wrmsr(IA32_STAR, star);

  /* IA32_LSTAR contains the RIP to jump to on syscall */
  wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);

  /* IA32_FMASK: RFLAGS to clear on syscall.
   * Mask interrupts (IF=0x200), Direction flag (DF=0x400)
   */
  wrmsr(IA32_FMASK, 0x200 | 0x400);

  pr_info("AMD64 MSR SYSCALL configured\n");
}
