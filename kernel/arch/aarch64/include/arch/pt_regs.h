/*
 * kernel/arch/aarch64/include/arch/pt_regs.h
 * AArch64 register frame layout (816 bytes)
 *
 * Must match the stack layout in exception.S exactly.
 */
#ifndef _ARCH_AARCH64_PT_REGS_H
#define _ARCH_AARCH64_PT_REGS_H

#include <stdint.h>

/* Full Register State (matching stack layout in exception.S) */
struct pt_regs {
  uint64_t regs[31]; /* x0-x30 */
  uint64_t unused;   /* Padding to align to 16 bytes (32 * 8 = 256) */
  uint64_t elr;      /* 256 */
  uint64_t spsr;     /* 264 */
  uint64_t sp_el0;   /* 272 */
  uint64_t padding;  /* 280 */

  /* FPU/SIMD State (NEON q0-q31 = 32 * 16 bytes = 512 bytes) */
  __uint128_t qregs[32]; /* 288 - 800 */
  uint32_t fpsr;         /* 800 */
  uint32_t fpcr;         /* 804 */
  uint64_t fpu_padding;  /* 808-816 */
};

/* ─── Architecture-Agnostic Accessors ─── */

/* Syscall number: x8 on AArch64 */
static inline uint64_t pt_regs_syscall_num(struct pt_regs *r) {
  return r->regs[8];
}

/* Syscall arguments: x0-x5 on AArch64 */
static inline uint64_t pt_regs_arg(struct pt_regs *r, int n) {
  return r->regs[n]; /* x0..x5 maps directly */
}

/* Set syscall return value: x0 on AArch64 */
static inline void pt_regs_set_return(struct pt_regs *r, uint64_t v) {
  r->regs[0] = v;
}

/* Get program counter (ELR_EL1) */
static inline uint64_t pt_regs_pc(struct pt_regs *r) { return r->elr; }

/* Set program counter */
static inline void pt_regs_set_pc(struct pt_regs *r, uint64_t v) {
  r->elr = v;
}

/* Get user stack pointer (SP_EL0) */
static inline uint64_t pt_regs_user_sp(struct pt_regs *r) {
  return r->sp_el0;
}

/* Set user stack pointer */
static inline void pt_regs_set_user_sp(struct pt_regs *r, uint64_t v) {
  r->sp_el0 = v;
}

/* Retry syscall: rewind PC by 4 bytes (ARM instruction size) */
static inline void pt_regs_retry_syscall(struct pt_regs *r) { r->elr -= 4; }

/* Initialize context for a user-mode process (ELF entry) */
static inline void pt_regs_init_user_task(struct pt_regs *r, uint64_t entry,
                                           uint64_t usp) {
  r->elr    = entry;
  r->sp_el0 = usp;
  r->spsr   = 0; /* EL0t, IRQs unmasked */
}

/* Pass main()'s argc/argv to a fresh user task (AArch64 AAPCS: x0, x1).
 * Call after pt_regs_init_user_task(); argv is a user virtual address. */
static inline void pt_regs_set_user_args(struct pt_regs *r, uint64_t argc,
                                         uint64_t argv) {
  r->regs[0] = argc;
  r->regs[1] = argv;
}

/* Initialize context for a kernel-mode thread (ksp unused on AArch64) */
static inline void pt_regs_init_kernel_task(struct pt_regs *r, uint64_t entry,
                                             uint64_t ksp) {
  (void)ksp;
  r->elr  = entry;
  r->spsr = 0x05; /* EL1h, IRQs unmasked */
  r->sp_el0 = 0;
}

#endif /* _ARCH_AARCH64_PT_REGS_H */
