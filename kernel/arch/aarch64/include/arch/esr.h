/*
 * kernel/arch/aarch64/include/arch/esr.h
 * ESR_EL1 (Exception Syndrome Register) and PSTATE decode constants.
 *
 * Single source of truth for the EC (Exception Class) values ARM ARM
 * (DDI0487, D13.2.37) assigns in ESR_ELx[31:26], named after Linux's own
 * arch/arm64/include/asm/esr.h so the mapping from magic hex to meaning is
 * never a bare literal again. Before this header existed, ESR_ELx_EC_SHIFT
 * (26), the 0x3F EC mask and the SVC64 class (0x15) were duplicated as bare
 * integer literals in kernel/arch/aarch64/cpu/cpu.c (sync_handler) AND
 * kernel/arch/aarch64/cpu/syscall.c (syscall_handler) — the two files that
 * must agree on what "this is a syscall, not a fault" means; nothing tied
 * the two copies together.
 *
 * Only the exception classes this kernel actually branches on are named
 * here; consult the ARM ARM or Linux's esr.h for the full EC space.
 */
#ifndef _ARCH_AARCH64_ESR_H
#define _ARCH_AARCH64_ESR_H

#define ESR_ELx_EC_SHIFT 26U
#define ESR_ELx_EC_MASK 0x3FU
#define ESR_ELx_EC(esr) (((esr) >> ESR_ELx_EC_SHIFT) & ESR_ELx_EC_MASK)

/* ISS (Instruction Specific Syndrome), ESR_ELx[24:0]. sync_handler's fuller
 * dump additionally masks the low 24 bits only ([23:0]) when printing
 * "EC/ISS" for a human, which is a display choice, not the architectural
 * field width — the full ISS field is 25 bits. */
#define ESR_ELx_ISS_MASK 0x1FFFFFFU

#define ESR_ELx_EC_UNKNOWN 0x00U  /* Unknown reason */
#define ESR_ELx_EC_WFx 0x01U      /* WFI/WFE trapped */
#define ESR_ELx_EC_SVC64 0x15U    /* SVC instruction, AArch64 (syscall) */
#define ESR_ELx_EC_IABT_LOW 0x20U /* Instruction Abort, lower EL (EL0) */
#define ESR_ELx_EC_IABT_CUR 0x21U /* Instruction Abort, same EL (EL1) */
#define ESR_ELx_EC_DABT_LOW 0x24U /* Data Abort, lower EL (EL0) */
#define ESR_ELx_EC_DABT_CUR 0x25U /* Data Abort, same EL (EL1) */
#define ESR_ELx_EC_SP_ALIGN 0x26U /* SP alignment fault */

/*
 * PSTATE.M (SPSR_EL1[3:0]) mode field: distinguishes an EL0 (user) origin
 * exception frame from an EL1 (kernel) one. EL0t (0b0000) is the only user
 * mode on this kernel (no AArch32 EL0 support) — matches Linux's
 * PSR_MODE_EL0t in arch/arm64/include/asm/ptrace.h.
 */
#define PSTATE_M_MASK 0xFU
#define PSTATE_M_EL0T 0x0U

#endif /* _ARCH_AARCH64_ESR_H */