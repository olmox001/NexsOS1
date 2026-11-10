/*
 * kernel/HAL/arch/amd64/cpu/cpu.c
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
#include <hal/fault_protocol.h>
#include <hal/diagnostic_protocol.h>
#include <hal/core_protocol.h>
#include <arch/pt_regs.h>
#include <hal/core.h>
#include <hal/cpu_protocol.h>
#include <hal/provider.h>
#include <kernel/string.h>
#include <arch/arch.h>
#include <arch/amd64_internal.h>
#include <arch/amd64/apic.h>
#include <stddef.h>
#include <stdint.h>

struct nxhal_amd64_cpu_local {
  struct nxhal_amd64_cpu_local *self;
  uint32_t cpu_id;
  uint32_t online;
  uint64_t stack_top;
  uint64_t user_stack_tmp;
};

_Static_assert(offsetof(struct nxhal_amd64_cpu_local, stack_top) == 16,
               "amd64 GS stack_top offset must remain 16");
_Static_assert(offsetof(struct nxhal_amd64_cpu_local, user_stack_tmp) == 24,
               "amd64 GS user_stack_tmp offset must remain 24");

static struct nxhal_amd64_cpu_local amd64_cpu_local[NXHAL_CPU_LIMIT];
/* PCID-tagged TLB support (perf §3, DIR-06): 1 once CPUID has confirmed PCID +
 * INVPCID and CR4.PCIDE has been enabled.  Read by the arch.h flush primitives
 * and arch_cpu_switch_context; 0 (default) keeps the legacy flush-on-switch
 * behaviour so the build stays portable on CPUs without PCID. */
int amd64_pcid_enabled = 0;

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
void nxhal_provider_cpu_local_init(void) {
  uint32_t id = hal_cpu_id();

  if (id >= NXHAL_CPU_LIMIT) {
    /* Fallback for early printk if LAPIC ID is weird */
    id = 0;
  }

  /* Enable SSE EARLY (Functions like memset might use XMM registers) */
  uint64_t cr0 = arch_isa_cr0();
  uint64_t cr4 = 0U;
  cr0 &= ~(1 << 2); /* Clear EM (Emulation) */
  cr0 |= (1 << 1);  /* Set MP (Monitor co-processor) */
  arch_isa_write_cr0(cr0);

  /* NOTE(CPU-AMD64-01): OSFXSR+OSXMMEXCPT enable fxsave/fxrstor and unmasked
   * SIMD FP exceptions, but no FXSAVE is inserted in ctx_switch or the ISR
   * entry path.  CR4.SMAP is intentionally NOT set here — uaccess.c's claim
   * of SMAP protection is therefore inaccurate (UACC-AMD64-01). */
  cr4 = arch_isa_cr4();
  cr4 |= (1 << 9);  /* OSFXSR (OS support for fxsave/fxrstor) */
  cr4 |= (1 << 10); /* OSXMMEXCPT (OS support for unmasked simd fp exceptions) */
  arch_isa_write_cr4(cr4);

  /* PCID-tagged TLB (perf §3, DIR-06): enable CR4.PCIDE per-CPU iff the CPU
   * supports PCID (CPUID.01h:ECX[17]).  INVPCID is NOT required — the teardown
   * all-PCID flush uses a CR4.PGE toggle instead (see amd64_flush_all_pcids in
   * arch.h), so PCID activates on the common CPUs/hypervisors that expose PCID
   * but not INVPCID (e.g. HVF here).  CR4.PCIDE may only be set while
   * CR3[11:0]==0, which holds here (boot CR3 is page-aligned).  Portable: a CPU
   * without PCID keeps flush-on-switch.  Runs on every CPU (BSP + APs), so each
   * enables its own CR4.PCIDE before it ever runs the scheduler; the global flag
   * is published for the fast paths. */
  {
    uint32_t eax = 0U, ebx = 0U, ecx = 0U, edx = 0U;
    uint32_t max_basic = 0U;
    int has_pcid = 0, has_invpcid = 0;

    arch_isa_cpuid(0U, 0U, &max_basic, &ebx, &ecx, &edx);
    if (max_basic >= 1U) {
      arch_isa_cpuid(1U, 0U, &eax, &ebx, &ecx, &edx);
      has_pcid = (int)((ecx >> 17U) & 1U);
    }
    if (max_basic >= 7U) {
      arch_isa_cpuid(7U, 0U, &eax, &ebx, &ecx, &edx);
      has_invpcid = (int)((ebx >> 10U) & 1U);
    }
    if (has_pcid) {
      uint64_t cr4now = arch_isa_cr4();
      cr4now |= (1UL << 17); /* CR4.PCIDE */
      arch_isa_write_cr4(cr4now);
      amd64_pcid_enabled = 1;
    }
    if (id == 0) /* one-shot boot diagnostic (not a hot path) */
      nxhal_diag_info("CPU: PCID-tagged TLB %s (pcid=%d invpcid=%d)\n",
              amd64_pcid_enabled ? "ENABLED" : "disabled (flush-on-switch)",
              has_pcid, has_invpcid);
  }

  struct nxhal_amd64_cpu_local *cpu = &amd64_cpu_local[id];
  cpu->self = cpu;
  cpu->cpu_id = id;
  cpu->online = 1U;
  if (nxhal_cpu_online(id) != 0)
    nxhal_fault_panic("AMD64 CPU %u cannot publish online state", id);

  nxhal_diag_info("CPU: Initializing GDT...\n");
  gdt_init();

  /* Set up GS base for per-CPU data access (AFTER GDT to avoid wipeout) */
  uint64_t cpu_info_ptr = (uint64_t)cpu;
  wrmsr(0xC0000101, cpu_info_ptr); /* IA32_GS_BASE */
  wrmsr(0xC0000102, cpu_info_ptr); /* IA32_KERNEL_GS_BASE */

  nxhal_diag_info("CPU: Initializing IDT...\n");
  idt_init();

  nxhal_diag_info("CPU: Initializing Syscall...\n");
  amd64_syscall_init();

  nxhal_diag_info("CPU: Initializing LAPIC...\n");
  lapic_init();

  nxhal_diag_info("AMD64 CPU %u initialized (GDT, IDT, Syscall, SSE, GS enabled)\n", id);

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
    uint64_t kernel_root = hal_vmm_get_kernel_pgd();
    if (kernel_root != 0U)
      hal_vmm_set_pgd(kernel_root);
  }
}

int32_t nxhal_provider_cpu_id_fault_safe(void) {
  uintptr_t p = (uintptr_t)arch_isa_rdmsr(UINT32_C(0xC0000101));
  uintptr_t base = (uintptr_t)&amd64_cpu_local[0];

  if (p < base || p >= base + sizeof(amd64_cpu_local))
    return -1;
  if ((p - base) % sizeof(struct nxhal_amd64_cpu_local) != 0U)
    return -1;
  return (int32_t)((p - base) / sizeof(struct nxhal_amd64_cpu_local));
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
 *   cpu->current_task: used by the generic scheduler and nxhal_core_syscall
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
void nxhal_provider_cpu_switch_context(
    const struct nxhal_cpu_context_switch *context) {
  uint32_t id = hal_cpu_id();
  struct nxhal_amd64_cpu_local *cpu;

  if (context == NULL)
    hal_cpu_halt();
  if (id >= NXHAL_CPU_LIMIT)
    id = 0U;
  cpu = &amd64_cpu_local[id];

  /* Update per-CPU data for assembly stubs (syscall.S / isr_stubs.S) */
  cpu->stack_top = context->kernel_stack_top;

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
    uint64_t pgd = (context->flags & NXHAL_CPU_CONTEXT_HAS_USER_ROOT)
                       ? context->user_root_phys
                       : hal_vmm_get_kernel_pgd();
    if (pgd != 0U) {
      if (amd64_pcid_enabled) {
        uint64_t pcid =
            (context->flags & NXHAL_CPU_CONTEXT_HAS_USER_ROOT)
                ? (uint64_t)context->asid
                : 0U;
        hal_vmm_set_pgd(pgd | pcid | (UINT64_C(1) << 63));
      } else {
        hal_vmm_set_pgd(pgd);
      }
    }
  }

  /* Update TSS RSP0 for interrupt stack switching */
  gdt_set_rsp0(context->kernel_stack_top);
}
