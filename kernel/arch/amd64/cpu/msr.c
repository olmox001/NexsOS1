/*
 * kernel/arch/amd64/cpu/msr.c
 * Model-Specific Register (MSR) Configuration for the SYSCALL Fast Path (x86-64)
 *
 * Responsibilities:
 *   Configure the four MSRs that enable and control the SYSCALL/SYSRET
 *   instruction pair on x86-64:
 *     IA32_EFER  (0xC0000080) — enable SCE (System Call Enable) bit
 *     IA32_STAR  (0xC0000081) — segment selectors for SYSCALL/SYSRET
 *     IA32_LSTAR (0xC0000082) — kernel entry RIP for SYSCALL
 *     IA32_FMASK (0xC0000084) — RFLAGS bits cleared on SYSCALL entry
 *
 * STAR encoding [verified static against gdt.c and pt_regs.h]:
 *   STAR[47:32] = GDT_KERN_CODE (0x08) → SYSCALL loads CS=0x08 (kernel code),
 *                                         SS=0x08+8=0x10 (kernel data).
 *   STAR[63:48] = GDT_KERN_DATA (0x10) → SYSRET would load CS=0x10+16=0x20
 *                                         (user code), SS=0x10+8=0x18 (user data).
 *   These selectors match the GDT layout in gdt.c:100-103 and the segment
 *   constants in pt_regs.h:99-107.  The STAR value is correct [static].
 *
 * FMASK: 0x600 clears IF (bit 9, interrupt enable) and DF (bit 10, direction
 *   flag) on SYSCALL entry, preventing user RFLAGS from enabling interrupts or
 *   setting DF in kernel context before the kernel stack is set up.
 *
 * Known issues:
 *   SYS-AMD64-01 (W2 PERF/REFINE) The fast path is correctly configured via
 *     LSTAR (syscall_entry in syscall.S), but the return path uses 'iretq'
 *     instead of 'sysretq' (syscall.S:105-107).  This is intentional ("more
 *     robust") but 20-50 cycles slower.  Prerequisites for sysretq are met
 *     (RCX holds return RIP, R11 holds return RFLAGS), so the switch is
 *     straightforward when the performance regression is deemed unacceptable.
 */
#include <kernel/types.h>
#include <arch/arch.h>
#include <arch/amd64_internal.h>
#include <kernel/printk.h>
#include "gdt_defs.h"

#define IA32_EFER       0xC0000080
#define IA32_STAR       0xC0000081
#define IA32_LSTAR      0xC0000082
#define IA32_FMASK      0xC0000084

#define EFER_SCE        0x01       /* System Call Enable */

/* GDT Selectors (Must match gdt.c) */
#ifndef GDT_KERN_CODE
#endif /* GDT_KERN_CODE */
#ifndef GDT_KERN_DATA
#endif /* GDT_KERN_DATA */
#ifndef GDT_USER_DATA
#endif /* GDT_USER_DATA */
#ifndef GDT_USER_CODE
#endif /* GDT_USER_CODE */

extern void syscall_entry(void);

/*
 * amd64_syscall_init - program the SYSCALL MSRs for this CPU.
 *
 * Must be called after gdt_init() (so the GDT selectors are valid) and after
 * the GS base is set (so syscall_entry can access cpu_info via %gs).
 * Called from arch_cpu_init() for both BSP and APs (each CPU needs its own
 * LSTAR/STAR/FMASK settings, though the values are identical across CPUs).
 *
 * Side effects:
 *   - IA32_EFER.SCE = 1  : SYSCALL/SYSRET instructions enabled.
 *   - IA32_STAR programmed: see file header for selector encoding.
 *   - IA32_LSTAR = &syscall_entry : CPU will jump here on SYSCALL.
 *   - IA32_FMASK = 0x600 : IF and DF cleared on SYSCALL entry.
 *
 * NOTE(SYS-AMD64-01): syscall_entry (syscall.S) currently returns via iretq,
 * not sysretq.  The LSTAR/FMASK setup is correct for both; only the exit path
 * in syscall.S needs to change to use sysretq.
 */
void amd64_syscall_init(void) {
  uint64_t star;

  /* IA32_EFER.SCE = 1: enable SYSCALL/SYSRET instructions */
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
