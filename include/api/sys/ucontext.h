/*
 * include/api/sys/ucontext.h
 * NexsOS1 ucontext definition for AMD64 and AArch64.
 */

#ifndef _SYS_UCONTEXT_H
#define _SYS_UCONTEXT_H

#include <signal.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) || defined(ARCH_AMD64)

enum {
  REG_R8 = 0,
  REG_R9,
  REG_R10,
  REG_R11,
  REG_R12,
  REG_R13,
  REG_R14,
  REG_R15,
  REG_RDI,
  REG_RSI,
  REG_RBP,
  REG_RBX,
  REG_RDX,
  REG_RAX,
  REG_RCX,
  REG_RSP,
  REG_RIP,
  REG_EFL,
  REG_CSGSFS,
  REG_ERR,
  REG_TRAPNO,
  REG_OLDMASK,
  REG_CR2
};

#define REG_R8 REG_R8
#define REG_R9 REG_R9
#define REG_R10 REG_R10
#define REG_R11 REG_R11
#define REG_R12 REG_R12
#define REG_R13 REG_R13
#define REG_R14 REG_R14
#define REG_R15 REG_R15
#define REG_RDI REG_RDI
#define REG_RSI REG_RSI
#define REG_RBP REG_RBP
#define REG_RBX REG_RBX
#define REG_RDX REG_RDX
#define REG_RAX REG_RAX
#define REG_RCX REG_RCX
#define REG_RSP REG_RSP
#define REG_RIP REG_RIP
#define REG_EFL REG_EFL
#define REG_CSGSFS REG_CSGSFS
#define REG_ERR REG_ERR
#define REG_TRAPNO REG_TRAPNO
#define REG_OLDMASK REG_OLDMASK
#define REG_CR2 REG_CR2

typedef long long greg_t;
typedef greg_t gregset_t[23];

typedef struct {
  gregset_t gregs;
  void *fpregs;
  unsigned long __reserved1[8];
} mcontext_t;

#elif defined(__aarch64__) || defined(ARCH_AARCH64)

typedef struct {
  unsigned long long fault_address;
  unsigned long long regs[31];
  unsigned long long sp;
  unsigned long long pc;
  unsigned long long pstate;
  unsigned char __reserved[4096] __attribute__((__aligned__(16)));
} mcontext_t;

#else

typedef struct {
  unsigned long regs[32];
} mcontext_t;

#endif

#ifndef _STACK_T_DEFINED
#define _STACK_T_DEFINED
typedef struct {
  void *ss_sp;
  int ss_flags;
  size_t ss_size;
} stack_t;
#endif

typedef struct ucontext_t {
  unsigned long uc_flags;
  struct ucontext_t *uc_link;
  stack_t uc_stack;
  mcontext_t uc_mcontext;
  sigset_t uc_sigmask;
  jmp_buf __jb;
  int __jb_valid;
} ucontext_t;

int getcontext(ucontext_t *ucp);
int setcontext(const ucontext_t *ucp);
void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...);
int swapcontext(ucontext_t *restrict oucp, const ucontext_t *restrict ucp);
void __clear_cache(void *beginning, void *end);

#endif /* _SYS_UCONTEXT_H */
