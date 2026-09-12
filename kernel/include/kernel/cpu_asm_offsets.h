/*
 * kernel/include/kernel/cpu_asm_offsets.h
 * Named byte offsets into struct cpu_info (kernel/include/kernel/cpu.h) for
 * the amd64 %gs-relative accesses in hand-written assembly.
 *
 * WHY THIS FILE EXISTS.  x86-64 per-CPU data is reached through %gs with a
 * plain integer displacement — there is no struct-member syntax available to
 * the assembler. Before this header existed, kernel/arch/amd64/cpu/syscall.S
 * and kernel/arch/amd64/cpu/isr_stubs.S each spelled that displacement as a
 * bare literal (%gs:16, %gs:24, %gs:32) with nothing tying it to the actual
 * layout of struct cpu_info; a field added, removed or reordered in cpu.h
 * would silently desynchronise every one of those literals; this depended on
 * the reader remembering to physically compare an .S file against a .h file.
 *
 * This is the same problem Linux solves by generating asm-offsets.h from
 * offsetof() at build time (arch/x86/kernel/asm-offsets.c); this tree already
 * has a lighter-weight, zero-build-step mechanism for exactly this class of
 * assembly/struct coupling — NX_ASSERT_OFFSET (kernel/include/kernel/
 * nx_contract.h), used the same way for struct pt_regs in <arch/pt_regs.h>.
 * cpu.h pins each constant below against the real struct with
 * NX_ASSERT_OFFSET so a layout change that forgets to update this file is a
 * COMPILE failure, not a silent corruption discovered at runtime.
 *
 * This header has no include guard requirements beyond the usual pragma: it
 * contains nothing but integer #defines, so it is safe to #include from
 * assembly (.S, preprocessed by the C preprocessor per the Makefile's
 * `$(CC) $(CFLAGS) -c` rule) and from C alike.
 */
#ifndef _KERNEL_CPU_ASM_OFFSETS_H
#define _KERNEL_CPU_ASM_OFFSETS_H

/* offsetof(struct cpu_info, self) — must stay 0: %gs:0 is how a running CPU
 * finds its OWN cpu_info (GS_BASE points at the struct itself). */
#define CPU_INFO_SELF_OFF 0

/* offsetof(struct cpu_info, stack_top) — the kernel stack pointer loaded by
 * syscall_entry (syscall.S) when switching off the user stack. */
#define CPU_INFO_STACK_TOP_OFF 16

/* offsetof(struct cpu_info, user_stack_tmp) — scratch slot syscall_entry
 * uses to stash the user RSP for the duration of the syscall. */
#define CPU_INFO_USER_STACK_TMP_OFF 24

/* offsetof(struct cpu_info, current_task) — read by isr_stubs.S and
 * syscall.S to decide whether there is a task whose FPU/SSE state must be
 * saved/restored around the C dispatcher (NULL during early boot, before
 * the first task exists). */
#define CPU_INFO_CURRENT_TASK_OFF 32

#endif /* _KERNEL_CPU_ASM_OFFSETS_H */