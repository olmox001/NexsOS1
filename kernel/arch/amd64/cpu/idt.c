/*
 * kernel/arch/amd64/cpu/idt.c
 * Interrupt Descriptor Table (IDT) and Exception Dispatcher for x86-64
 *
 * Responsibilities:
 *   - Define the 256-entry 64-bit IDT (struct idt_entry, 16 bytes each).
 *   - Install all 256 ISR stubs (from isr_stub_table[] in isr_stubs.S) as
 *     kernel-only interrupt gates (IDT_GATE_KERNEL_INTERRUPT). There is no
 *     user-callable gate: the only syscall entry is syscall/sysret via LSTAR
 *     (SYS-AMD64-03).
 *   - Dispatch CPU exceptions (vectors 0-31) and hardware IRQs (32-255) from
 *     the single C entry point amd64_isr_dispatch.
 *   - Acknowledge the LAPIC and legacy 8259 PIC after hardware IRQs.
 *
 * Invariants:
 *   - The IDT is a single shared array; all CPUs load the same base address.
 *     Only CPU 0 fills the table (guarded by idt_initialized). APs spin-wait
 *     on a load-acquire of that flag (EXC-AMD64-04, resolved below).
 *   - Every hardware IRQ (vec 32-255) ends through irq_chip_end(): the chip
 *     (pic_chip_end) owns the full LAPIC + 8259 EOI sequence. Spurious
 *     IRQ7/IRQ15 (IRQ7_VECTOR/IRQ15_VECTOR) and the LAPIC spurious vector
 *     (SPURIOUS_APIC_VECTOR) are filtered before dispatch and follow their
 *     own EOI rules.
 *   - Every vector number that means something outside this file is a named
 *     constant from <arch/irq_vectors.h> (hardware IRQ range) or the
 *     `enum x86_trap` below (CPU exception range) — never a bare literal.
 *
 * Known issues (resolved history):
 *   EXC-AMD64-01 RESOLVED (Phase A step 14): the dead probe-recovery block
 *     and the amd64-local probe_in_progress/probe_failed flags were removed.
 *     Memory probing is an aarch64-only fallback (FDT parse failure); its
 *     flags and the working ELR_EL1 fixup live in aarch64/cpu/cpu.c. amd64
 *     gets its memory map from PVH/multiboot and never probes.
 *   EXC-AMD64-02 RESOLVED (Phase A): every vector 0-31 routes through
 *     fault_handle_user_or_panic (kernel/core/fault.c) — user faults
 *     terminate the process and schedule a successor; kernel faults dump,
 *     print a symbolized backtrace and panic on the IST fault stacks.
 *   EXC-AMD64-04 RESOLVED: idt_initialized was a plain `static int` with no
 *     volatile qualifier and no memory barrier. An AP spinning on
 *     `while (!idt_initialized)` was free to cache a stale zero forever, and
 *     even if it re-read memory it had no guarantee that CPU 0's 256 gate
 *     writes were globally visible before it executed `lidt` on the strength
 *     of that flag. Fixed with an explicit release store on the writer side
 *     (__ATOMIC_RELEASE, after every idt_set_gate() call) and an acquire load
 *     on the reader side (__ATOMIC_ACQUIRE): the acquire load is guaranteed
 *     to observe every write that happened-before the matching release, so
 *     an AP that sees idt_initialized==1 also sees a fully populated table.
 *   SYS-AMD64-03 RESOLVED (#168): the legacy int 0x80 gate (DPL=3) was
 *     removed. Vector 0x80 is a kernel-only gate like every other; the sole
 *     syscall entry is syscall/sysret via LSTAR. A userland int 0x80 raises
 *     #GP and is handled as a user fault.
 *   CPU-AMD64-01 RESOLVED: FPU/SSE state is saved to the current task before
 *     either interrupt dispatcher runs and restored from the selected task
 *     immediately before returning to it (ISR and SYSCALL paths).
 */
#include "gdt_defs.h" /* GDT_KERN_CODE — kernel CS selector for every gate */
#include <arch/amd64_internal.h>
#include <arch/arch.h>
#include <arch/irq_vectors.h>
#include <arch/pt_regs.h>
#include <drivers/timer.h> /* HZ, for Tier-2 per-CPU tick accounting */
#include <kernel/arch.h>
#include <kernel/cpu.h>
#include <kernel/fault.h>
#include <kernel/irq.h>
#include <kernel/nx_contract.h>
#include <kernel/printk.h>
#include <kernel/string.h>
#include <kernel/types.h>

#define IDT_ENTRIES 256U

/*
 * enum x86_trap - CPU exception vector numbers (Intel SDM Vol.3 Table 6-1),
 * named to match Linux's arch/x86/include/asm/trapnr.h so the mapping from
 * vector number to fault is never a bare literal in a switch/case again.
 * The subset with a non-zero hardware error code (marked *ERR*) must stay in
 * lockstep with the ISR_ERR/ISR_NOERR emission in isr_stubs.S.
 */
enum x86_trap {
  X86_TRAP_DE = 0,  /* Divide-by-zero */
  X86_TRAP_DB = 1,  /* Debug */
  X86_TRAP_NMI = 2, /* Non-Maskable Interrupt */
  X86_TRAP_BP = 3,  /* Breakpoint (INT3) */
  X86_TRAP_OF = 4,  /* Overflow (INTO) */
  X86_TRAP_BR = 5,  /* Bound Range Exceeded */
  X86_TRAP_UD = 6,  /* Invalid Opcode */
  X86_TRAP_NM = 7,  /* Device Not Available (x87 not present) */
  X86_TRAP_DF = 8,  /* *ERR* Double Fault */
  X86_TRAP_OLD_MF =
      9,            /* Coprocessor Segment Overrun (legacy, unused since 486) */
  X86_TRAP_TS = 10, /* *ERR* Invalid TSS */
  X86_TRAP_NP = 11, /* *ERR* Segment Not Present */
  X86_TRAP_SS = 12, /* *ERR* Stack-Segment Fault */
  X86_TRAP_GP = 13, /* *ERR* General Protection Fault */
  X86_TRAP_PF = 14, /* *ERR* Page Fault (CR2 holds faulting address) */
  X86_TRAP_SPURIOUS = 15, /* reserved by Intel */
  X86_TRAP_MF = 16,       /* x87 Floating-Point Exception */
  X86_TRAP_AC = 17,       /* *ERR* Alignment Check */
  X86_TRAP_MC = 18,       /* Machine Check (not recoverable; no error code) */
  X86_TRAP_XF = 19,       /* SIMD Floating-Point Exception (SSE/AVX) */
  X86_TRAP_VE = 20, /* Virtualization Exception (EPT violation, no VMX-root) */
  X86_TRAP_CP = 21, /* *ERR* Control-Protection Exception (CET) */
  X86_TRAP_VC = 29, /* *ERR* VMM Communication Exception (AMD SEV-ES) */
  X86_TRAP_SX = 30, /* *ERR* Security Exception */
};

/*
 * x86_trap_mnemonics[] - fault name for every defined vector 0-31, indexed by
 * enum x86_trap. Reserved/unassigned indices are left NULL; x86_trap_name()
 * below is the only reader and supplies the "Reserved Exception" fallback so
 * a hole in this table can never format a NULL through "%s".
 */
static const char *const x86_trap_mnemonics[32] = {
    [X86_TRAP_DE] = "Divide Error",
    [X86_TRAP_DB] = "Debug Exception",
    [X86_TRAP_NMI] = "Non-Maskable Interrupt",
    [X86_TRAP_BP] = "Breakpoint",
    [X86_TRAP_OF] = "Overflow",
    [X86_TRAP_BR] = "Bound Range Exceeded",
    [X86_TRAP_UD] = "Invalid Opcode",
    [X86_TRAP_NM] = "Device Not Available",
    [X86_TRAP_DF] = "Double Fault",
    [X86_TRAP_OLD_MF] = "Coprocessor Segment Overrun",
    [X86_TRAP_TS] = "Invalid TSS",
    [X86_TRAP_NP] = "Segment Not Present",
    [X86_TRAP_SS] = "Stack-Segment Fault",
    [X86_TRAP_GP] = "General Protection Fault",
    [X86_TRAP_PF] = "Page Fault",
    [X86_TRAP_MF] = "x87 Floating-Point Exception",
    [X86_TRAP_AC] = "Alignment Check",
    [X86_TRAP_MC] = "Machine Check",
    [X86_TRAP_XF] = "SIMD Floating-Point Exception",
    [X86_TRAP_VE] = "Virtualization Exception",
    [X86_TRAP_CP] = "Control Protection Exception",
    [X86_TRAP_VC] = "VMM Communication Exception",
    [X86_TRAP_SX] = "Security Exception",
};

/* x86_trap_name - "%s"-safe fault mnemonic for vec (0-31 only; the caller in
 * amd64_isr_dispatch already guards vec < 32 before using this). */
static const char *x86_trap_name(uint64_t vec) {
  const char *name = (vec < 32U) ? x86_trap_mnemonics[vec] : NULL;
  return name ? name : "Reserved Exception";
}

/*
 * IDT gate encoding (Intel SDM Vol.3 Table 3-2, type_attr byte):
 *   bit 7    Present
 *   bits 6:5 DPL
 *   bit  4   S (0 for interrupt/trap gates)
 *   bits 3:0 Type (0xE = 64-bit interrupt gate: hardware clears IF on entry;
 *                  0xF = 64-bit trap gate: IF left as-is — unused in this
 *                  kernel but named for parity with the SDM/Linux tables)
 * IDT_GATE_KERNEL_INTERRUPT composes to 0x8E: Present, DPL=0, interrupt gate.
 * This is the ONLY gate type installed anywhere in idt_init(): there is no
 * DPL=3 gate in this table (SYS-AMD64-03).
 */
#define IDT_GATE_TYPE_INTERRUPT 0x0EU
#define IDT_GATE_TYPE_TRAP 0x0FU
#define IDT_GATE_PRESENT 0x80U
#define IDT_GATE_DPL_SHIFT 5U
#define IDT_GATE_DPL(dpl) (((uint8_t)(dpl) & 0x3U) << IDT_GATE_DPL_SHIFT)
#define IDT_GATE_KERNEL_INTERRUPT                                              \
  (uint8_t)(IDT_GATE_PRESENT | IDT_GATE_DPL(0) |                               \
            IDT_GATE_TYPE_INTERRUPT) /* 0x8E */

/*
 * Interrupt Stack Table slot selection for idt_set_gate()'s `ist` argument.
 * The gate's IST field is 1-based; IDT_IST_FAULT_STACK == 1 selects
 * TSS.ist[0], IDT_IST_DOUBLE_FAULT == 2 selects TSS.ist[1]. Both stacks are
 * carved out and installed into the TSS by gdt_init() (kernel/arch/amd64/cpu/
 * gdt.c) — the names here MUST agree with the ist[] indices written there.
 *
 *   IDT_IST_FAULT_STACK   -> #GP (X86_TRAP_GP) and #PF (X86_TRAP_PF): the
 *                            handler always runs on a fresh, always-mapped
 *                            stack even if the faulting RSP is wild or the
 *                            task's kernel stack has overflowed.
 *   IDT_IST_DOUBLE_FAULT  -> #DF (X86_TRAP_DF): a separate index so that even
 *                            a #PF/#GP storm that clobbers the fault stack
 *                            cannot take down double-fault reporting too.
 *   IDT_IST_NONE          -> every other vector: run on the current RSP
 *                            (kernel fault) or TSS.RSP0 (user fault).
 *
 * NMI (X86_TRAP_NMI) deliberately stays IDT_IST_NONE: a correct IST-based NMI
 * entry needs the paranoid swapgs-detection sequence Linux uses for NMI/MCE,
 * which is out of scope here (see Phase A plan, Risks) — nested NMI is a
 * known gap, not an oversight.
 */
#define IDT_IST_NONE 0U
#define IDT_IST_FAULT_STACK 1U
#define IDT_IST_DOUBLE_FAULT 2U

/*
 * struct idt_entry - x86-64 64-bit interrupt/trap gate descriptor (16 bytes).
 *
 * Fields per Intel SDM Vol.3 Table 3-2:
 *   offset_lo  [15:0]  : bits 0-15 of handler RIP
 *   selector   [15:0]  : code segment selector (GDT_KERN_CODE for every gate)
 *   ist        [2:0]   : Interrupt Stack Table index (see IDT_IST_* above)
 *   type_attr  [7:0]   : gate type + DPL + present (see IDT_GATE_* above)
 *   offset_mid [15:0]  : bits 16-31 of handler RIP
 *   offset_hi  [31:0]  : bits 32-63 of handler RIP
 *   zero       [31:0]  : reserved, must be 0
 */
struct idt_entry {
  uint16_t offset_lo;
  uint16_t selector;
  uint8_t ist;       /* Interrupt Stack Table offset */
  uint8_t type_attr; /* Type and Attributes */
  uint16_t offset_mid;
  uint32_t offset_hi;
  uint32_t zero;
} __packed;
NX_ASSERT_SIZE(struct idt_entry, 16);

/*
 * struct idtr - IDTR pseudo-descriptor loaded with 'lidt'.
 *   limit: byte length of IDT - 1  (256*16 - 1 = 0xFFF)
 *   base:  linear address of idt[]
 */
struct idtr {
  uint16_t limit;
  uint64_t base;
} __packed;

/* idt[]: the 256-entry Interrupt Descriptor Table, 16-byte aligned for lidt.
 * All CPUs share this single table; it is filled only by CPU 0. */
static struct idt_entry idt[IDT_ENTRIES] __aligned(16);

/* Defined in isr_stubs.S — 256-entry array of isr_stub_N addresses. */
extern uint64_t isr_stub_table[];

/*
 * idt_set_gate - write one IDT descriptor.
 *
 * num:   vector index 0-255 (uint8_t domain matches IDT_ENTRIES exactly —
 *        NX_ASSERT_SIZE below pins that invariant at compile time, so no
 *        runtime range check on `num` is needed or possible to violate)
 * base:  64-bit handler address (isr_stub_N from isr_stub_table[])
 * sel:   code segment selector (GDT_KERN_CODE for every gate in this table)
 * flags: type_attr byte — IDT_GATE_KERNEL_INTERRUPT for every gate here
 * ist:   IST stack index — one of IDT_IST_NONE/IDT_IST_FAULT_STACK/
 *        IDT_IST_DOUBLE_FAULT
 *
 * The 64-bit offset is split across offset_lo/mid/hi as required by the
 * x86-64 descriptor format.
 */
static void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel,
                         uint8_t flags, uint8_t ist) {
  idt[num].offset_lo = (uint16_t)(base & 0xFFFFU);
  idt[num].selector = sel;
  idt[num].ist = ist;
  idt[num].type_attr = flags;
  idt[num].offset_mid = (uint16_t)((base >> 16) & 0xFFFFU);
  idt[num].offset_hi = (uint32_t)(base >> 32);
  idt[num].zero = 0;
}

/* NX_ASSERT_SIZE above already guarantees IDT_ENTRIES fits uint8_t exactly;
 * restate it as a value assertion too so a future edit to IDT_ENTRIES alone
 * (without touching idt_set_gate's prototype) fails the build instead of
 * silently truncating vector numbers >= 256. */
_Static_assert(IDT_ENTRIES - 1U <= 0xFFU,
               "idt_set_gate() takes uint8_t: IDT_ENTRIES must stay <= 256");

/*
 * idt_initialized - set by CPU 0 after filling the IDT; APs spin on it.
 *
 * EXC-AMD64-04 (resolved): declared volatile AND accessed exclusively through
 * __atomic_store_n(..., __ATOMIC_RELEASE) / __atomic_load_n(...,
 * __ATOMIC_ACQUIRE). volatile alone stops the compiler from caching the value
 * in a register across loop iterations; it says nothing about the ORDER in
 * which other CPUs observe CPU 0's earlier writes, which is what the
 * explicit release/acquire pair provides — an AP that observes the acquire
 * load return 1 is guaranteed to also observe every one of CPU 0's 256+3
 * idt_set_gate() writes that preceded the release store.
 */
static volatile int idt_initialized = 0;

/*
 * idt_init - initialise and load the IDT for the calling CPU.
 *
 * CPU 0 path: fills all 256 entries with IDT_GATE_KERNEL_INTERRUPT gates and
 * publishes idt_initialized = 1 with a release store when done. SYS-AMD64-03
 * (#168): no DPL-3 gate is installed anywhere — the legacy int 0x80 syscall
 * surface was removed; syscall/sysret via LSTAR is the sole syscall entry.
 *
 * AP path: spins on an acquire load of idt_initialized (see EXC-AMD64-04
 * above for why this must be an acquire, not a plain read), then executes
 * lidt with a CPU-local idtr pointing at the shared idt[]. Each CPU must
 * execute lidt itself; the IDTR is a per-CPU register.
 */
void idt_init(void) {
  if (arch_get_cpu_id() == 0) {
    memset(&idt, 0, sizeof(struct idt_entry) * IDT_ENTRIES);

    /* All 256 vectors start as kernel-only interrupt gates on the default
     * (non-IST) stack; the three fault vectors below are then upgraded to a
     * dedicated IST stack. */
    for (unsigned i = 0; i < IDT_ENTRIES; i++) {
      idt_set_gate((uint8_t)i, isr_stub_table[i], GDT_KERN_CODE,
                   IDT_GATE_KERNEL_INTERRUPT, IDT_IST_NONE);
    }

    /* SYS-AMD64-03 (#168): vector 0x80 intentionally stays a kernel-only gate
     * (DPL=0, set by the loop above). The legacy int 0x80 syscall surface is
     * removed; the only syscall entry is syscall/sysret via LSTAR. A
     * userland int 0x80 now raises #GP and is handled as a user fault. */

    /* Dedicated IST fault stacks (Phase A step 3; stacks live in gdt.c) —
     * see the IDT_IST_* block comment above for the full rationale. */
    idt_set_gate(X86_TRAP_DF, isr_stub_table[X86_TRAP_DF], GDT_KERN_CODE,
                 IDT_GATE_KERNEL_INTERRUPT, IDT_IST_DOUBLE_FAULT);
    idt_set_gate(X86_TRAP_GP, isr_stub_table[X86_TRAP_GP], GDT_KERN_CODE,
                 IDT_GATE_KERNEL_INTERRUPT, IDT_IST_FAULT_STACK);
    idt_set_gate(X86_TRAP_PF, isr_stub_table[X86_TRAP_PF], GDT_KERN_CODE,
                 IDT_GATE_KERNEL_INTERRUPT, IDT_IST_FAULT_STACK);

    /* Publish the fully-populated table with a release store: every write
     * above is guaranteed visible to any AP whose acquire load below
     * observes the 1 this store writes. */
    __atomic_store_n(&idt_initialized, 1, __ATOMIC_RELEASE);
  }

  /* AP path (and CPU 0 falls through here too, trivially true by then):
   * acquire-load pairs with the release store above. arch_yield() issues
   * `pause` on amd64 — the correct primitive for a busy-wait spin, unlike
   * a bare nop it lets the core de-prioritise the spin for the hyperthread
   * partner and avoids a memory-order pipeline flush on exit. */
  while (!__atomic_load_n(&idt_initialized, __ATOMIC_ACQUIRE)) {
    arch_yield();
  }

  struct idtr local_idtr;
  local_idtr.limit = (uint16_t)(sizeof(struct idt_entry) * IDT_ENTRIES - 1U);
  local_idtr.base = (uint64_t)&idt;

  __asm__ __volatile__("lidt %0" : : "m"(local_idtr));
}

/*
 * fault_cpu_id - CPU id for fault banners, via the MSR-based safe path.
 * Returns -1 when the per-CPU structure cannot be located (early boot or
 * corrupted GS); never touches LAPIC MMIO (kernel/fault.h, step 5).
 */
static int fault_cpu_id(void) {
  struct cpu_info *ci = arch_cpu_info_fault_safe();
  return ci ? (int)ci->cpu_id : -1;
}

/*
 * amd64_dump_regs - print all saved registers from the exception frame.
 *
 * Params:
 *   regs - pointer to the pt_regs struct built by common_isr_entry.
 * Called unconditionally before halting on any unhandled exception.
 *
 * Uses fault_printf (lock-free, no per-CPU buffer, no LAPIC reads): by the
 * time this runs the address space and lock state may be arbitrary, and a
 * printk here is exactly the recursive-fault chain that used to end in a
 * triple fault (SCHED-UAF-01 post-mortem).
 */
static void amd64_dump_regs(struct pt_regs *regs) {
  fault_printf("RIP: %016lx CS: %02lx RFLAGS: %016lx\n", regs->rip, regs->cs,
               regs->rflags);
  fault_printf("RAX: %016lx RBX: %016lx RCX: %016lx RDX: %016lx\n", regs->rax,
               regs->rbx, regs->rcx, regs->rdx);
  fault_printf("RSI: %016lx RDI: %016lx RBP: %016lx RSP: %016lx\n", regs->rsi,
               regs->rdi, regs->rbp, regs->rsp);
  fault_printf("R8:  %016lx R9:  %016lx R10: %016lx R11: %016lx\n", regs->r8,
               regs->r9, regs->r10, regs->r11);
  fault_printf("R12: %016lx R13: %016lx R14: %016lx R15: %016lx\n", regs->r12,
               regs->r13, regs->r14, regs->r15);
  fault_printf("Vector: %ld (%s), Error Code: %lx\n", regs->vec,
               x86_trap_name(regs->vec), regs->err);
}

/*
 * amd64_page_fault_handler - handle X86_TRAP_PF (#PF).
 *
 * CR2 holds the faulting linear address. Error code bits (Intel SDM Vol.3
 * Section 6.15):
 *   bit 0  P   : 0 = not-present, 1 = protection violation
 *   bit 1  W/R : 0 = read, 1 = write
 *   bit 2  U/S : 0 = kernel, 1 = user
 *   bit 3  RSVD: reserved-bit violation
 *   bit 4  I/D : instruction fetch (NX violation)
 *
 * EXC-AMD64-02 resolved: a user #PF terminates the process (via
 * fault_handle_user_or_panic) and schedules a successor instead of halting
 * the kernel; only a kernel-mode #PF reaches the panic below.
 */
static struct pt_regs *amd64_page_fault_handler(struct pt_regs *regs) {
  uint64_t cr2;
  __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));

  uint64_t error_code = regs->err;

  struct pt_regs *next =
      fault_handle_user_or_panic(regs, (regs->cs & 3) == 3, cr2, regs->rip,
                                 x86_trap_name(X86_TRAP_PF), regs->err);
  if (next)
    return next;

  fault_printf("\n[C%d] KERNEL PAGE FAULT: Access to 0x%lx\n", fault_cpu_id(),
               cr2);
  fault_printf("Frame: %p (IST fault stack)\n", (void *)regs);
  fault_printf("Error Code: 0x%lx (P:%d, W:%d, U:%d, R:%d, I:%d)\n", error_code,
               (error_code & 1) ? 1 : 0, (error_code & 2) ? 1 : 0,
               (error_code & 4) ? 1 : 0, (error_code & 8) ? 1 : 0,
               (error_code & 16) ? 1 : 0);

  amd64_dump_regs(regs);
  backtrace_regs(regs->rip, regs->rbp);
  backtrace_scan(regs->rsp);
  panic("Unrecoverable kernel #PF at RIP 0x%lx (CR2 0x%lx)", regs->rip, cr2);
}

/*
 * amd64_gpf_handler - handle X86_TRAP_GP (#GP).
 *
 * General Protection Fault: can be caused by descriptor/segment violations,
 * invalid IOPL instructions, or non-canonical addresses.
 */
static struct pt_regs *amd64_gpf_handler(struct pt_regs *regs) {
  struct pt_regs *next =
      fault_handle_user_or_panic(regs, (regs->cs & 3) == 3, 0, regs->rip,
                                 x86_trap_name(X86_TRAP_GP), regs->err);
  if (next)
    return next;

  fault_printf("\n[C%d] KERNEL GENERAL PROTECTION FAULT\n", fault_cpu_id());
  amd64_dump_regs(regs);
  backtrace_regs(regs->rip, regs->rbp);
  backtrace_scan(regs->rsp);
  panic("Unrecoverable kernel #GP at RIP 0x%lx (err 0x%lx)", regs->rip,
        regs->err);
}

/*
 * amd64_double_fault_handler - handle X86_TRAP_DF (#DF).
 *
 * Double fault occurs when an exception fires while handling another
 * exception. The CPU pushes a zero error code. Recovery is impossible in the
 * general case; this vector runs on its own dedicated IST stack
 * (IDT_IST_DOUBLE_FAULT) precisely so that reporting it never depends on the
 * stack that was already in trouble.
 */
static void amd64_double_fault_handler(struct pt_regs *regs) {
  fault_printf("\n[C%d] DOUBLE FAULT\n", fault_cpu_id());
  amd64_dump_regs(regs);
  backtrace_regs(regs->rip, regs->rbp);
  backtrace_scan(regs->rsp);
  arch_cpu_halt();
}

/* kernel_timer_tick: reached from the LOCAL_TIMER_VECTOR arm below.
 * kernel_syscall_dispatcher is reached only via syscall/sysret (syscall.S)
 * since the legacy int 0x80 gate was removed (SYS-AMD64-03 #168). */
extern struct pt_regs *kernel_timer_tick(struct pt_regs *regs);

/*
 * amd64_isr_dispatch - central exception and interrupt dispatcher.
 *
 * Called from common_isr_entry (isr_stubs.S) with RSP pointing at struct
 * pt_regs. Returns a (possibly different) pt_regs* — the common entry writes
 * the return value to RSP before restoring registers, allowing a context
 * switch by returning a different task's frame.
 *
 * Dispatch logic:
 *   vec < FIRST_EXTERNAL_VECTOR : CPU exceptions. Recursion guard
 *               (fault_enter), then switch on X86_TRAP_DF/GP/PF; every other
 *               vector routes through fault_handle_user_or_panic (user ->
 *               terminate, kernel -> panic).
 *   (vector 0x80 is a kernel-only gate now — the legacy int 0x80 syscall
 *    surface was removed, SYS-AMD64-03 #168; syscalls use syscall/sysret.)
 *   vec >= FIRST_EXTERNAL_VECTOR : Hardware IRQs. IRQ7_VECTOR/IRQ15_VECTOR
 *               spurious cases and SPURIOUS_APIC_VECTOR are filtered first;
 *               LOCAL_TIMER_VECTOR -> kernel_timer_tick; others ->
 *               irq_dispatch. All end through irq_chip_end() (chip-owned
 *               LAPIC + PIC EOI). NOTE(EXC-AMD64-03, resolved): the PIT is
 *               halted after LAPIC calibration, so LOCAL_TIMER_VECTOR has a
 *               single source (the LAPIC timer).
 *
 * Calling convention: called from assembly with a C ABI call; RDI = &pt_regs.
 * Returns RAX = new RSP (next task's pt_regs or the same regs on no-switch).
 *
 * The assembly entry has already saved the interrupted task's FXSAVE image;
 * its common return path restores the task selected here.
 */
struct pt_regs *amd64_isr_dispatch(struct pt_regs *regs) {
  uint64_t vec = regs->vec;

  if (vec < FIRST_EXTERNAL_VECTOR) {
    /* Fault recursion guard (Phase A step 7): a fault inside a fault handler
     * used to recurse on the same stack until #DF -> triple fault. Detect
     * the nesting FIRST — before any code that could itself fault — and stop
     * with one raw line. fault_exit() runs on every path below that resumes
     * execution; the halting paths deliberately keep the depth elevated. */
    if (fault_enter() > 1) {
      fault_printf("\n[C%d] NESTED CPU EXCEPTION vec=%lu (%s) err=0x%lx "
                   "rip=%016lx — halting\n",
                   fault_cpu_id(), vec, x86_trap_name(vec), regs->err,
                   regs->rip);
      arch_cpu_halt();
    }

    /* Handle exceptions (EXC-AMD64-02 resolved): user-attributable faults —
     * any vector 0-31 from CS RPL 3, e.g. #DE, #UD, #PF, #GP — terminate the
     * process via fault_handle_user_or_panic and return the next task's
     * frame. Kernel faults dump and panic. #DF never attempts recovery:
     * machine state is not trustworthy by definition. */
    switch (vec) {
    case X86_TRAP_DF:
      amd64_double_fault_handler(regs);
      break;
    case X86_TRAP_GP:
      return amd64_gpf_handler(regs);
    case X86_TRAP_PF:
      return amd64_page_fault_handler(regs);
    default: {
      struct pt_regs *next =
          fault_handle_user_or_panic(regs, (regs->cs & 3) == 3, 0, regs->rip,
                                     x86_trap_name(vec), regs->err);
      if (next)
        return next;
      fault_printf("\n[C%d] Unhandled kernel CPU Exception: %ld (%s)\n",
                   fault_cpu_id(), vec, x86_trap_name(vec));
      amd64_dump_regs(regs);
      backtrace_regs(regs->rip, regs->rbp);
      backtrace_scan(regs->rsp);
      panic("Unrecoverable kernel exception (vec %lu, %s) at RIP 0x%lx", vec,
            x86_trap_name(vec), regs->rip);
    }
    }
  } else {
    /* Hardware interrupts (FIRST_EXTERNAL_VECTOR-255). SYS-AMD64-03 (#168):
     * the legacy int 0x80 syscall surface was removed — vector 0x80 is now a
     * kernel-only gate, so a userland int 0x80 raises #GP (handled as a user
     * fault) and the only syscall entry is syscall/sysret via LSTAR. */
    struct pt_regs *ret_regs = regs;

    /* 8259 spurious IRQ7/IRQ15: not real interrupts — a level pulse
     * deasserted before INTA. Filtered BEFORE dispatch because their EOI
     * rules differ (none / master-only); pic_handle_spurious() performs
     * what is needed. Likewise the LAPIC spurious vector requires no EOI
     * and no dispatch. */
    if ((vec == IRQ7_VECTOR || vec == IRQ15_VECTOR) &&
        pic_handle_spurious((uint32_t)vec)) {
      return ret_regs;
    }
    if (vec == SPURIOUS_APIC_VECTOR) {
      return ret_regs;
    }

    if (vec == LOCAL_TIMER_VECTOR) {
      /* Tier-2 per-CPU drift accounting (docs/TIMER-MODEL.md §3): the
       * arch-neutral, HAL-driven timer_percpu_tick() advances the software
       * per-CPU schedule against the free-running TSC so lost ticks are
       * recovered per core. arch_timer_set_compare() is a no-op here (the
       * LAPIC stays periodic). Done BEFORE the tick so it reflects this
       * IRQ. */
      timer_percpu_tick(get_cpu_info());

      ret_regs = kernel_timer_tick(regs);
    } else {
      /* All other Hardware interrupts - route via generic system */
      pr_debug("AMD64: Hardware Interrupt Vector %lu triggered!\n", vec);
      extern struct pt_regs *irq_dispatch(uint32_t irq, struct pt_regs *regs);
      ret_regs = irq_dispatch((uint32_t)vec, regs);
    }

    /* End-of-interrupt through the chip (IRQ-01 fix): pic_chip_end() owns
     * the complete LAPIC + 8259 sequence; nothing here EOIs by hand. */
    irq_chip_end((uint32_t)vec);

    return ret_regs;
  }

  return regs;
}