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
#include <kernel/fault.h>
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

  /* ARCH-AMD64-APPGD-01 (amd64 HAL only): the AP trampoline brings each AP up on
   * the stale boot_pml4 (32-bit CR3), which lacks the >4GB device MMIO at
   * PML4[1] (e.g. the virtio-input ISR window at 0xc000005000).  Once the live
   * kernel_pgd exists (built by vmm_dynamic_remap + arch_vmm_map_device before
   * SMP bringup), adopt it here so a device IRQ taken on this CPU cannot
   * page-fault on the high MMIO and halt the core.  On the BSP's early
   * cpu_init() call kernel_pgd is still NULL (VMM not up yet) -> no-op; the BSP
   * already runs on kernel_pgd via vmm_dynamic_remap().  Confined to the amd64
   * HAL; platform.c is untouched. */
  {
    extern uint64_t *kernel_pgd;
    if (kernel_pgd)
      arch_vmm_set_pgd(virt_to_phys(kernel_pgd)); /* CR3 takes the PA */
  }
}

/*
 * get_cpu_info - return the per-CPU struct for the calling CPU.
 *
 * Reads the LAPIC ID (arch_get_cpu_id) and indexes cpu_data[].  Falls back to
 * cpu_data[0] if the ID is out of range (paranoia guard for early boot).
 *
 * Side effects: none.  May be called from interrupt context.
 * NOTE: arch_get_cpu_id reads LAPIC MMIO — NOT safe from a fault handler on a
 * compromised address space; fault paths use arch_cpu_info_fault_safe below.
 */
struct cpu_info *get_cpu_info(void) {
  uint32_t id = arch_get_cpu_id();
  if (id >= MAX_CPUS) return &cpu_data[0];
  return &cpu_data[id];
}

/*
 * arch_cpu_info_fault_safe - per-CPU info with no faultable dependencies
 * (kernel/fault.h, Phase A step 5).
 *
 * The LAPIC-MMIO read in arch_get_cpu_id needs a live page-table mapping —
 * exactly what a freed-PGD fault destroys.  Instead read the IA32_GS_BASE MSR
 * (rdmsr: register-only, cannot fault) and validate it against the cpu_data[]
 * bounds BEFORE any dereference.  This also covers the two GS hazards:
 *   - user GS base (fault inside the syscall-entry window before swapgs):
 *     fails the bounds check -> NULL, no wild dereference;
 *   - pre-cpu_init MSR garbage: same.
 */
struct cpu_info *arch_cpu_info_fault_safe(void) {
  uint32_t lo, hi;
  __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101u));
  uintptr_t p = ((uintptr_t)hi << 32) | lo;
  uintptr_t base = (uintptr_t)&cpu_data[0];
  if (p < base || p >= base + sizeof(cpu_data))
    return NULL;
  if ((p - base) % sizeof(struct cpu_info) != 0)
    return NULL;
  return (struct cpu_info *)p;
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
 *   CR3:               loaded with next->page_table (physical PA of PML4), or
 *                      with the shared kernel_pgd when page_table is NULL
 *                      (kernel thread — SCHED-UAF-01: never leave the previous
 *                      process's possibly-freed PGD active).
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

  /* Switch address space.  A kernel thread (e.g. idle) has no private address
   * space (page_table == NULL): switch it onto the shared kernel_pgd instead of
   * leaving the PREVIOUS process's PGD active in CR3.
   *
   * SCHED-UAF-01 (interactive-close residual): if a CPU keeps a terminated
   * process's PGD as CR3 — because it switched prev->idle and we skipped the
   * reload — then when that process is reaped and its PGD pages are freed, the
   * CPU is left executing on a freed PGD.  Its kernel low-half identity map
   * (which includes printk) vanishes, so the next kernel fetch #PFs; the amd64
   * #PF handler then calls printk, which #PFs again -> recursive fault ->
   * stack overflow -> #DF -> triple fault.  That is the "Terminating process
   * ... PID" -> instant reboot seen on window close.  Confined to the amd64 HAL. */
  {
    extern uint64_t *kernel_pgd;
    /* page_table / kernel_pgd are kernel virtual pointers; CR3 takes the
     * PHYSICAL PML4 base — translate with virt_to_phys. */
    uint64_t pgd = next->page_table ? virt_to_phys(next->page_table)
                                    : (kernel_pgd ? virt_to_phys(kernel_pgd) : 0);
    if (pgd)
      arch_vmm_set_pgd(pgd);
  }

  /* Update TSS RSP0 for interrupt stack switching */
  gdt_set_rsp0(next->kernel_stack);
}
