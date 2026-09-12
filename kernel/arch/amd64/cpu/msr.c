/*
 * kernel/arch/amd64/cpu/msr.c
 * Model-Specific Register (MSR) Configuration for the SYSCALL Fast Path
 * (x86-64)
 *
 * Responsibilities:
 *   Configure the four MSRs that enable and control the SYSCALL/SYSRET
 *   instruction pair on x86-64:
 *     IA32_EFER  (0xC0000080) — enable SCE (System Call Enable) bit
 *     IA32_STAR  (0xC0000081) — segment selectors for SYSCALL/SYSRET
 *     IA32_LSTAR (0xC0000082) — kernel entry RIP for SYSCALL
 *     IA32_FMASK (0xC0000084) — RFLAGS bits cleared on SYSCALL entry
 *
 * STAR encoding (Intel SDM Vol.2, SYSCALL/SYSRET; AMD64 APM Vol.2 §6.1.2):
 *   STAR[47:32] is loaded into CS on SYSCALL; SS is then STAR[47:32] + 8.
 *     We program GDT_KERN_CODE (0x08) here, giving CS=0x08 (kernel code),
 *     SS=0x10 (kernel data) — matching the GDT layout in gdt.c and the
 *     KERN_CS_SEL/KERN_DS_SEL constants in gdt_defs.h.
 *   STAR[63:48] is loaded into CS on SYSRET, PLUS 16, with RPL forced to 3
 *     by the instruction itself; SS is then that same base + 8. We program
 *     GDT_KERN_DATA (0x10) here: CS = 0x10+16 = 0x20 (GDT_USER_CODE),
 *     SS = 0x10+8 = 0x18 (GDT_USER_DATA) — matching USER_CS_SEL/USER_DS_SEL
 *     (0x23/0x1B once SYSRET's forced RPL=3 is applied).
 *   This is why STAR takes GDT_KERN_DATA for its user-facing half rather
 *   than GDT_USER_CODE directly: SYSRET's fixed "+16/+8 from one base"
 *   arithmetic assumes the four selectors (kernel CS, kernel DS, user CS,
 *   user DS) sit at consecutive GDT slots 0x08/0x10/0x18/0x20 in exactly
 *   that order — which is why gdt.c's layout is not arbitrary.
 *
 * FMASK: bits set here are CLEARED from RFLAGS on SYSCALL entry, before any
 *   kernel instruction executes — X86_RFLAGS_IF so a user RFLAGS with
 *   interrupts enabled cannot leave IF set before the kernel stack switch in
 *   syscall_entry (syscall.S) is complete, and X86_RFLAGS_DF so the SysV
 *   ABI's "DF=0 on function entry" invariant holds even though nothing has
 *   executed 'cld' yet at that point (common_isr_entry does this explicitly
 *   for the interrupt path; the SYSCALL path relies on FMASK instead since
 *   there is no separate common entry stub before the C dispatcher is
 *   reached — see the file header of syscall.S).
 *
 * Known issues:
 *   SYS-AMD64-01 (W2 PERF/REFINE) The fast path is correctly configured via
 *     LSTAR (syscall_entry in syscall.S), but the return path uses 'iretq'
 *     instead of 'sysretq' (syscall.S, tail of syscall_entry). This is
 *     intentional for now (iretq tolerates a return CS/SS sysretq's fixed
 *     STAR arithmetic does not) but costs roughly 20-50 cycles per syscall
 *     return. Prerequisites for sysretq are met (RCX holds return RIP, R11
 *     holds return RFLAGS), so the switch is a drop-in change to the tail of
 *     syscall_entry when that cost is no longer acceptable.
 */
#include "gdt_defs.h"
#include <arch/amd64_internal.h>
#include <arch/arch.h>
#include <kernel/printk.h>
#include <kernel/types.h>

#define IA32_EFER 0xC0000080
#define IA32_STAR 0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_FMASK 0xC0000084

#define EFER_SCE 0x01 /* System Call Enable */

/* RFLAGS bits FMASK clears on SYSCALL entry (Intel SDM Vol.1 §3.4.3). */
#define X86_RFLAGS_IF 0x200 /* bit 9:  interrupt enable */
#define X86_RFLAGS_DF 0x400 /* bit 10: direction flag */

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
 *   - IA32_FMASK clears IF and DF on SYSCALL entry (see file header).
 *
 * NOTE(SYS-AMD64-01): syscall_entry (syscall.S) currently returns via iretq,
 * not sysretq. The LSTAR/FMASK setup here is correct for either; only the
 * exit path in syscall.S needs to change to use sysretq.
 */
void amd64_syscall_init(void) {
  /* IA32_EFER.SCE = 1: enable SYSCALL/SYSRET instructions */
  uint64_t efer = rdmsr(IA32_EFER);
  wrmsr(IA32_EFER, efer | EFER_SCE);

  /* IA32_STAR: kernel selectors in bits [47:32], user-base selector (see
   * file header for the SYSRET +16/+8 derivation) in bits [63:48]. */
  uint64_t star =
      ((uint64_t)GDT_KERN_DATA << 48) | ((uint64_t)GDT_KERN_CODE << 32);
  wrmsr(IA32_STAR, star);

  /* IA32_LSTAR: RIP the CPU jumps to on SYSCALL. */
  wrmsr(IA32_LSTAR, (uint64_t)syscall_entry);

  /* IA32_FMASK: RFLAGS bits to clear on SYSCALL entry (see file header). */
  wrmsr(IA32_FMASK, X86_RFLAGS_IF | X86_RFLAGS_DF);

  pr_info("AMD64 MSR SYSCALL configured\n");
}