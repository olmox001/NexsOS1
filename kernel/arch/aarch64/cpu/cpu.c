/*
 * kernel/arch/aarch64/cpu/cpu.c
 * AArch64 CPU initialisation, synchronous exception dispatch, and SMP support.
 *
 * Role:
 *   This file owns three distinct concerns for the AArch64 port:
 *   1. Per-CPU hardware initialisation (CPACR, NEON, VBAR installation).
 *   2. The C-level synchronous exception handler (sync_handler), which is the
 *      top of the dispatch chain entered from exception.S's vector_stub macro.
 *      It decodes ESR_EL1.EC and routes to the syscall handler, the memory-probe
 *      recovery path, or a per-EL fault printer / panic / process terminator.
 *   3. SMP helper routines: per-CPU kernel stack management, secondary-CPU PGD
 *      propagation, and the early MMU-enable sequence (arch_vmm_init_hw).
 *
 * Invariants:
 *   - sync_handler is called with IRQs already masked (DAIF.I/F set by hardware
 *     on exception entry); it may only re-enable IRQs under specific recovery
 *     paths (see is_kernel_user_access_fault branch).
 *   - arch_vmm_init_hw must be called exactly once on the primary CPU before any
 *     secondary CPU is woken; secondaries read secondary_ttbr0 after this point.
 *   - CPACR_EL1.FPEN is set to 0b11 on every CPU so that the full NEON register
 *     set can be saved/restored by exception.S (q0–q31, FPSR, FPCR).
 *
 * Known issues:
 *   ARCH-04 (W2 BAD-IMPL·BUG)  FDT memory parse often fails; the manual probe
 *            fallback in arch_platform_get_mem_regions (platform.c:62–96) then
 *            sets arch_mem_regions[0] twice — once at lines 83–86 and again at
 *            91–94 — the second write silently overrides the first. See
 *            platform.c comments for detail.
 *   ARCH-05 (W2 REFINE) The probe relies on sync_handler's probe_in_progress
 *            shortcut; a fault during probing of device memory windows may
 *            trigger undesirable side-effects before the handler fires.
 *   AMMU-08 (W2 BUG) arch_vmm_init_hw issues TLB flush only on the current CPU;
 *            no SMP TLB shootdown is performed when secondary CPUs arrive.
 *   CPU-AARCH64-01 (W2 REFINE) sync_handler: a wild kernel pointer that
 *            happens to fall in the user VA range is misclassified as a
 *            kernel-uaccess fault, leading to silent process termination instead
 *            of a kernel panic. [static]
 *   SYS-AARCH64-02 (W2 BAD-IMPL) The is_kernel_user_access_fault recovery
 *            path unconditionally releases current_process->mm_lock and calls
 *            local_irq_enable(), assuming exact lock discipline that is not
 *            documented or enforced. A future refactor of uaccess critical
 *            sections could silently violate this coupling.
 */
#include <kernel/printk.h>
#include <kernel/types.h>

/* Exception frame structure */
#include <kernel/cpu.h>
#include <kernel/sched.h>

#include <kernel/arch.h>
#include <kernel/vmm.h>

/* External definitions from core */
extern struct cpu_info cpu_data[MAX_CPUS];
extern uint32_t nr_cpus;

/* External functions from assembly */
extern void exception_vectors_install(void);

/*
 * arch_cpu_init - per-CPU hardware initialisation (called on every core).
 *
 * Actions performed on each CPU:
 *   1. Records cpu_id and marks the CPU online in cpu_data[].
 *   2. Increments nr_cpus atomically for secondaries (primary sets nr_cpus=1).
 *   3. Enables the FPU/SIMD (NEON) unit by setting CPACR_EL1.FPEN[21:20]=0b11,
 *      which allows EL0 and EL1 to execute floating-point and NEON instructions
 *      without trapping to a higher EL.  An ISB is issued to ensure the CPACR
 *      write takes effect before any subsequent NEON instruction.
 *   4. Installs the exception vector table (VBAR_EL1) via exception_vectors_install.
 *
 * Side effects: modifies CPACR_EL1, VBAR_EL1; prints boot diagnostics.
 * EL context: called at EL1 (MMU may or may not be active; called both pre- and
 *             post-MMU during primary and secondary CPU bring-up).
 */
void arch_cpu_init(void) {
  uint32_t id = arch_get_cpu_id();

  cpu_data[id].cpu_id = id;
  cpu_data[id].online = 1;

  if (id == 0) {
    nr_cpus = 1;
    pr_info("CPU: Primary core %u initialized\n", id);
  } else {
    /* Avoid pr_info here as it might cause lock contention with primary core
     * boot logs */
    __sync_fetch_and_add(&nr_cpus, 1);
  }

  /* Enable FPU/SIMD (NEON) - set CPACR_EL1.FPEN = 0b11.
   * FPEN[21:20] == 0b00 traps FP/NEON from EL0+EL1 to EL1.
   * FPEN[21:20] == 0b11 disables the trap entirely; needed so that
   * exception.S can save/restore q0-q31 without faulting. */
  uint64_t cpacr = arch_impl_get_cpacr();
  cpacr |= (3 << 20); /* FPEN bits [21:20] = 0b11 */
  arch_impl_set_cpacr(cpacr);
  arch_impl_mb();
  arch_impl_isb(); /* ensure CPACR change is visible before NEON use */

  /* Install exception vector table */
  exception_vectors_install();

  pr_info("CPU: Vector Table set to 0x%lx\n", arch_impl_get_vbar());
}

/*
 * probe_in_progress / probe_failed - volatile flags for safe memory probing.
 *
 * Set by the manual RAM discovery loop in platform.c (arch_platform_get_mem_regions)
 * when the FDT memory parse fails.  When probe_in_progress is true and a Data Abort
 * or Instruction Abort fires, sync_handler advances ELR_EL1 by 4 to skip the faulting
 * load and sets probe_failed so the caller knows the address is unmapped.
 *
 * Both flags must be volatile: they are written by C code and tested in the
 * exception handler on the same CPU; without volatile the compiler could optimise
 * away the test or hoist the write out of the loop.
 *
 * NOTE(ARCH-05): This mechanism works only because QEMU's virt machine delivers
 * aborts synchronously on an invalid load.  On real hardware with speculative
 * prefetch or device windows that do not abort cleanly, the probe may have
 * undesirable side-effects (e.g., triggering a device register read).
 */
volatile bool probe_in_progress = false;
volatile bool probe_failed = false;

/*
 * sync_handler - C-level synchronous exception handler called from exception.S.
 *
 * Parameters:
 *   frame  Pointer to the 816-byte exception frame pushed on the kernel stack by
 *          the vector_stub macro in exception.S.  The handler may modify frame->elr
 *          to change the return address (e.g., probe recovery), or swap in a
 *          different frame pointer entirely (e.g., schedule() returning a new task).
 *
 * Returns: pointer to the exception frame that should be restored on eret.
 *          Normally the same frame; after schedule() it is the new task's frame.
 *
 * EL context: entered at EL1 with IRQs masked (DAIF.I set by hardware).
 *
 * EC decoding (ESR_EL1[31:26]):
 *   0x00  Unknown / uncategorized — print and fall through to fault handler.
 *   0x15  SVC instruction from AArch64 EL0 — dispatch to syscall_handler().
 *   0x20  Instruction Abort from lower EL (EL0 code page fault).
 *   0x21  Instruction Abort from same EL (EL1 code fault — kernel bug).
 *   0x24  Data Abort from lower EL (EL0 load/store fault).
 *   0x25  Data Abort from same EL (EL1 load/store fault — kernel bug or probe).
 *   0x26  SP alignment fault.
 *   other  Unhandled — print and panic (kernel) or terminate (user).
 *
 * Probe recovery path (probe_in_progress == true):
 *   If a Data or Instruction Abort occurs while probe_in_progress is set,
 *   probe_failed is marked and ELR_EL1 is advanced by 4 to skip the faulting
 *   single instruction.  This is the only case where the handler returns early
 *   without the full fault-classification logic.
 *   NOTE(ARCH-05): See probe_in_progress declaration for caveats on real hardware.
 *
 * Fault classification (for all non-SVC ECs):
 *   is_user_fault       — SPSR.M[3:0] == 0b0000 (EL0t, i.e. exception came from EL0).
 *   is_kernel_user_access_fault — kernel was executing but FAR_EL1 points into user
 *                         VA range; implies an arch_copy_from/to_user access faulted.
 *
 *   NOTE(CPU-AARCH64-01): A wild kernel pointer coincidentally in user VA range is
 *   misclassified as is_kernel_user_access_fault and leads to process termination
 *   instead of a kernel panic.  [static]
 *
 * User / uaccess fault handling:
 *   The current process is terminated and schedule() is called to pick a new task.
 *   NOTE(SYS-AARCH64-02): If is_kernel_user_access_fault is true, this code assumes
 *   the faulting code held mm_lock and had IRQs disabled, and unconditionally calls
 *   spin_unlock + local_irq_enable before process_terminate.  This is fragile; any
 *   future change to the uaccess critical section discipline must update this path.
 *
 * Kernel fault handling: dumps all registers and calls panic().
 */
struct pt_regs *sync_handler(struct pt_regs *frame) {
  uint64_t esr, far, elr;
  uint32_t ec;

  if (!frame)
    return NULL;

  /* Read exception syndrome */
  esr = arch_get_fault_status(); /* ESR_EL1: exception syndrome register */
  far = arch_get_fault_address(); /* FAR_EL1: fault address register (valid for aborts) */
  elr = frame->elr; /* ELR_EL1 saved in frame by vector_stub */

  /* ESR_EL1[31:26]: Exception Class — identifies the exception type */
  ec = (esr >> 26) & 0x3F;

  /* Probe recovery: if a Data Abort (EC=0x24/0x25) or Instruction Abort
   * (EC=0x20/0x21) fires while a RAM probe is in progress, set probe_failed
   * and skip the faulting instruction by advancing ELR_EL1 by 4.
   * The probe load is always a single 4-byte instruction (ldr/ldur), so +4 is safe.
   * NOTE(ARCH-05): On real hardware with speculative loads this may not be
   * sufficient; a proper exception-table fixup (like Linux's extable) is safer. */
  if (probe_in_progress && (ec == 0x24 || ec == 0x25 || ec == 0x20 || ec == 0x21)) {
    probe_failed = true;
    /* Skip the faulting instruction (increment ELR by 4) */
    frame->elr += 4;
    return frame;
  }

  switch (ec) {
  case 0x00: /* Unknown exception — ESR does not encode a specific cause */
    pr_err("Unknown exception at 0x%016lx\n", elr);
    break;

  case 0x15: /* SVC instruction from AArch64 EL0 — syscall entry point */
    return syscall_handler(frame);

  case 0x20: /* Instruction abort from lower EL (EL0 code page not mapped) */
  case 0x21: /* Instruction abort from same EL (EL1 instruction fault — kernel bug) */
    pr_err("Instruction abort at 0x%016lx, FAR=0x%016lx\n", elr, far);
    break;

  case 0x24: /* Data abort from lower EL (EL0 load/store to unmapped/protected addr) */
  case 0x25: /* Data abort from same EL (EL1 load/store fault — kernel bug or probe) */
    pr_err("Data abort at 0x%016lx, FAR=0x%016lx\n", elr, far);
    break;

  case 0x26: /* SP alignment fault — SP not 16-byte aligned on exception entry */
    pr_err("SP alignment fault at 0x%016lx\n", elr);
    break;

  default:
    pr_err("Unhandled exception EC=0x%x at 0x%016lx\n", ec, elr);
    break;
  }

  if (ec != 0x15) {
    /* Determine the fault origin to choose between process termination and panic.
     *
     * is_user_fault: SPSR.M[3:0] encodes the EL and SP at the time of exception.
     *   0b0000 = EL0t (user space).  Any other value means EL1 (kernel).
     *   NOTE: bits [3:0] of SPSR_EL1 == 0x0 is the AArch64 EL0t encoding.
     *
     * is_kernel_user_access_fault: kernel was at EL1 but FAR points into user VA.
     *   This happens when arch_copy_from/to_user switches TTBR0 and a page fault
     *   fires before the copy completes.
     *   NOTE(CPU-AARCH64-01): A kernel NULL dereference or wild pointer that
     *   coincidentally maps to user VA range would be misclassified here. [static] */
    bool is_user_fault = ((frame->spsr & 0xF) == 0);
    bool is_kernel_user_access_fault = (current_process != NULL && vmm_is_user_addr(far));

    if (is_user_fault || is_kernel_user_access_fault) {
      pr_err("[ERROR] KERNEL-USER FAULT: EC=0x%lx (0x%lx) FAR=0x%lx ELR=0x%lx PID=%d\n",
             (uint64_t)ec, esr, far, elr, current_process->pid);
      pr_err("[DEBUG] Context: x0=0x%lx x1=0x%lx x2=0x%lx x3=0x%lx sp=0x%lx spsr=0x%lx\n",
             frame->regs[0], frame->regs[1], frame->regs[2], frame->regs[3],
             frame->sp_el0, frame->spsr);
      pr_err("Terminating PID %d\n", current_process->pid);
      
      if (elr == 0) {
        pr_err("CRITICAL: Process PID %d jumped to NULL (ELR=0).\n",
               current_process->pid);
      }

      /* If it was a kernel-user access fault, we are holding mm_lock and have IRQs disabled!
       * We MUST release them to avoid system deadlock.
       * NOTE(SYS-AARCH64-02): This assumes the uaccess critical section (syscall.c:
       * arch_copy_from/to_user) always holds exactly mm_lock with IRQs saved when a
       * fault fires.  Any refactor of that critical section must keep this coupling
       * in sync.  The current pairing is: syscall.c acquires IRQ-save then mm_lock;
       * we release mm_lock then IRQ-enable here (reverse order). [static]
       */
      if (is_kernel_user_access_fault) {
        spin_unlock(&current_process->mm_lock);
        local_irq_enable();
      }

      pr_err("Terminating PID %d\n", current_process->pid);

      process_terminate(current_process->pid);
      return schedule(frame);
    }


    pr_err("%s", "--- Kernel Exception Context Dump ---\n");
    pr_err("Process: PID %d\n",
           current_process ? (int)current_process->pid : -1);
    pr_err("SPSR_EL1: 0x%016lx\n", frame->spsr);
    pr_err("ELR_EL1:  0x%016lx\n", frame->elr);
    pr_err("FAR_EL1:  0x%016lx\n", far);
    pr_err("ESR_EL1:  0x%016lx\n", esr);
    pr_err("EC: 0x%x, ISS: 0x%x\n", ec, (uint32_t)(esr & 0xFFFFFF));

    if (elr == 0) {
        pr_err("%s", "CRITICAL: Kernel jumped to NULL! Check exception vector table and function pointers.\n");
        pr_err("Stack at 0x%lx:\n", (uint64_t)frame);
        for (int i = 0; i < 8; i++) {
            pr_err("  [%p] 0x%016lx\n", (void*)&((uint64_t*)frame)[i*2], ((uint64_t*)frame)[i*2]);
        }
    }

    for (int i = 0; i < 31; i += 2) {
      if (i + 1 < 31) {
        pr_err("X%02d: 0x%016lx  X%02d: 0x%016lx\n", i, frame->regs[i], i + 1,
               frame->regs[i + 1]);
      } else {
        pr_err("X%02d: 0x%016lx\n", i, frame->regs[i]);
      }
    }
    pr_err("SP_EL0:  0x%016lx\n", frame->sp_el0);
    pr_err("%s", "-----------------------------\n");
    panic("Unrecoverable kernel exception");
  }

  return frame;
}

/*
 * arch_secondary_stacks[] - per-CPU kernel stack top pointers for CPUs 0..MAX_CPUS-1.
 * Index i holds the TOP (highest address) of the 128KB kernel stack for CPU i,
 * because AArch64 stacks grow downward.
 * Populated by arch_smp_setup_stacks() before any secondary CPU is woken.
 */
void *arch_secondary_stacks[MAX_CPUS] = {0};
extern char __kernel_stack[]; /* BSS symbol; MAX_CPUS * 128KB array, see start.S */

/*
 * arch_smp_setup_stacks - allocate and record kernel stack tops for all CPUs.
 *
 * Parameters:
 *   cpu_count  Number of CPUs to prepare stacks for (clamped to MAX_CPUS).
 *
 * Strategy:
 *   CPUs 0..7 (indices 0-based) use a static BSS region (__kernel_stack) that is
 *   pre-allocated in the linker script as MAX_CPUS * 128KB.  The top of CPU i's
 *   slice is at &__kernel_stack[(i+1) * 131072] (131072 == 128 KB).
 *
 *   CPUs 8 and above receive a dynamically allocated 128KB region from the PMM.
 *   The pointer stored is the HIGH end (top) because stacks grow downward.
 *
 * Side effects: writes arch_secondary_stacks[]; allocates PMM pages for CPUs >= 8.
 * Called by: arch_smp_init() in platform.c before arch_cpu_wake_secondary().
 */
void arch_smp_setup_stacks(uint32_t cpu_count) {
    if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;

    for (uint32_t i = 0; i < cpu_count; i++) {
        if (i < 8) {
            /* First 8 cores use the static BSS stack area (512KB reserved).
             * Each 128KB slice: CPU i occupies [i*128KB .. (i+1)*128KB).
             * The stack pointer is set to the TOP of the slice (+1 past end). */
            arch_secondary_stacks[i] = (void *)&__kernel_stack[(uint64_t)(i + 1) * 131072];
        } else {
            /* Core 9+ needs dynamic allocation from PMM */
            void *ptr = pmm_alloc_pages(131072 / 4096);
            if (!ptr) {
                pr_err("CPU: OOM! Cannot allocate stack for core %u\n", i);
                break;
            }
            /* Stack grows down, return the TOP */
            arch_secondary_stacks[i] = (void *)((uintptr_t)ptr + 131072);
            pr_info("CPU: Core %u using dynamic stack at %p\n", i, arch_secondary_stacks[i]);
        }
    }
}

/*
 * arch_get_kernel_stack - return the kernel stack top for a given CPU.
 *
 * Parameters:
 *   cpu_id  Zero-based CPU index.
 * Returns: pointer to the top (highest address) of the kernel stack, or NULL
 *          if cpu_id >= MAX_CPUS.
 *
 * Prefers the arch_secondary_stacks[] entry if it has been set up (non-NULL).
 * Falls back to the BSS array formula for CPUs not yet set up (bootstrap path).
 */
void *arch_get_kernel_stack(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return NULL;

    /* If dynamic stacks are set up, use them */
    if (arch_secondary_stacks[cpu_id]) return arch_secondary_stacks[cpu_id];

    /* Bootstrap fallback: same formula as arch_smp_setup_stacks for CPUs < 8 */
    return (void *)&__kernel_stack[(uint64_t)(cpu_id + 1) * 131072];
}

/*
 * arch_vmm_set_secondary_pgd - publish the primary CPU's PGD to secondary CPUs.
 *
 * Parameters:
 *   pgd  Physical address of the kernel L0 page table to share.
 *
 * secondary_ttbr0 (defined in start.S .data) is read by secondary_startup in
 * start.S before the secondary CPU has its own MMU enabled.  Writing the value
 * and then cache-cleaning it (arch_cache_clean_range) ensures the physical
 * cache line is written back to DRAM so that the secondary CPU's uncached read
 * sees the correct value.
 *
 * NOTE(AMMU-08): No DSB/SMP shootdown is issued here beyond the cache clean +
 * arch_mb(); secondary CPUs must perform their own ISB after loading TTBR0_EL1
 * (done in secondary_startup, start.S:163-183). [static]
 */
extern uint64_t secondary_ttbr0;
void arch_vmm_set_secondary_pgd(uint64_t pgd) {
    secondary_ttbr0 = pgd;
    arch_cache_clean_range(&secondary_ttbr0, sizeof(secondary_ttbr0));
    arch_mb();
}

/*
 * arch_vmm_init_hw - program MAIR, TCR, TTBR0, SCTLR and enable the MMU.
 *
 * Parameters:
 *   kernel_pgd  Physical address of the L0 (PGD) page table for the kernel.
 *               Built by arch_vmm_map_range() / arch_vmm_create_process_pgd()
 *               in mmu.c before this function is called.
 *
 * Sequence (ARM64 Reference Manual, D5-2 required order):
 *   1. MAIR_EL1  — memory attribute palette used by page-table attribute indices.
 *      Attr0 (index 0) = 0xFF = Normal Write-Back/Write-Allocate (cacheable).
 *      Attr1 (index 1) = 0x04 = Device nGnRE (non-gathering, non-reordering,
 *                                early-acknowledgement; used for MMIO).
 *
 *   2. TCR_EL1   — translation control (VA size, granule, cacheability).
 *      T0SZ  [5:0]  = 16 → TTBR0 covers 2^(64-16) = 48-bit VA.
 *      IRGN0 [9:8]  = 0b01 → inner write-back/write-allocate (cacheable walks).
 *      ORGN0 [11:10]= 0b01 → outer write-back/write-allocate.
 *      SH0   [13:12]= 0b11 → inner-shareable page table walks.
 *      TG0   [15:14]= 0b00 → 4KB granule for TTBR0.
 *      EPD1  [23]   = 1    → disable TTBR1 translation (no higher-half split yet).
 *      IPS   [34:32]= 0    → 32-bit PA (sufficient for QEMU virt at 0x40000000;
 *                            limits PA to 4GB, undersized for real hardware).
 *
 *   3. TTBR0_EL1 — install the kernel PGD; TLB flushed before and after.
 *
 *   4. SCTLR_EL1 — enable MMU (M=1), D-cache (C=1), I-cache (I=1).
 *      RES1 bits [29,28,23,22,20,11] are set as required by the architecture.
 *      Alignment check (A, bit 1) and stack alignment check (SA, bit 3) are
 *      disabled for compatibility with existing kernel code that does not
 *      guarantee 16-byte aligned stack access on every path.
 *
 * Side effects: MAIR_EL1, TCR_EL1, TTBR0_EL1, SCTLR_EL1 are written; TLB is
 *               flushed; MMU is enabled by the end of this function.
 *               After return, all accesses go through the installed page tables.
 *
 * NOTE(AMMU-01): mmu.c maps RAM with PAGE_KERNEL which sets PTE_UXN|PTE_PXN
 *   only for device pages; normal RAM is executable (no W^X enforcement).
 * NOTE(AMMU-08): This function runs on the primary CPU only; secondaries set up
 *   their own MAIR/TCR/SCTLR in secondary_startup (start.S:165-183). [static]
 */
void arch_vmm_init_hw(uint64_t kernel_pgd) {
  pr_info("AArch64 VMM: Setting up MAIR (PGD at 0x%lx)\n", kernel_pgd);

  /* Ensure all previous writes are visible before touching system registers */
  arch_impl_mb();
  arch_impl_isb();

  /* 1. Setup MAIR_EL1 (Memory Attribute Indirection Register).
   * Attr0 [7:0]  = 0xFF = Normal WB/WA Inner+Outer (used for RAM pages).
   * Attr1 [15:8] = 0x04 = Device nGnRE (used for MMIO pages).
   * All other attribute indices are left as zero (unused). */
  uint64_t mair = (0xFFUL << 0) | (0x04UL << 8);
  arch_impl_set_mair(mair);
  arch_impl_isb(); /* barrier required before TTBR/TCR changes */

  pr_info("%s", "AArch64 VMM: Setting up TCR\n");
  /* 2. Setup TCR_EL1 (Translation Control Register).
   * Bit breakdown:
   *   [5:0]  T0SZ = 16  → 48-bit VA space under TTBR0 (covers [0, 2^48)).
   *   [9:8]  IRGN0 = 1  → inner WB/WA cache for page table walks.
   *   [11:10]ORGN0 = 1  → outer WB/WA cache for page table walks.
   *   [13:12]SH0   = 3  → inner-shareable domain for page table walks.
   *   [15:14]TG0   = 0  → 4KB translation granule.
   *   [23]   EPD1  = 1  → disable TTBR1 (no separate kernel higher-half).
   *   [34:32]IPS   = 0  → 32-bit physical address space (4GB max).
   * The (0UL << 32) term is a no-op; left for readability against the IPS field. */
  uint64_t tcr = (16UL << 0) | (3UL << 12) | (1UL << 10) | (1UL << 8) | (0UL << 32) | (0UL << 14) | (1UL << 23);
  arch_impl_set_tcr(tcr);
  arch_impl_isb();

  pr_info("%s", "AArch64 VMM: Setting TTBR0\n");
  /* 3. Set TTBR0_EL1: flush TLB first (vmalle1) to eliminate stale entries,
   * write the new PGD physical address, then fence with DSB+ISB so subsequent
   * instruction fetches see the new translation. */
  arch_impl_tlb_flush_local();
  arch_vmm_set_pgd(kernel_pgd);
  arch_impl_mb();
  arch_impl_isb();

  pr_info("%s", "AArch64 VMM: Enabling SCTLR bits (MMU, Caches)\n");
  /* 4. Enable MMU in SCTLR_EL1.
   * Read-modify-write to preserve any existing configuration bits. */
  uint64_t sctlr = arch_impl_get_sctlr();

  /* Mandatory RES1 bits for EL1 SCTLR (ARM DDI 0487 D5.2.88):
   * [29] LSMAOE, [28] nTLSMD, [23] SPAN, [22] EIS, [20] TSCXT, [11] EOS.
   * These must be 1 for correct operation; writing 0 is UNPREDICTABLE. */
  sctlr |= (1UL << 29) | (1UL << 28) | (1UL << 23) | (1UL << 22) | (1UL << 20) | (1UL << 11);

  /* Enable MMU (M=bit0), Instruction Cache (I=bit12), Data Cache (C=bit2).
   * Enabling all three at once; the page tables must be correct before this. */
  sctlr |= (1UL << 0) | (1UL << 12) | (1UL << 2);

  /* Disable Alignment Check (A=bit1) and Stack Alignment Check (SA=bit3).
   * Leaving these enabled would require all kernel stack frames and loads to
   * be naturally aligned; existing code does not guarantee this. */
  sctlr &= ~((1UL << 1) | (1UL << 3));

  arch_impl_mb();
  arch_impl_set_sctlr(sctlr); /* MMU enable; instruction stream continues via TLB */
  arch_impl_mb();
  arch_impl_isb(); /* pipeline synchronisation after MMU enable */

  pr_info("AArch64 VMM: MMU Enabled. SCTLR=0x%lx\n", sctlr);
}

/*
 * arch_vmm_map_mmio - identity-map the QEMU virt MMIO window into a PGD.
 *
 * Parameters:
 *   pgd  Pointer to the L0 page table (physical address assumed == virtual for
 *        identity-mapped kernel bootstrap context).
 *
 * Maps [0x08000000 .. 0x0A800000) (40 MB) with PAGE_DEVICE attributes
 * (Device nGnRE via MAIR index 1, non-cacheable, strongly ordered).
 * This window covers the standard QEMU virt board device layout:
 *   0x08000000 GIC distributor / redistributor
 *   0x09000000 PL011 UART
 *   0x0A000000 VirtIO MMIO devices (VIRTIO_MMIO_BASE, 32 slots * 0x200)
 *
 * Called from kernel_main during early MMU setup for both the primary kernel
 * PGD and each process PGD (via arch_vmm_create_process_pgd -> arch_vmm_map_mmio).
 */
void arch_vmm_map_mmio(uint64_t *pgd) {
  /* Identity Map MMIO (UART, GIC, VirtIO) */
  /* 0x08000000 to 0x0A800000 covers typical QEMU virt devices */
  arch_vmm_map_range((uint64_t)pgd, 0x08000000UL, 0x08000000UL,
                     0x02800000UL, PAGE_DEVICE);
}

/*
 * arch_cpu_switch_context - perform the architecture-specific address-space switch.
 *
 * Parameters:
 *   next  Process descriptor for the task being scheduled in.
 *
 * Writes TTBR0_EL1 with the physical address of next->page_table (the process's
 * L0 PGD).  The caller (schedule / ctx_switch) is responsible for saving and
 * restoring the general-purpose register context (via ctx_switch in context.S).
 *
 * A kernel thread (e.g. idle) has no private address space (page_table == NULL):
 * switch it onto the shared kernel_pgd instead of leaving the PREVIOUS process's
 * PGD active in TTBR0_EL1.
 *
 * SCHED-UAF-01 (interactive-close residual, aarch64 HAL): the whole aarch64
 * kernel runs from TTBR0 only (EPD1=1, no TTBR1 higher-half — see
 * arch_vmm_init_hw).  If a CPU keeps a terminated process's PGD in TTBR0 —
 * because it switched prev->idle and we skipped the reload — then when that
 * process is reaped and its PGD pages are freed, the CPU is left executing on a
 * freed PGD.  Its identity map (including printk) vanishes and the next fetch
 * faults, mirroring the amd64 triple-fault-on-window-close failure.  Load the
 * shared kernel_pgd for NULL page_table to keep the kernel mapping live.
 *
 * This is now the SINGLE source of truth for the scheduler address-space switch
 * (the redundant hal_vmm_set_pgd block in schedule() was removed), so the TLB
 * flush + DSB/ISB that the scheduler block used to issue are performed here:
 * arch_vmm_set_pgd is a bare "msr ttbr0_el1" (arch.h) with no implicit barriers.
 */
void arch_cpu_switch_context(struct process *next) {
    extern uint64_t *kernel_pgd;
    /* kernel_pgd is identity-mapped (virt == phys), so its pointer value IS the
     * physical TTBR0 base; next->page_table is likewise passed as a PA, matching
     * the existing arch_vmm_set_pgd callers in this file. */
    uint64_t pgd = next->page_table ? (uint64_t)next->page_table
                                    : (uint64_t)(uintptr_t)kernel_pgd;
    if (pgd) {
        arch_vmm_set_pgd(pgd);
        arch_tlb_flush_all();
        arch_isb();
    }
}
