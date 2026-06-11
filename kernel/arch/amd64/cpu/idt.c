/*
 * kernel/arch/amd64/cpu/idt.c
 * Interrupt Descriptor Table (IDT) and Exception Dispatcher for x86-64
 *
 * Responsibilities:
 *   - Define the 256-entry 64-bit IDT (struct idt_entry, 16 bytes each).
 *   - Install all 256 ISR stubs (from isr_stub_table[] in isr_stubs.S) as
 *     kernel-only interrupt gates (DPL=0, type=0x8E) except vector 0x80
 *     which is a user-callable trap gate (DPL=3, type=0xEE).
 *   - Dispatch CPU exceptions (vectors 0-31), legacy syscalls (0x80), and
 *     hardware IRQs (32-255) from the single C entry point amd64_isr_dispatch.
 *   - Acknowledge the LAPIC and legacy 8259 PIC after hardware IRQs.
 *
 * Invariants:
 *   - The IDT is a single shared array; all CPUs load the same base address.
 *     Only CPU 0 fills the table (guarded by idt_initialized).  APs spin-wait.
 *   - Every hardware IRQ (vec 32-255) ends with lapic_eoi(); legacy PIC range
 *     (32-47) additionally calls pic_send_eoi() to deassert the 8259 output.
 *
 * Known issues:
 *   EXC-AMD64-01 (W2 MISSING/STUB) probe_in_progress / probe_failed are
 *     declared here but the x86 RIP-advance logic is unimplemented; the block
 *     falls through to the exception handler which halts.  The aarch64 path
 *     correctly advances ELR_EL1 (:cpu.c:75-79).  Dead code on amd64.
 *   EXC-AMD64-02 (W3 MISSING) No user-vs-kernel discrimination for exception
 *     vectors 0-31 (except 8/13/14).  A divide-by-zero in a user ELF causes
 *     the kernel to halt rather than terminating the process.  Fix: check
 *     regs->cs & 3; if Ring 3, call process_terminate + schedule.
 *   EXC-AMD64-04 (W2 BUG) idt_initialized is a plain static int with no
 *     volatile qualifier and no memory barrier.  APs spinning on
 *     while (!idt_initialized) may cache a stale zero indefinitely; a release
 *     barrier on the write side and a load-acquire on the read side are needed.
 *   SYS-AMD64-03 (W2 REFINE) int 0x80 gate (DPL=3) installs a second syscall
 *     surface alongside the LSTAR fast path.  If the ABI goal is SYSCALL-only,
 *     this gate should be removed or explicitly documented.
 *   CPU-AMD64-01 (W3 MISSING) common_isr_entry in isr_stubs.S saves only 15
 *     GP registers; no FXSAVE of XMM state.  See isr_stubs.S:95-109 and the
 *     cross-ref in cpu.c Known issues.
 */
#include <kernel/types.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <kernel/cpu.h>
#include <kernel/fault.h>
#include <arch/pt_regs.h>
#include <arch/arch.h>
#include <arch/amd64_internal.h>
#include <kernel/arch.h>

#define IDT_ENTRIES 256

/*
 * struct idt_entry - x86-64 64-bit interrupt/trap gate descriptor (16 bytes).
 *
 * Fields per Intel SDM Vol.3 Table 3-2:
 *   offset_lo  [15:0]  : bits 0-15 of handler RIP
 *   selector   [15:0]  : code segment selector (0x08 = kernel CS)
 *   ist        [2:0]   : Interrupt Stack Table index (0 = none)
 *   type_attr  [7:0]   : gate type (0x8E=interrupt, 0xEE=trap DPL3) + present
 *   offset_mid [15:0]  : bits 16-31 of handler RIP
 *   offset_hi  [31:0]  : bits 32-63 of handler RIP
 *   zero       [31:0]  : reserved, must be 0
 *
 * type_attr encoding:
 *   0x8E = P=1 DPL=00 S=0 Type=1110 (64-bit interrupt gate, Ring 0 only)
 *   0xEE = P=1 DPL=11 S=0 Type=1110 (64-bit interrupt gate, callable from Ring 3)
 */
struct idt_entry {
  uint16_t offset_lo;
  uint16_t selector;
  uint8_t  ist;       /* Interrupt Stack Table offset */
  uint8_t  type_attr; /* Type and Attributes */
  uint16_t offset_mid;
  uint32_t offset_hi;
  uint32_t zero;
} __packed;

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
/* static struct idtr idtr; - Removed, using local idtr in idt_init */

/* Defined in isr_stubs.S — 256-entry array of isr_stub_N addresses. */
extern uint64_t isr_stub_table[];

/*
 * idt_set_gate - write one IDT descriptor.
 *
 * num:   vector index 0-255
 * base:  64-bit handler address (isr_stub_N from isr_stub_table[])
 * sel:   code segment selector (0x08 = GDT_KERN_CODE)
 * flags: type_attr byte (0x8E = kernel interrupt gate; 0xEE = user trap gate)
 * ist:   IST stack index (0 = use the current RSP/TSS RSP0)
 *
 * The 64-bit offset is split across offset_lo/mid/hi as required by the
 * x86-64 descriptor format.
 */
static void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist) {
  idt[num].offset_lo  = (uint16_t)(base & 0xFFFF);
  idt[num].selector   = sel;
  idt[num].ist        = ist;
  idt[num].type_attr  = flags;
  idt[num].offset_mid = (uint16_t)((base >> 16) & 0xFFFF);
  idt[num].offset_hi  = (uint32_t)(base >> 32);
  idt[num].zero       = 0;
}

/* idt_initialized: flag set by CPU 0 after filling the IDT; APs spin on it.
 * NOTE(EXC-AMD64-04): This is a plain int with no volatile or memory barrier.
 * On SMP the compiler and CPU are free to cache the zero in a register; the
 * spin loop on APs may never observe the write.  A proper release/acquire pair
 * (e.g. __atomic_store_n + __atomic_load_n) is needed for correctness. */
static int idt_initialized = 0;

/*
 * idt_init - initialise and load the IDT for the calling CPU.
 *
 * CPU 0 path: fills all 256 entries with DPL-0 interrupt gates, then
 * overwrites vector 0x80 with a DPL-3 trap gate for the legacy int 0x80
 * syscall path.  Sets idt_initialized = 1 when done.
 * NOTE(SYS-AMD64-03): The int 0x80 gate (DPL=3) provides a second syscall
 * surface in addition to the LSTAR fast path.  If the ABI goal is
 * SYSCALL-only this gate should be removed.
 *
 * AP path: spins on idt_initialized (NOTE EXC-AMD64-04: no memory barrier),
 * then executes lidt with a CPU-local idtr pointing at the shared idt[].
 * Each CPU must execute lidt itself; the IDTR is per-CPU.
 */
void idt_init(void) {
  if (arch_get_cpu_id() == 0) {
      memset(&idt, 0, sizeof(struct idt_entry) * IDT_ENTRIES);

      /* Set up IDT gates: all 256 as kernel-only interrupt gates (0x8E) */
      for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E, 0);
      }

      /* Syscall via int 0x80: override with DPL=3 trap gate (0xEE) so user
       * space can invoke vector 0x80 without triggering a GPF.
       * NOTE(SYS-AMD64-03): This is an additional (legacy) syscall surface. */
      idt_set_gate(0x80, isr_stub_table[0x80], 0x08, 0xEE, 0);

      /* Dedicated IST fault stacks (Phase A step 3; stacks live in gdt.c):
       *   IST1 -> #GP (13) and #PF (14): the handler always runs on a fresh,
       *           always-mapped stack even if the faulting RSP is wild or the
       *           task kernel stack has overflowed.
       *   IST2 -> #DF (8): separate index so that even a #PF/#GP storm that
       *           clobbers IST1 cannot take down double-fault reporting.
       * NMI (2) deliberately stays IST=0: an IST NMI needs the paranoid
       * swapgs-entry pattern, out of scope here (see Phase A plan, Risks). */
      idt_set_gate(8,  isr_stub_table[8],  0x08, 0x8E, 2);
      idt_set_gate(13, isr_stub_table[13], 0x08, 0x8E, 1);
      idt_set_gate(14, isr_stub_table[14], 0x08, 0x8E, 1);

      idt_initialized = 1;
  }

  /* Wait for CPU 0 to finish if needed (not strictly necessary with sequential boot) */
  while (!idt_initialized) arch_nop(); /* NOTE(EXC-AMD64-04): missing barrier */

  struct idtr local_idtr;
  local_idtr.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
  local_idtr.base  = (uint64_t)&idt;

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
  fault_printf("RIP: %016lx CS: %02lx RFLAGS: %016lx\n", regs->rip, regs->cs, regs->rflags);
  fault_printf("RAX: %016lx RBX: %016lx RCX: %016lx RDX: %016lx\n", regs->rax, regs->rbx, regs->rcx, regs->rdx);
  fault_printf("RSI: %016lx RDI: %016lx RBP: %016lx RSP: %016lx\n", regs->rsi, regs->rdi, regs->rbp, regs->rsp);
  fault_printf("R8:  %016lx R9:  %016lx R10: %016lx R11: %016lx\n", regs->r8, regs->r9, regs->r10, regs->r11);
  fault_printf("R12: %016lx R13: %016lx R14: %016lx R15: %016lx\n", regs->r12, regs->r13, regs->r14, regs->r15);
  fault_printf("Vector: %ld, Error Code: %lx\n", regs->vec, regs->err);
}

/*
 * amd64_page_fault_handler - handle vector 14 (#PF).
 *
 * CR2 holds the faulting linear address.  Error code bits (Intel SDM Vol.3
 * Section 6.15):
 *   bit 0  P   : 0 = not-present, 1 = protection violation
 *   bit 1  W/R : 0 = read, 1 = write
 *   bit 2  U/S : 0 = kernel, 1 = user
 *   bit 3  RSVD: reserved-bit violation
 *   bit 4  I/D : instruction fetch (NX violation)
 *
 * NOTE(EXC-AMD64-02): The U/S bit (err & 4) is printed but never branched on.
 * A user-space page fault causes the kernel to halt rather than delivering a
 * signal to the user process.  Fix: if (regs->cs & 3) == 3, call
 * process_terminate(current_process) + schedule() instead of halting.
 */
static void amd64_page_fault_handler(struct pt_regs *regs) {
  uint64_t cr2;
  __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));

  uint64_t error_code = regs->err;

  fault_printf("\n[C%d] PAGE FAULT: Access to 0x%lx\n", fault_cpu_id(), cr2);
  fault_printf("Frame: %p (IST1 fault stack)\n", (void *)regs);
  fault_printf("Error Code: 0x%lx (P:%d, W:%d, U:%d, R:%d, I:%d)\n",
               error_code,
               (error_code & 1) ? 1 : 0,
               (error_code & 2) ? 1 : 0,
               (error_code & 4) ? 1 : 0,
               (error_code & 8) ? 1 : 0,
               (error_code & 16) ? 1 : 0);

  amd64_dump_regs(regs);
  arch_cpu_halt();
}

/*
 * amd64_gpf_handler - handle vector 13 (#GP).
 *
 * General Protection Fault: can be caused by descriptor/segment violations,
 * invalid IOPL instructions, or non-canonical addresses.
 * NOTE(EXC-AMD64-02): No user-vs-kernel discrimination; both halt the kernel.
 */
static void amd64_gpf_handler(struct pt_regs *regs) {
  fault_printf("\n[C%d] GENERAL PROTECTION FAULT\n", fault_cpu_id());
  amd64_dump_regs(regs);
  arch_cpu_halt();
}

/*
 * amd64_double_fault_handler - handle vector 8 (#DF).
 *
 * Double fault occurs when an exception fires while handling another exception.
 * The CPU pushes a zero error code.  Recovery is impossible in the general case;
 * a dedicated IST stack entry should be used for #DF to avoid stack-overflow
 * recursion (IST=0 here, so it shares the TSS RSP0 stack — risky).
 */
static void amd64_double_fault_handler(struct pt_regs *regs) {
  fault_printf("\n[C%d] DOUBLE FAULT\n", fault_cpu_id());
  amd64_dump_regs(regs);
  arch_cpu_halt();
}

extern struct pt_regs *kernel_syscall_dispatcher(struct pt_regs *regs);
extern struct pt_regs *kernel_timer_tick(struct pt_regs *regs);

/* probe_in_progress / probe_failed — flags for safe memory probing.
 * NOTE(EXC-AMD64-01): probe_in_progress is never set to true on amd64
 * (only aarch64/platform.c:63 sets it).  The probe-recovery block below
 * (vec==14||vec==13 while probe_in_progress) is therefore unreachable dead
 * code.  Furthermore even if reached, no RIP adjustment is performed — the
 * code falls through to the normal exception handler and halts.  The aarch64
 * counterpart correctly increments ELR_EL1 by 4 to resume after the probe.
 * Fix: implement a fixup table or fixed-size probe stub so RIP can be advanced
 * by a known constant, then return regs instead of halting. */
volatile bool probe_in_progress = false;
volatile bool probe_failed = false;

/*
 * amd64_isr_dispatch - central exception and interrupt dispatcher.
 *
 * Called from common_isr_entry (isr_stubs.S) with RSP pointing at struct
 * pt_regs.  Returns a (possibly different) pt_regs* — the common entry writes
 * the return value to RSP before restoring registers, allowing a context
 * switch by returning a different task's frame.
 *
 * Dispatch logic:
 *   vec < 32  : CPU exceptions.  Probe-recovery check (NOTE EXC-AMD64-01:
 *               dead on amd64), then switch on vec 8/13/14; all other vectors
 *               print and halt.  NOTE(EXC-AMD64-02): No user-vs-kernel split —
 *               user exceptions also halt the kernel.
 *   vec == 0x80: Legacy int 0x80 syscall → kernel_syscall_dispatcher.
 *   vec 32-255: Hardware IRQs.  vec==32 (timer) → kernel_timer_tick; others →
 *               irq_dispatch.  All end with lapic_eoi(); vectors 32-47 also
 *               call pic_send_eoi() to satisfy the legacy 8259 PIC.
 *               NOTE(EXC-AMD64-03): LAPIC LINT0 is configured as ExtINT in
 *               apic.c:22, so legacy PIC IRQ 0 (vec==32) may fire simultaneously
 *               with the LAPIC periodic timer at vec==32 → double timer tick.
 *
 * Calling convention: called from assembly with a C ABI call; RDI = &pt_regs.
 * Returns RAX = new RSP (next task's pt_regs or the same regs on no-switch).
 *
 * NOTE(CPU-AMD64-01): No FXSAVE in common_isr_entry; XMM state is not
 * preserved across a preemptive switch triggered by the timer IRQ (vec==32).
 */
struct pt_regs *amd64_isr_dispatch(struct pt_regs *regs) {
  uint64_t vec = regs->vec;

  if (vec < 32) {
    /* Fault recursion guard (Phase A step 7): a fault inside a fault handler
     * used to recurse on the same stack until #DF -> triple fault.  Detect
     * the nesting FIRST — before any code that could itself fault — and stop
     * with one raw line.  fault_exit() runs on every path below that resumes
     * execution; the halting paths deliberately keep the depth elevated. */
    if (fault_enter() > 1) {
      fault_printf("\n[C%d] NESTED CPU EXCEPTION vec=%lu err=0x%lx rip=%016lx — halting\n",
                   fault_cpu_id(), vec, regs->err, regs->rip);
      arch_cpu_halt();
    }

    /* Check if this is a fault during a safe probe.
     * NOTE(EXC-AMD64-01): probe_in_progress is never set on amd64; this
     * entire block is unreachable dead code.  The intended RIP fixup is
     * also unimplemented — falls through to the normal handler below. */
    if (probe_in_progress && (vec == 14 || vec == 13)) {
      probe_failed = true;
      /* On x86, we need to adjust RIP to skip the faulting instruction.
       * Most move instructions involved in the probe are 3-7 bytes.
       * This is tricky without a full disassembler, but we can assume
       * the probe uses a specific move format or we can use a simpler approach.
       *
       * Actually, a better way for x86 is to use a specific assembly probe function
       * that we know the length of.
       */

      /* For now, let's try to handle it in platform.c with a setjmp-like mechanism
       * or just by adjusting RIP if we know it's a 3-byte move (e.g. mov [rdx], rax)
       * In platform.c: *ptr = 0x55AA... is typically 10 bytes (movabsq + mov)
       */

      /* Let's assume the faulting instruction is a memory access.
       * We'll use a safer approach in platform.c.
       */
    }

    /* Handle exceptions.
     * NOTE(EXC-AMD64-02): The default branch halts the kernel for ANY unhandled
     * vector including those caused by user-space code (divide-by-zero, invalid
     * opcode, etc.).  Should check regs->cs & 3 to discriminate. */
    switch (vec) {
      case 8: /* Double Fault (#DF) */
        amd64_double_fault_handler(regs);
        break;
      case 13: /* General Protection Fault (#GP) */
        amd64_gpf_handler(regs);
        break;
      case 14: /* Page Fault (#PF) — CR2 holds faulting address */
        amd64_page_fault_handler(regs);
        break;
      default:
        fault_printf("\n[C%d] Unhandled CPU Exception: %ld\n", fault_cpu_id(), vec);
        amd64_dump_regs(regs);
        arch_cpu_halt();
        break;
    }

    /* Currently unreachable (every vec<32 path halts); kept so that when a
     * recovery path is added (user-fault termination, probe fixup) the
     * recursion depth is correctly unwound. */
    fault_exit();
  } else if (vec == 0x80) {
    /* Legacy syscall via int 0x80 (DPL=3 gate installed in idt_init).
     * NOTE(SYS-AMD64-03): second syscall surface alongside LSTAR. */
    return kernel_syscall_dispatcher(regs);
  } else {
    /* Hardware interrupts (32-255) */
    struct pt_regs *ret_regs = regs;

    if (vec == 32) {
        /* Timer Interrupt (PIT or LAPIC periodic, vector 32).
         * NOTE(EXC-AMD64-03): If LAPIC LINT0 (ExtINT) and the LAPIC periodic
         * timer both fire at vec==32, kernel_timer_tick runs twice per interval.
         * NOTE(CPU-AMD64-01): No FPU save; ctx_switch on this path risks XMM
         * corruption between concurrently running kernel tasks. */
        ret_regs = kernel_timer_tick(regs);
    } else {
        /* All other Hardware interrupts - route via generic system */
        if (vec != 32) {
            pr_debug("AMD64: Hardware Interrupt Vector %lu triggered!\n", vec);
        }
        extern struct pt_regs *irq_dispatch(uint32_t irq, struct pt_regs * regs);
        ret_regs = irq_dispatch(vec, regs);
    }

    /* Acknowledge LAPIC for all HW interrupts.
     * Writing 0 to the EOI register signals end-of-interrupt. */
    lapic_eoi();

    /* Also acknowledge legacy PIC for its range (32-47) if active.
     * Vectors 32-47 correspond to 8259 IRQs 0-15; the PIC requires its own
     * EOI sequence in addition to the LAPIC EOI. */
    if (vec >= 32 && vec < 48) {
        pic_send_eoi(vec - 32);
    }

    return ret_regs;
  }

  return regs;
}
