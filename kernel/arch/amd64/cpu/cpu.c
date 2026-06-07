/*
 * kernel/arch/amd64/cpu/cpu.c
 * AMD64 Per-CPU Initialization and Context-Switch Support
 *
 * Responsibilities:
 *   - Enable SSE/FXSAVE in CR0/CR4 (required early; GCC may auto-vectorise
 *     even kernel code with XMM registers).
 *   - Populate cpu_data[id] (struct cpu_info) and write both IA32_GS_BASE
 *     and IA32_KERNEL_GS_BASE so that swapgs gives the kernel per-CPU pointer.
 *   - Sequence per-CPU GDT, IDT, SYSCALL MSR, and LAPIC init calls.
 *   - Implement arch_cpu_switch_context: update per-CPU bookkeeping, switch
 *     CR3 to the next process's PML4, and update TSS RSP0 so interrupt delivery
 *     uses the new task's kernel stack.
 *
 * Invariants:
 *   - Identity-map: PA == VA throughout the kernel.  All pointer casts from
 *     physical addresses are safe.
 *   - GS base: after arch_cpu_init returns, %gs points at cpu_data[id]; all
 *     assembly stubs (isr_stubs.S, syscall.S) rely on %gs:16 = stack_top and
 *     %gs:24 = user_stack_tmp matching struct cpu_info field offsets exactly.
 *     (Verified static: cpu.h:13-19 gives self=0, cpu_id=8, online=12,
 *     stack_top=16, user_stack_tmp=24 — matching syscall.S lines 41-43.)
 *
 * Known issues:
 *   CPU-AMD64-01 (W3 MISSING) isr_stubs.S common_isr_entry saves only the 15
 *     GP registers.  No FXSAVE/XSAVE of XMM/AVX state is performed.  This
 *     file enables SSE (CR4.OSFXSR, :36-39), so the compiler may use XMM regs
 *     in kernel code.  A preemptive context switch on a timer IRQ between two
 *     kernel tasks can silently corrupt their XMM state.  Cross-ref:
 *     isr_stubs.S:95-109, context.S:12-36.
 *   UACC-AMD64-01 (W2 DOC/MISSING) CR4.SMAP is NOT set here (:36-39); only
 *     OSFXSR and OSXMMEXCPT are enabled.  uaccess.c's header claims SMAP is
 *     active — that is wrong.  See uaccess.c Known issues.
 */
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/vmm.h>
#include <arch/amd64_internal.h>
#include <arch/amd64/apic.h>

#include <cpuid.h>

extern struct cpu_info cpu_data[MAX_CPUS];
extern uint32_t nr_cpus;

extern void gdt_init(void);
extern void idt_init(void);
extern void amd64_syscall_init(void);


/*
 * arch_cpu_init - per-CPU initialization entry point.
 *
 * Called by the BSP on the boot path (via kernel_main) and by each APs
 * secondary_cpu_entry in start.S.  Sequence must match:
 *   SSE → GDT → GS base → IDT → SYSCALL MSRs → LAPIC
 *
 * SSE is enabled FIRST because C code called after this point (including
 * the GDT/IDT helpers) may be compiled with -msse or auto-vectorised.
 * NOTE(CPU-AMD64-01): FXSAVE/XSAVE is not used in ctx_switch or
 * common_isr_entry; enabling SSE here creates the hazard without the
 * save/restore infrastructure.
 *
 * GS base is written AFTER gdt_init() because lgdt wipes all segment
 * selectors including GS; writing IA32_GS_BASE before lgdt would be lost.
 * Both IA32_GS_BASE (0xC0000101) and IA32_KERNEL_GS_BASE (0xC0000102) are
 * set to cpu_info_ptr so that swapgs in isr/syscall stubs switches from
 * user GS to the correct kernel per-CPU pointer.
 *
 * nr_cpus: BSP sets unconditionally to 1 (no RMW needed); APs use
 * __sync_fetch_and_add to safely increment the shared counter.
 */
void arch_cpu_init(void) {
  uint32_t id = arch_get_cpu_id();

  if (id >= MAX_CPUS) {
    /* Fallback for early printk if LAPIC ID is weird */
    id = 0;
  }

  cpu_data[id].cpu_id = id;

  /* Enable SSE EARLY (Functions like memset might use XMM registers) */
  uint64_t cr0, cr4;
  __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(1 << 2); /* Clear EM (Emulation) */
  cr0 |= (1 << 1);  /* Set MP (Monitor co-processor) */
  __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0));

  /* NOTE(CPU-AMD64-01): OSFXSR+OSXMMEXCPT enable fxsave/fxrstor and unmasked
   * SIMD FP exceptions, but no FXSAVE is inserted in ctx_switch or the ISR
   * entry path.  CR4.SMAP is intentionally NOT set here — uaccess.c's claim
   * of SMAP protection is therefore inaccurate (UACC-AMD64-01). */
  __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= (1 << 9);  /* OSFXSR (OS support for fxsave/fxrstor) */
  cr4 |= (1 << 10); /* OSXMMEXCPT (OS support for unmasked simd fp exceptions) */
  __asm__ __volatile__("mov %0, %%cr4" :: "r"(cr4));

  struct cpu_info *cpu = &cpu_data[id];
  cpu->self = cpu;
  cpu->online = 1;
  spin_lock_init(&cpu->sched_lock);

  pr_info("CPU: Initializing GDT...\n");
  gdt_init();

  /* Set up GS base for per-CPU data access (AFTER GDT to avoid wipeout) */
  uint64_t cpu_info_ptr = (uint64_t)cpu;
  wrmsr(0xC0000101, cpu_info_ptr); /* IA32_GS_BASE */
  wrmsr(0xC0000102, cpu_info_ptr); /* IA32_KERNEL_GS_BASE */

  pr_info("CPU: Initializing IDT...\n");
  idt_init();

  pr_info("CPU: Initializing Syscall...\n");
  amd64_syscall_init();

  pr_info("CPU: Initializing LAPIC...\n");
  lapic_init();

  if (id == 0) {
      nr_cpus = 1;
  } else {
      __sync_fetch_and_add(&nr_cpus, 1);
  }

  pr_info("AMD64 CPU %u initialized (GDT, IDT, Syscall, SSE, GS enabled)\n", id);
}

/*
 * get_cpu_info - return the per-CPU struct for the calling CPU.
 *
 * Reads the LAPIC ID (arch_get_cpu_id) and indexes cpu_data[].  Falls back to
 * cpu_data[0] if the ID is out of range (paranoia guard for early boot).
 *
 * Side effects: none.  May be called from interrupt context.
 */
struct cpu_info *get_cpu_info(void) {
  uint32_t id = arch_get_cpu_id();
  if (id >= MAX_CPUS) return &cpu_data[0];
  return &cpu_data[id];
}

/*
 * arch_cpu_switch_context - switch the hardware context to run 'next'.
 *
 * Called by the scheduler before invoking ctx_switch (context.S) with the
 * two ctx_amd64 save-area pointers.  This function updates the per-CPU
 * bookkeeping that the assembly stubs depend on:
 *
 *   cpu->stack_top:    read by syscall_entry (%gs:16) to load the kernel RSP
 *                      on the SYSCALL fast path.
 *   cpu->current_task: used by the generic scheduler and kernel_syscall_dispatcher
 *                      to find current_process.
 *   CR3:               loaded with next->page_table (physical PA of PML4) to
 *                      switch the address space.  Skipped if page_table is NULL
 *                      (kernel thread with no private address space).
 *   TSS RSP0:          updated via gdt_set_rsp0 so that hardware interrupt
 *                      delivery from Ring 3 uses the correct kernel stack.
 *
 * Params:
 *   next - the process to switch to; must not be NULL.
 *
 * NOTE(CPU-AMD64-01): No FPU/XMM state save/restore is done here.  This is the
 * cooperative switch path; the Intel ABI treats XMM regs as caller-saved, so
 * a cooperative switch is safe.  The hazard is preemptive switches via the
 * timer ISR in common_isr_entry, which does not save/restore XMM state.
 */
void arch_cpu_switch_context(struct process *next) {
  struct cpu_info *cpu = get_cpu_info();

  /* Update per-CPU data for assembly stubs (syscall.S / isr_stubs.S) */
  cpu->stack_top = next->kernel_stack;
  cpu->current_task = next;

  /* Switch address space */
  if (next->page_table) {
    arch_vmm_set_pgd((uint64_t)next->page_table);
  }

  /* Update TSS RSP0 for interrupt stack switching */
  gdt_set_rsp0(next->kernel_stack);
}
