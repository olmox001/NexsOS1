/* gdt_defs.h — generato da kernel_doctor */
#pragma once
#define GDT_KERN_CODE 0x08
#define GDT_KERN_DATA 0x10
#define GDT_USER_CODE 0x20
#define GDT_USER_DATA 0x18

/*
 * Ring-3 segment SELECTORS (GDT index | RPL), as actually loaded into a
 * register — distinct from the GDT_* descriptor-table indices above, which
 * carry no RPL. Every one of these was previously a bare literal duplicated
 * across kernel/arch/amd64/include/arch/pt_regs.h and
 * kernel/arch/amd64/cpu/syscall.S; both now use these instead.
 *
 * SYSRET (see amd64_syscall_init, kernel/arch/amd64/cpu/msr.c) always forces
 * RPL=3 into CS/SS regardless of the GDT index's own RPL bits, so
 * USER_CS_SEL/USER_DS_SEL are written here with an explicit |3 purely for
 * readability at the call sites that build a ring-3 pt_regs frame by hand
 * (syscall.S, pt_regs_init_user_task) — the value is identical either way.
 */
#define GDT_RPL_KERNEL 0
#define GDT_RPL_USER 3
#define KERN_CS_SEL (GDT_KERN_CODE | GDT_RPL_KERNEL) /* 0x08 */
#define KERN_DS_SEL (GDT_KERN_DATA | GDT_RPL_KERNEL) /* 0x10 */
#define USER_CS_SEL (GDT_USER_CODE | GDT_RPL_USER)   /* 0x23 */
#define USER_DS_SEL (GDT_USER_DATA | GDT_RPL_USER)   /* 0x1B */