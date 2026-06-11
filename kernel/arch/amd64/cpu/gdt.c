/*
 * kernel/arch/amd64/cpu/gdt.c
 * Global Descriptor Table (GDT) and Task State Segment (TSS) for x86-64
 *
 * Responsibilities:
 *   - Maintain per-CPU GDT (gdt_raw[MAX_CPUS][GDT_ENTRIES]) and TSS
 *     (tss_data[MAX_CPUS]) so each CPU has its own descriptor tables.
 *   - Install 5 usable segments (indices 1-4) and a 64-bit TSS descriptor
 *     at index 5-6 (TSS occupies two consecutive 8-byte slots).
 *   - Load the new GDT via a far-return sequence that atomically switches CS.
 *   - Load the TSS via 'ltr' so that hardware interrupt delivery uses the
 *     correct RSP0 from tss.rsp0.
 *   - Provide gdt_set_rsp0() so the scheduler can update RSP0 on each
 *     context switch without reloading the entire GDT.
 *
 * GDT layout (index → selector):
 *   0 → 0x00  Null descriptor (required)
 *   1 → 0x08  Kernel Code 64-bit (DPL=0, L=1, access=0x9A, gran=0xAF)
 *   2 → 0x10  Kernel Data 64-bit (DPL=0, access=0x92, gran=0xCF)
 *   3 → 0x18  User Data   32/64  (DPL=3, access=0xF2, gran=0xCF)
 *   4 → 0x20  User Code   64-bit (DPL=3, L=1, access=0xFA, gran=0xAF)
 *   5 → 0x28  TSS low word  (16-byte system descriptor, occupies slots 5+6)
 *   6 → 0x30  TSS high word
 *
 * Invariants:
 *   - gdt_raw and tss_data are statically allocated; no dynamic memory is used.
 *   - Each CPU calls gdt_init() independently; GDTR points at cpu-specific array.
 *   - tss.rsp0 is the only TSS field written at runtime (by gdt_set_rsp0).
 *
 * Known issues:
 *   GDT-AMD64-01 (W2 REFINE) User data (0x18) appears before user code (0x20).
 *     Conventional x86-64 ABI order is kern-code, kern-data, user-code,
 *     user-data.  The STAR MSR and pt_regs.h are both consistent with this
 *     reversed order, so the system functions correctly, but the ordering
 *     differs from Linux and is a source of confusion.
 */
#include <kernel/types.h>
#include <kernel/string.h>
#include <kernel/cpu.h>
#include <arch/arch.h>
#include <arch/amd64_internal.h>
#include <kernel/printk.h>
#include "gdt_defs.h"

/* ─── GDT Segment Selectors ─────────────────────────────────────────────────
 * These byte offsets into the GDT are the segment selectors loaded into CS,
 * DS, SS, etc.  RPL bits [1:0] are 0 for kernel selectors; user selectors
 * have RPL=3 but GDT index determines the descriptor, not the RPL in the
 * selector itself (the CPU checks DPL ≥ RPL for privilege).
 * ──────────────────────────────────────────────────────────────────────────*/
#define GDT_NULL      0x00
#ifndef GDT_KERN_CODE
#endif /* GDT_KERN_CODE */
#ifndef GDT_KERN_DATA
#endif /* GDT_KERN_DATA */
#ifndef GDT_USER_DATA
#endif /* GDT_USER_DATA */
#ifndef GDT_USER_CODE
#endif /* GDT_USER_CODE */
#define GDT_TSS       0x28
#define GDT_ENTRIES   7

/*
 * struct tss64 - x86-64 Task State Segment (Intel SDM Vol.3 Section 7.7).
 *
 * Only rsp0 and iopb_offset are used at runtime:
 *   rsp0:        kernel stack loaded on interrupt from Ring 3 (updated by
 *                gdt_set_rsp0 on every context switch).
 *   iopb_offset: set to sizeof(tss64) so the I/O permission bitmap is empty —
 *                all port I/O from Ring 3 generates a #GP.
 *
 * rsp1, rsp2 and ist[] would be used for more-privileged stacks and IST
 * entries (e.g. a dedicated #DF/NMI stack).  They remain zero here.
 */
struct tss64 {
  uint32_t reserved0;
  uint64_t rsp0;
  uint64_t rsp1;
  uint64_t rsp2;
  uint64_t reserved1;
  uint64_t ist[7];
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iopb_offset;
} __packed;

/*
 * struct gdt_entry - 8-byte segment descriptor (code/data segments).
 *
 * Fields per Intel SDM Vol.3 Table 3-1:
 *   limit_lo [15:0], gran[3:0] : 20-bit segment limit (0xFFFFF here → 4GB)
 *   base_lo/mid/hi             : 32-bit base address (0 for flat model)
 *   access                     : type, DPL, P, S flags
 *   granularity                : G, D/B, L, AVL, limit_hi nibble
 *
 * In 64-bit long mode most data/code segment fields are ignored except:
 *   L bit (granularity[5]) = 1 → 64-bit code segment
 *   D/B bit = 0 when L = 1 (must be 0 per SDM)
 *   DPL (access[6:5]) = privilege level
 */
struct gdt_entry {
  uint16_t limit_lo;
  uint16_t base_lo;
  uint8_t  base_mid;
  uint8_t  access;
  uint8_t  granularity;
  uint8_t  base_hi;
} __packed;

/*
 * struct gdt_system_entry - 16-byte system descriptor used for the TSS.
 *
 * The TSS descriptor occupies two consecutive 8-byte GDT slots (indices 5+6).
 * base_upper holds bits 63:32 of the TSS linear address (needed in 64-bit
 * mode since the TSS can be anywhere in the 64-bit address space).
 */
struct gdt_system_entry {
  uint16_t limit_lo;
  uint16_t base_lo;
  uint8_t  base_mid;
  uint8_t  access;
  uint8_t  granularity;
  uint8_t  base_hi;
  uint32_t base_upper;
  uint32_t reserved;
} __packed;

/*
 * struct gdtr - GDTR pseudo-descriptor for 'lgdt'.
 *   limit: byte size of GDT - 1  (GDT_ENTRIES*8 - 1)
 *   base:  linear address of the GDT array
 */
struct gdtr {
  uint16_t limit;
  uint64_t base;
} __packed;

/* Per-CPU GDT and TSS arrays, 16-byte aligned for GDTR requirements. */
static struct tss64 tss_data[MAX_CPUS] __aligned(16);
static uint64_t gdt_raw[MAX_CPUS][GDT_ENTRIES] __aligned(16);

/*
 * gdt_set_entry - fill a standard 8-byte code/data segment descriptor.
 *
 * base is always 0 (flat 64-bit model); limit is 0xFFFFF (max, with G=1 →
 * granularity in 4KB units → 4GB limit, ignored in 64-bit mode for data segs).
 * access and gran encode the descriptor type, privilege level, and 64-bit flags.
 *
 * Examples used in gdt_init:
 *   access=0x9A gran=0xAF : Kernel 64-bit code (DPL=0, L=1, D=0, G=1)
 *   access=0x92 gran=0xCF : Kernel 64-bit data (DPL=0, D=1, G=1)
 *   access=0xF2 gran=0xCF : User   32/64 data  (DPL=3, D=1, G=1)
 *   access=0xFA gran=0xAF : User   64-bit code  (DPL=3, L=1, D=0, G=1)
 */
static void gdt_set_entry(uint64_t *gdt, int index, uint8_t access, uint8_t gran) {
  struct gdt_entry *e = (struct gdt_entry *)&gdt[index];
  e->limit_lo    = 0xFFFF;
  e->base_lo     = 0;
  e->base_mid    = 0;
  e->access      = access;
  e->granularity = gran;
  e->base_hi     = 0;
}

/*
 * gdt_set_tss - fill the 16-byte TSS system descriptor at gdt[index..index+1].
 *
 * The TSS descriptor spans two 8-byte slots (the high slot holds base_upper
 * which is the bits 63:32 of the TSS physical address).  access=0x89 encodes
 * type=9 (64-bit Available TSS), DPL=0, Present=1.
 *
 * After gdt_init calls this, 'ltr' loads the TSS selector (0x28) which marks
 * the TSS as Busy (type changes from 9 to 11 in the descriptor).
 */
static void gdt_set_tss(uint64_t *gdt, int index, uint64_t base, uint32_t limit) {
  struct gdt_system_entry *e = (struct gdt_system_entry *)&gdt[index];
  e->limit_lo    = (uint16_t)(limit & 0xFFFF);
  e->base_lo     = (uint16_t)(base & 0xFFFF);
  e->base_mid    = (uint8_t)((base >> 16) & 0xFF);
  e->access      = 0x89; /* Present, DPL=0, 64-bit Available TSS */
  e->granularity = (uint8_t)((limit >> 16) & 0x0F);
  e->base_hi     = (uint8_t)((base >> 24) & 0xFF);
  e->base_upper  = (uint32_t)(base >> 32);
  e->reserved    = 0;
}

/*
 * gdt_init - build and load the GDT and TSS for the calling CPU.
 *
 * Each CPU calls this independently; gdt_raw[cpu_id] and tss_data[cpu_id]
 * are separate per-CPU arrays.  The GDTR loaded by lgdt points at the CPU's
 * private gdt_raw[] slice.
 *
 * GDT build order (NOTE GDT-AMD64-01: unconventional user-data before user-code):
 *   idx 0 : null (cleared by memset)
 *   idx 1 : 0x08 kernel code 64-bit  (access=0x9A L=1)
 *   idx 2 : 0x10 kernel data 64-bit  (access=0x92)
 *   idx 3 : 0x18 user data   (DPL=3) (access=0xF2) — before user code
 *   idx 4 : 0x20 user code   (DPL=3) (access=0xFA L=1)
 *   idx 5 : 0x28 TSS low  (16-byte descriptor spans indices 5+6)
 *
 * The far-return trick to reload CS (0x08):
 *   'lgdt' updates the GDTR but the CS shadow register still holds the old
 *   descriptor.  To atomically load the new CS the code pushes 0x08 (kernel
 *   code selector) + RIP of label 1f, then executes 'lretq' (far return),
 *   which pops CS:RIP and resumes at label 1.  DS/ES/FS/SS are then set to
 *   0x10 (kernel data) with movw.
 *
 * TSS: iopb_offset = sizeof(tss64) places the IOPB past the end of the TSS,
 * effectively denying all port I/O to Ring 3 (any access generates #GP).
 *
 * 'ltr' loads the TSS register (selector 0x28 = GDT_TSS) and marks the TSS
 * as Busy.  RSP0 in the TSS is initially 0; it is set by the first call to
 * gdt_set_rsp0 before the first Ring-3 entry.
 */
/*
 * Per-CPU IST fault stacks — Phase A step 3.
 *
 * Without IST every exception reuses the current RSP (kernel fault) or
 * TSS.RSP0 = the current task's kernel stack (user fault): a recursive #PF
 * overflowed that stack into #DF (also IST=0) and triple-faulted — the
 * SCHED-UAF-01 failure signature.  idt_init points #PF/#GP at IST1 and #DF at
 * IST2 (separate, so a #PF storm cannot clobber the #DF stack).
 *
 * 16KB per stack, sized for handler + fault_printf dump + backtrace walk.
 * In .bss → always mapped in kernel_pgd.  A nested fault on the same IST
 * index re-enters at the top and clobbers the outer frame; that is caught
 * immediately by the fault_enter() depth guard before any state is trusted.
 */
#define IST_STACK_SIZE 16384
static uint8_t ist1_fault_stacks[MAX_CPUS][IST_STACK_SIZE] __aligned(16);
static uint8_t ist2_df_stacks[MAX_CPUS][IST_STACK_SIZE] __aligned(16);

void gdt_init(void) {
  uint32_t cpu_id = arch_get_cpu_id();
  if (cpu_id >= MAX_CPUS) return;

  uint64_t *my_gdt = gdt_raw[cpu_id];
  struct tss64 *my_tss = &tss_data[cpu_id];

  memset(my_gdt, 0, GDT_ENTRIES * 8);
  memset(my_tss, 0, sizeof(struct tss64));

  /* IST entries are 1-based in the IDT gate; ist[0] here is IST1.
   * IST1 = #PF/#GP fault stack, IST2 = #DF stack (see idt_init). */
  my_tss->ist[0] = (uint64_t)&ist1_fault_stacks[cpu_id][IST_STACK_SIZE];
  my_tss->ist[1] = (uint64_t)&ist2_df_stacks[cpu_id][IST_STACK_SIZE];

  /* NOTE(GDT-AMD64-01): user-data (idx 3, sel 0x18) is before user-code
   * (idx 4, sel 0x20).  This is internally consistent with msr.c STAR and
   * pt_regs.h but reverses the conventional ordering. */
  gdt_set_entry(my_gdt, 1, 0x9A, 0xAF); /* kernel code 64-bit */
  gdt_set_entry(my_gdt, 2, 0x92, 0xCF); /* kernel data 64-bit */
  gdt_set_entry(my_gdt, 3, 0xF2, 0xCF); /* user data DPL=3 */
  gdt_set_entry(my_gdt, 4, 0xFA, 0xAF); /* user code 64-bit DPL=3 */

  my_tss->iopb_offset = sizeof(struct tss64);
  gdt_set_tss(my_gdt, 5, (uint64_t)my_tss, sizeof(struct tss64) - 1);

  struct gdtr gdtr = {
    .limit = (GDT_ENTRIES * 8) - 1,
    .base  = (uint64_t)my_gdt
  };

  pr_info("GDT: Loading GDTR...\n");
  __asm__ __volatile__(
    "lgdt %0\n\t"
    /* Far-return to atomically reload CS with the new GDT's kernel code sel */
    "pushq $0x08\n\t"          /* new CS = 0x08 (kernel code) */
    "leaq 1f(%%rip), %%rax\n\t"/* new RIP = label 1 */
    "pushq %%rax\n\t"
    "lretq\n\t"                 /* pop CS:RIP → now running with new GDT CS */
    "1:\n\t"
    "movw $0x10, %%ax\n\t"     /* 0x10 = kernel data selector */
    "movw %%ax, %%ds\n\t"
    "movw %%ax, %%es\n\t"
    "movw %%ax, %%fs\n\t"
    "movw %%ax, %%ss\n\t"
    :
    : "m"(gdtr)
    : "rax", "memory"
  );

  pr_info("GDT: Loading TSS...\n");
  __asm__ __volatile__("ltr %0" :: "r"((uint16_t)GDT_TSS));
  pr_info("GDT: Done\n");
}

/*
 * gdt_set_rsp0 - update TSS.RSP0 for the calling CPU.
 *
 * Called by arch_cpu_switch_context on every scheduler switch.  RSP0 is the
 * kernel stack pointer the CPU loads on a Ring-3→Ring-0 privilege transition
 * (interrupt or exception delivery).  If this is not updated, a Ring-3 fault
 * would push the interrupt frame onto the PREVIOUS task's kernel stack.
 *
 * Params:
 *   rsp0 - top of the next task's kernel stack (high address, stack descends).
 */
void gdt_set_rsp0(uint64_t rsp0) {
  uint32_t cpu_id = arch_get_cpu_id();
  if (cpu_id < MAX_CPUS) {
    tss_data[cpu_id].rsp0 = rsp0;
  }
}
