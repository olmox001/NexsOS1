/*
 * kernel/arch/amd64/include/arch/pt_regs.h
 * x86-64 register frame layout
 *
 * Must match the stack layout in isr_stubs.S and syscall.S exactly.
 * Layout: GP regs pushed by stub, then CPU-pushed interrupt frame.
 */
#ifndef _ARCH_AMD64_PT_REGS_H
#define _ARCH_AMD64_PT_REGS_H

#include <kernel/nx_contract.h>
#include <stdint.h>

/*
 * Full Register State for x86-64 exceptions/syscalls.
 * Pushed by isr_stubs.S (GP regs) + CPU (iret frame).
 */
struct pt_regs {
  /* Pushed by software (isr_stubs.S common_isr_entry) */
  uint64_t r15;
  uint64_t r14;
  uint64_t r13;
  uint64_t r12;
  uint64_t r11;
  uint64_t r10;
  uint64_t r9;
  uint64_t r8;
  uint64_t rbp;
  uint64_t rdi;
  uint64_t rsi;
  uint64_t rdx;
  uint64_t rcx;
  uint64_t rbx;
  uint64_t rax;

  /* Pushed by ISR stub */
  uint64_t vec; /* interrupt/exception vector number */
  uint64_t err; /* error code (0 if none) */

  /* CPU-pushed interrupt/iretq frame */
  uint64_t rip;
  uint64_t cs;
  uint64_t rflags;
  uint64_t rsp; /* user RSP (only valid from Ring 3) */
  uint64_t ss;  /* user SS  (only valid from Ring 3) */
};

/*
 * The layout above is built BY HAND in assembly — syscall_entry in
 * kernel/arch/amd64/cpu/syscall.S pushes these fields one at a time, and
 * common_isr_entry in isr_stubs.S does the same for the interrupt path.  The C
 * struct and those push sequences are two statements of one layout with
 * nothing connecting them, so inserting a field here would silently shift
 * every offset the assembly writes and the dispatcher reads.
 *
 * `rip` at 0x88 is not a decorative number: it is the offset the compiler
 * emits for pt_regs_retry_syscall's `r->rip -= 2`, and a kernel #PF writing to
 * frame+0x88 is what made that visible.  Pinning these three makes a reorder a
 * BUILD failure at the edit instead of a page fault at runtime.
 */
NX_ASSERT_OFFSET(struct pt_regs, rbx, 0x68);
NX_ASSERT_OFFSET(struct pt_regs, rax, 0x70);
NX_ASSERT_OFFSET(struct pt_regs, vec, 0x78);
NX_ASSERT_OFFSET(struct pt_regs, err, 0x80);
NX_ASSERT_OFFSET(struct pt_regs, rip, 0x88);
NX_ASSERT_SIZE(struct pt_regs, 0xB0);

/* ─── Architecture-Agnostic Accessors ─── */

/* Syscall number: RAX on x86-64 (Linux convention) */
static inline uint64_t pt_regs_syscall_num(struct pt_regs *r) {
  return r->rax;
}

/* Syscall arguments: RDI, RSI, RDX, R10, R8, R9 (Linux x86-64 ABI) */
static inline uint64_t pt_regs_arg(struct pt_regs *r, int n) {
  switch (n) {
  case 0:
    return r->rdi;
  case 1:
    return r->rsi;
  case 2:
    return r->rdx;
  case 3:
    return r->r10; /* R10 replaces RCX (clobbered by syscall) */
  case 4:
    return r->r8;
  case 5:
    return r->r9;
  default:
    return 0;
  }
}

/* Set syscall return value: RAX on x86-64 */
static inline void pt_regs_set_return(struct pt_regs *r, uint64_t v) {
  r->rax = v;
}

/* Get program counter (RIP) */
static inline uint64_t pt_regs_pc(struct pt_regs *r) { return r->rip; }

/* Set program counter */
static inline void pt_regs_set_pc(struct pt_regs *r, uint64_t v) {
  r->rip = v;
}

/* Get user stack pointer (RSP from iret frame) */
static inline uint64_t pt_regs_user_sp(struct pt_regs *r) { return r->rsp; }

/* Set user stack pointer */
static inline void pt_regs_set_user_sp(struct pt_regs *r, uint64_t v) {
  r->rsp = v;
}

/* Retry syscall: rewind PC by 2 bytes (syscall instruction is 0F 05) */
static inline void pt_regs_retry_syscall(struct pt_regs *r) { r->rip -= 2; }

/* Initialize context for a user-mode process (ELF entry)
 * GDT_USER_CODE=0x20 → selector 0x23 (ring3), GDT_USER_DATA=0x18 → 0x1B */
static inline void pt_regs_init_user_task(struct pt_regs *r, uint64_t entry,
                                           uint64_t usp) {
  r->rip    = entry;
  r->rsp    = usp;
  r->rflags = 0x202;  /* IF + reserved */
  r->cs     = 0x23;   /* user code segment, ring 3 */
  r->ss     = 0x1B;   /* user data segment, ring 3 */
  
  /* For AMD64, SYSRET uses RCX for RIP and R11 for RFLAGS. 
   * If we return via sysretq path (context switch during syscall),
   * these must be correctly initialized. */
  r->rcx = entry;
  r->r11 = 0x202;
}

/* Pass main()'s argc/argv to a fresh user task (System V AMD64: rdi, rsi).
 * Call after pt_regs_init_user_task(); argv is a user virtual address. */
static inline void pt_regs_set_user_args(struct pt_regs *r, uint64_t argc,
                                         uint64_t argv) {
  r->rdi = argc;
  r->rsi = argv;
}

/* Initialize context for a kernel-mode thread */
static inline void pt_regs_init_kernel_task(struct pt_regs *r, uint64_t entry,
                                             uint64_t ksp) {
  r->rip    = entry;
  r->cs     = 0x08;   /* kernel code segment */
  r->rflags = 0x202;  /* IF + reserved */
  r->rsp    = ksp;
  r->ss     = 0x10;   /* kernel data segment */
}

#endif /* _ARCH_AMD64_PT_REGS_H */
