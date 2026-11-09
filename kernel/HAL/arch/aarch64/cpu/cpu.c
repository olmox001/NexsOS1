/*
 * kernel/HAL/arch/aarch64/cpu/cpu.c
 * AArch64 CPU initialisation, synchronous exception dispatch, and SMP support.
 *
 * Role:
 *   This file owns three distinct concerns for the AArch64 port:
 *   1. Per-CPU hardware initialisation (CPACR, NEON, VBAR installation).
 *   2. The C-level synchronous exception handler (sync_handler), which is the
 *      top of the dispatch chain entered from exception.S's vector_stub macro.
 *      It decodes ESR_EL1.EC and routes to the syscall handler, the
 * memory-probe recovery path, or a per-EL fault printer / panic / process
 * terminator.
 *   3. SMP helper routines: per-CPU kernel stack management, secondary-CPU PGD
 *      propagation, and the early MMU-enable sequence (arch_vmm_init_hw).
 *
 * Invariants:
 *   - sync_handler is called with IRQs already masked (DAIF.I/F set by hardware
 *     on exception entry); it may only re-enable IRQs under specific recovery
 *     paths (see is_kernel_user_access_fault branch).
 *   - arch_vmm_init_hw must be called exactly once on the primary CPU before
 * any secondary CPU is woken; secondaries read secondary_ttbr0 after this
 * point.
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
 *            kernel-uaccess fault, leading to silent process termination
 * instead of a kernel panic. [static] SYS-AARCH64-02 (W2 BAD-IMPL) The
 * is_kernel_user_access_fault recovery path unconditionally releases
 * current_process->mm_lock and calls local_irq_enable(), assuming exact lock
 * discipline that is not documented or enforced. A future refactor of uaccess
 * critical sections could silently violate this coupling.
 */
#include <hal/diagnostic_protocol.h>
#include <hal/fault_protocol.h>
#include <hal/core_protocol.h>
#include <arch/pt_regs.h>
#include <arch/arch.h>
#include <hal/core.h>
#include <hal/memory_protocol.h>
#include <hal/cpu_protocol.h>
#include <hal/provider.h>
#include <kernel/string.h>
#include <kernel/types.h>

/* Exception frame structure */
#include <kernel/fault.h>
#include <kernel/sched.h>

#include <kernel/arch.h>
#include <kernel/memlayout.h>
#include <kernel/platform.h>
#include <kernel/vmm.h>

/* External definitions from core */
/*
 * Per-CPU EL1 fault (abort) stacks — Phase A step 4.
 *
 * aarch64 has no IST: an EL1-from-EL1 sync abort or SError re-uses SP_EL1,
 * so a kernel-stack overflow or a fault with a wild SP recursed until silent
 * death.  handle_el1_spx_sync/serror (exception.S) switch onto
 * arch_fault_stack_top[cpu] before building the 816-byte frame; the original
 * SP is parked in the 16-byte slot directly above the frame so the C handler
 * can both report it and copy the frame back for resuming paths (probe fixup).
 *
 * 16KB per CPU sized for: sync_handler + classification + fault_printf dump +
 * the backtrace walk, with margin.  In .bss → identity-mapped in kernel_pgd.
 */
#define FAULT_STACK_SIZE 16384
static uint8_t fault_stacks[NXHAL_CPU_LIMIT][FAULT_STACK_SIZE]
    __attribute__((aligned(16)));
uint64_t arch_fault_stack_top[NXHAL_CPU_LIMIT]; /* read by exception.S vectors */

/*
 * arch_frame_on_fault_stack - is this exception frame on a per-CPU fault stack?
 * Used by sync_handler to know whether the parked-SP slot at frame+816 exists
 * and whether a resuming path must copy the frame back to the original stack.
 */
int arch_frame_on_fault_stack(const void *frame) {
  uintptr_t p = (uintptr_t)frame;
  uintptr_t base = (uintptr_t)fault_stacks;
  return p >= base && p < base + sizeof(fault_stacks);
}

/*
 * arch_cpu_info_fault_safe - per-CPU info with no faultable dependencies
 * (kernel/fault.h, Phase A step 5).
 *
 * On aarch64 the CPU id comes from MPIDR_EL1 — a system-register read that
 * cannot fault — so this is simply the bounds-checked cpu_data[] lookup.
 */
int32_t nxhal_provider_cpu_id_fault_safe(void) {
  uint32_t id = arch_impl_get_cpu_id();
  if (id >= NXHAL_CPU_LIMIT)
    return -1;
  return (int32_t)id;
}

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
 *   4. Installs the exception vector table (VBAR_EL1) via
 * exception_vectors_install.
 *
 * Side effects: modifies CPACR_EL1, VBAR_EL1; prints boot diagnostics.
 * EL context: called at EL1 (MMU may or may not be active; called both pre- and
 *             post-MMU during primary and secondary CPU bring-up).
 */
void nxhal_provider_cpu_local_init(void) {
  uint32_t id = hal_cpu_id();

  if (id >= NXHAL_CPU_LIMIT)
    nxhal_diag_panic("AArch64 CPU id %u exceeds NXHAL_CPU_LIMIT", id);
  if (nxhal_cpu_online(id) != 0)
    nxhal_diag_panic("AArch64 CPU %u cannot publish online state", id);

  /* Publish this CPU's EL1 fault-stack top BEFORE installing the vectors:
   * from the first VBAR exception onward, handle_el1_spx_sync/serror switch
   * onto it (a zero entry makes the vector stay on the current stack). */
  arch_fault_stack_top[id] = (uint64_t)&fault_stacks[id][FAULT_STACK_SIZE];

  if (id == 0)
    nxhal_diag_info("CPU: Primary core %u initialized\n", id);

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

  nxhal_diag_info("CPU: Vector Table set to 0x%lx\n", arch_impl_get_vbar());
}

/*
 * probe_in_progress / probe_failed - volatile flags for safe memory probing.
 *
 * Set by the manual RAM discovery loop in platform.c
 * (arch_platform_get_mem_regions) when the FDT memory parse fails.  When
 * probe_in_progress is true and a Data Abort or Instruction Abort fires,
 * sync_handler advances ELR_EL1 by 4 to skip the faulting load and sets
 * probe_failed so the caller knows the address is unmapped.
 *
 * Both flags must be volatile: they are written by C code and tested in the
 * exception handler on the same CPU; without volatile the compiler could
 * optimise away the test or hoist the write out of the loop.
 *
 * NOTE(ARCH-05): This mechanism works only because QEMU's virt machine delivers
 * aborts synchronously on an invalid load.  On real hardware with speculative
 * prefetch or device windows that do not abort cleanly, the probe may have
 * undesirable side-effects (e.g., triggering a device register read).
 */
volatile bool probe_in_progress = false;
volatile bool probe_failed = false;

extern char aarch64_probe_read32_instruction[];
extern char aarch64_probe_read32_fixup[];

/*
 * arch_platform_probe_read32 - read one physical word through the early direct
 * map, recovering only from a Data Abort at the exact LDR instruction.
 *
 * Callers are early-boot, primary-CPU code with IRQs masked. This is not a
 * general MMIO accessor; the caller must restrict probes to a platform-defined
 * RAM window. The exact-PC fixup prevents unrelated faults from being hidden.
 */
__attribute__((noinline))
int arch_platform_probe_read32(uint64_t physical, uint32_t *value) {
  const uint64_t phys_limit = HAL_PHYS_MAP_LIMIT;
  if (!value || (physical & 3ULL) != 0 ||
      physical < HAL_RAM_START ||
      physical > phys_limit - sizeof(uint32_t) || probe_in_progress)
    return -1;

  uint32_t read_value = 0;
  uintptr_t virtual_address = (uintptr_t)phys_to_virt(physical);
  probe_failed = false;
  probe_in_progress = true;
  __asm__ volatile("" ::: "memory");
  __asm__ volatile(
      ".global aarch64_probe_read32_instruction\n"
      "aarch64_probe_read32_instruction:\n"
      "ldr %w0, [%1]\n"
      ".global aarch64_probe_read32_fixup\n"
      "aarch64_probe_read32_fixup:\n"
      : "=&r"(read_value)
      : "r"(virtual_address)
      : "memory");
  __asm__ volatile("" ::: "memory");
  probe_in_progress = false;

  if (probe_failed)
    return -1;
  *value = read_value;
  return 0;
}

/*
 * sync_handler - C-level synchronous exception handler called from exception.S.
 *
 * Parameters:
 *   frame  Pointer to the 816-byte exception frame pushed on the kernel stack
 * by the vector_stub macro in exception.S.  The handler may modify frame->elr
 *          to change the return address (e.g., probe recovery), or swap in a
 *          different frame pointer entirely (e.g., nxhal_core_schedule() returning a new
 * task).
 *
 * Returns: pointer to the exception frame that should be restored on eret.
 *          Normally the same frame; after nxhal_core_schedule() it is the new task's
 * frame.
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
 *   If a Data Abort occurs at the exact probe LDR while probe_in_progress is
 *   set, probe_failed is marked and ELR_EL1 is moved to the LDR fixup label
 *   single instruction.  This is the only case where the handler returns early
 *   without the full fault-classification logic.
 *   NOTE(ARCH-05): See probe_in_progress declaration for caveats on real
 * hardware.
 *
 * Fault classification (for all non-SVC ECs):
 *   is_user_fault       — SPSR.M[3:0] == 0b0000 (EL0t, i.e. exception came from
 * EL0). is_kernel_user_access_fault — kernel was executing but FAR_EL1 points
 * into user VA range; implies an arch_copy_from/to_user access faulted.
 *
 *   NOTE(CPU-AARCH64-01): A wild kernel pointer coincidentally in user VA range
 * is misclassified as is_kernel_user_access_fault and leads to process
 * termination instead of a kernel panic.  [static]
 *
 * User / uaccess fault handling:
 *   The current process is terminated and nxhal_core_schedule() is called to pick a new
 * task. NOTE(SYS-AARCH64-02): If is_kernel_user_access_fault is true, this code
 * assumes the faulting code held mm_lock and had IRQs disabled, and
 * unconditionally calls spin_unlock + local_irq_enable before
 * process_terminate.  This is fragile; any future change to the uaccess
 * critical section discipline must update this path.
 *
 * Kernel fault handling: dumps all registers and calls nxhal_diag_panic().
 */
struct pt_regs *sync_handler(struct pt_regs *frame) {
  uint64_t esr, far, elr;
  uint32_t ec;

  if (!frame)
    return NULL;

  /* Read exception syndrome */
  esr = hal_fault_status();  /* ESR_EL1: exception syndrome register */
  far = hal_fault_address(); /* FAR_EL1: fault address register (valid for
                                     aborts) */
  elr = frame->elr;               /* ELR_EL1 saved in frame by vector_stub */

  /* ESR_EL1[31:26]: Exception Class — identifies the exception type */
  ec = (esr >> 26) & 0x3F;

  /* SVC from EL0 is a syscall, not a fault: bypass the recursion guard and
   * every fault-path primitive entirely. */
  if (ec == 0x15)
    return syscall_handler(frame);

  /* Fault recursion guard (Phase A step 7): an abort inside this handler used
   * to recurse on the kernel stack until silent death.  Detect nesting FIRST
   * — before anything that could itself abort — and stop with one raw line.
   * nxhal_fault_exit() runs on every resuming path below; the panic path keeps the
   * depth elevated so nxhal_diag_panic() selects its fault-safe output mode. */
  if (nxhal_fault_enter() > 1) {
    nxhal_diag_fault("\n[FATAL] NESTED EL1 EXCEPTION EC=0x%x ELR=%016lx FAR=%016lx "
                 "— halting\n",
                 ec, elr, far);
    hal_cpu_halt();
  }

  /* Recover only the exact physical-probe LDR. Do not hide an unrelated
   * kernel Data Abort merely because a probe flag is momentarily set. */
  if (probe_in_progress &&
      (ec == 0x24 || ec == 0x25) &&
      elr == (uint64_t)(uintptr_t)aarch64_probe_read32_instruction) {
    probe_failed = true;
    frame->elr = (uint64_t)(uintptr_t)aarch64_probe_read32_fixup;
    nxhal_fault_exit();
    /* If the vector switched us onto the per-CPU fault stack, the eret
     * epilogue computes the final SP as frame+816 — which would resume the
     * probe loop ON the fault stack.  Rebuild the frame just below the
     * original SP (free space: stacks grow down) and return that instead,
     * so execution resumes with SP exactly as it was at the abort. */
    if (arch_frame_on_fault_stack(frame)) {
      uint64_t old_sp =
          *(uint64_t *)((char *)frame + 816) + 16; /* +16: scratch push */
      struct pt_regs *orig = (struct pt_regs *)(old_sp - 816);
      memcpy(orig, frame, sizeof(*frame));
      return orig;
    }
    return frame;
  }

  /* One-line classification banner.  fault_printf (not printk): the address
   * space and lock state are unknown at this point for EL1-origin faults. */
  switch (ec) {
  case 0x00: /* Unknown exception — ESR does not encode a specific cause */
    nxhal_diag_fault("Unknown exception at 0x%016lx\n", elr);
    break;

  case 0x20: /* Instruction abort from lower EL (EL0 code page not mapped) */
  case 0x21: /* Instruction abort from same EL (EL1 instruction fault — kernel
                bug) */
    nxhal_diag_fault("Instruction abort at 0x%016lx, FAR=0x%016lx\n", elr, far);
    break;

  case 0x24: /* Data abort from lower EL (EL0 load/store to unmapped/protected
                addr) */
  case 0x25: /* Data abort from same EL (EL1 load/store fault — kernel bug or
                probe) */
    nxhal_diag_fault("Data abort at 0x%016lx, FAR=0x%016lx\n", elr, far);
    break;

  case 0x26: /* SP alignment fault — SP not 16-byte aligned on exception entry
              */
    nxhal_diag_fault("SP alignment fault at 0x%016lx\n", elr);
    break;

  default:
    nxhal_diag_fault("Unhandled exception EC=0x%x at 0x%016lx\n", ec, elr);
    break;
  }

  {
    /* User-vs-kernel decision (Phase A steps 8/10): delegated to the generic
     * helper.  is_user_fault = SPSR.M[3:0] == 0b0000 (EL0t).  The old
     * "FAR in user VA implies a uaccess fault" heuristic (CPU-AARCH64-01) is
     * gone: the helper recovers ONLY when the per-CPU uaccess_active flag —
     * set exclusively inside arch_copy_{from,to,string_from}_user — marks the
     * dereference window; the mm_lock/IRQ unwind (SYS-AARCH64-02) lives in
     * arch_uaccess_fault_fixup next to the code that takes those locks.
     * A wild kernel pointer that merely lands in user VA now panics below. */
    struct pt_regs *next = nxhal_fault_handle(
        frame, (frame->spsr & 0xF) == 0, far, elr, "SYNC EXCEPTION", esr);
    if (next)
      return next;

    /* Kernel fault: the address space may be compromised — every line below
     * goes through fault_printf (lock-free, no per-CPU buffer).  nxhal_diag_panic() at
     * the end (FIX(PANIC-LOCKFREE-01), kernel/lib/printk.c) always uses that
     * same lock-free output path now, not only when fault_depth() > 0. */
    nxhal_diag_fault("%s", "--- Kernel Exception Context Dump ---\n");
    nxhal_diag_fault("SPSR_EL1: 0x%016lx\n", frame->spsr);
    nxhal_diag_fault("ELR_EL1:  0x%016lx\n", frame->elr);
    nxhal_diag_fault("FAR_EL1:  0x%016lx\n", far);
    nxhal_diag_fault("ESR_EL1:  0x%016lx\n", esr);
    nxhal_diag_fault("EC: 0x%x, ISS: 0x%x\n", ec, (uint32_t)(esr & 0xFFFFFF));
    if (arch_frame_on_fault_stack(frame)) {
      /* The vector switched us onto the per-CPU fault stack; the SP at the
       * moment of the abort was parked just above the frame. */
      nxhal_diag_fault("Frame: %p (per-CPU fault stack), faulting SP: 0x%016lx\n",
                   (void *)frame, *(uint64_t *)((char *)frame + 816) + 16);
    } else {
      nxhal_diag_fault("Frame: %p (faulting kernel stack)\n", (void *)frame);
    }

    if (elr == 0) {
      nxhal_diag_fault("%s", "CRITICAL: Kernel jumped to NULL! Check exception "
                         "vector table and function pointers.\n");
      nxhal_diag_fault("Stack at 0x%lx:\n", (uint64_t)frame);
      for (int i = 0; i < 8; i++) {
        nxhal_diag_fault("  [%p] 0x%016lx\n", (void *)&((uint64_t *)frame)[i * 2],
                     ((uint64_t *)frame)[i * 2]);
      }
    }

    for (int i = 0; i < 31; i += 2) {
      if (i + 1 < 31) {
        nxhal_diag_fault("X%02d: 0x%016lx  X%02d: 0x%016lx\n", i, frame->regs[i],
                     i + 1, frame->regs[i + 1]);
      } else {
        nxhal_diag_fault("X%02d: 0x%016lx\n", i, frame->regs[i]);
      }
    }
    nxhal_diag_fault("SP_EL0:  0x%016lx\n", frame->sp_el0);
    nxhal_diag_fault("%s", "-----------------------------\n");
    nxhal_fault_backtrace_regs(frame->elr, frame->regs[29]);
    nxhal_fault_backtrace_scan((uint64_t)frame);
    nxhal_diag_panic("Unrecoverable kernel exception");
  }

  /* Unreachable today (the block above either schedules or panics); kept so
   * any future early-return path unwinds the recursion guard correctly. */
  nxhal_fault_exit();
  return frame;
}

/*
 * fiq_handler - unexpected FIQ (Phase A step 13).
 *
 * This kernel never configures FIQ sources; the vectors used to be `b .`
 * silent hangs.  An EL0-origin FIQ terminates the task (something user-
 * triggerable raised it); an EL1-origin FIQ means misconfigured hardware or
 * vector corruption — panic with the full diagnostic.
 */
struct pt_regs *fiq_handler(struct pt_regs *frame) {
  if (nxhal_fault_enter() > 1) {
    nxhal_diag_fault("\n[FATAL] NESTED FIQ ELR=%016lx — halting\n", frame->elr);
    hal_cpu_halt();
  }
  nxhal_diag_fault(
      "\n[FIQ] Unexpected FIQ: ELR=%016lx SPSR=%016lx (FIQ is never enabled)\n",
      frame->elr, frame->spsr);
  struct pt_regs *next = nxhal_fault_handle(
      frame, (frame->spsr & 0xF) == 0, 0, frame->elr, "UNEXPECTED FIQ", 0);
  if (next)
    return next;
  nxhal_fault_backtrace_regs(frame->elr, frame->regs[29]);
  nxhal_fault_backtrace_scan((uint64_t)frame);
  nxhal_diag_panic("Unexpected FIQ at EL1 (ELR 0x%lx)", frame->elr);
}

/*
 * aarch32_handler - exception from AArch32 EL0 (Phase A step 13).
 *
 * The kernel only loads AArch64 ELFs, so this means a process switched to
 * AArch32 state (or SPSR corruption).  Terminate it; if the frame claims EL1
 * origin the vector table itself is suspect — panic.
 */
struct pt_regs *aarch32_handler(struct pt_regs *frame) {
  if (nxhal_fault_enter() > 1) {
    nxhal_diag_fault("\n[FATAL] NESTED AArch32 exception ELR=%016lx — halting\n",
                 frame->elr);
    hal_cpu_halt();
  }
  nxhal_diag_fault("\n[EL0-32] AArch32 EL0 exception: ELR=%016lx SPSR=%016lx "
               "(unsupported)\n",
               frame->elr, frame->spsr);
  struct pt_regs *next = nxhal_fault_handle(frame, 1, 0, frame->elr,
                                                    "AARCH32 EL0 EXCEPTION", 0);
  if (next)
    return next;
  nxhal_fault_backtrace_regs(frame->elr, frame->regs[29]);
  nxhal_fault_backtrace_scan((uint64_t)frame);
  nxhal_diag_panic("AArch32 exception with no current task (ELR 0x%lx)", frame->elr);
}

/*
 * arch_secondary_stacks[] - per-CPU kernel stack top pointers for CPUs
 * 0..MAX_CPUS-1. Index i holds the TOP (highest address) of the 128KB kernel
 * stack for CPU i, because AArch64 stacks grow downward. Populated by
 * arch_smp_setup_stacks() before any secondary CPU is woken.
 */
void *arch_secondary_stacks[NXHAL_CPU_LIMIT] = {0};
extern char
    __kernel_stack[]; /* BSS symbol; MAX_CPUS * 128KB array, see start.S */

/*
 * arch_smp_setup_stacks - allocate and record kernel stack tops for all CPUs.
 *
 * Parameters:
 *   cpu_count  Number of CPUs to prepare stacks for (clamped to MAX_CPUS).
 *
 * Strategy:
 *   CPUs 0..7 (indices 0-based) use a static BSS region (__kernel_stack) that
 * is pre-allocated in the linker script as MAX_CPUS * 128KB.  The top of CPU
 * i's slice is at &__kernel_stack[(i+1) * 131072] (131072 == 128 KB).
 *
 *   CPUs 8 and above receive a dynamically allocated 128KB region from the PMM.
 *   The pointer stored is the HIGH end (top) because stacks grow downward.
 *
 * Side effects: writes arch_secondary_stacks[]; allocates PMM pages for CPUs
 * >= 8. Called by: arch_smp_init() in platform.c before
 * arch_impl_smp_wake_secondary().
 */
void arch_smp_setup_stacks(uint32_t cpu_count) {
  if (cpu_count > NXHAL_CPU_LIMIT)
    cpu_count = NXHAL_CPU_LIMIT;

  for (uint32_t i = 0; i < cpu_count; i++) {
    if (i < 8) {
      /* First 8 cores use the static BSS stack area (512KB reserved).
       * Each 128KB slice: CPU i occupies [i*128KB .. (i+1)*128KB).
       * The stack pointer is set to the TOP of the slice (+1 past end). */
      arch_secondary_stacks[i] =
          (void *)&__kernel_stack[(uint64_t)(i + 1) * 131072];
    } else {
      /* Core 9+ needs dynamic allocation from PMM */
      void *ptr = nxhal_memory_alloc_pages(131072 / 4096);
      if (!ptr) {
        nxhal_diag_error("CPU: OOM! Cannot allocate stack for core %u\n", i);
        break;
      }
      /* Stack grows down, return the TOP */
      arch_secondary_stacks[i] = (void *)((uintptr_t)ptr + 131072);
      nxhal_diag_info("CPU: Core %u using dynamic stack at %p\n", i,
              arch_secondary_stacks[i]);
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
  if (cpu_id >= NXHAL_CPU_LIMIT)
    return NULL;

  /* If dynamic stacks are set up, use them */
  if (arch_secondary_stacks[cpu_id])
    return arch_secondary_stacks[cpu_id];

  /* Bootstrap fallback: same formula as arch_smp_setup_stacks for CPUs < 8 */
  return (void *)&__kernel_stack[(uint64_t)(cpu_id + 1) * 131072];
}

/*
 * arch_vmm_set_secondary_pgd - publish the KERNEL PGD to secondary CPUs.
 *
 * Parameters:
 *   pgd  Physical address of the kernel L0 page table (the TTBR1 root).
 *
 * secondary_ttbr1 (defined in start.S .data) is read by secondary_startup
 * in start.S before the secondary CPU has its own MMU enabled and loaded
 * into TTBR1_EL1 (the kernel half; TTBR0 gets the boot identity tables).
 * Writing the value and then cache-cleaning it ensures the physical cache
 * line is written back to DRAM so that the secondary CPU's pre-MMU
 * (uncached) read sees the correct value.
 */
extern uint64_t secondary_ttbr1;
void arch_vmm_set_secondary_pgd(uint64_t pgd) {
  secondary_ttbr1 = pgd;
  hal_cache_clean(&secondary_ttbr1, sizeof(secondary_ttbr1));
  hal_mb();
}

/*
 * arch_vmm_init_hw - install the kernel PGD in TTBR1_EL1.
 *
 * Parameters:
 *   kernel_pgd  Physical address of the kernel L0 page table (higher-half
 *               direct map + image, built by vmm_init via vmm_map_ram_wx).
 *
 * Higher-half model (see memlayout.h / boot/start.S): the MMU, caches,
 * MAIR and TCR (48/48 TTBR0/TTBR1 split, 4KB granules, IPS=40-bit) are
 * already configured by start.S before any C runs — the kernel executes
 * at its link address through TTBR1 boot tables.  This function only
 * swaps the boot TTBR1 tables for the real kernel PGD.
 *
 * TTBR0 is the USER half: it keeps the boot identity tables until the
 * scheduler loads the first process PGD (or the idle user PGD allocated
 * here, which contains no mappings at all).
 */
static uint64_t *aarch64_idle_user_pgd;

void arch_vmm_init_hw(uint64_t kernel_pgd) {
  nxhal_diag_info("AArch64 VMM: Installing kernel PGD 0x%lx in TTBR1\n", kernel_pgd);

  /* Idle/kernel-thread TTBR0: an empty L0 table, so a CPU parked on a
   * kernel thread maps NO user half at all (instead of a stale process
   * PGD — SCHED-UAF-01 — or the kernel PGD aliased into the low half). */
  if (!aarch64_idle_user_pgd) {
    aarch64_idle_user_pgd = nxhal_memory_alloc_page();
    if (aarch64_idle_user_pgd) {
      memset(aarch64_idle_user_pgd, 0, 4096);
      hal_cache_clean(aarch64_idle_user_pgd, 4096);
    }
  }

  /* Ensure the new tables are visible to the walker, then switch TTBR1. */
  arch_impl_mb();
  arch_impl_tlb_flush_local();
  hal_vmm_set_kernel_pgd(kernel_pgd);
  arch_impl_mb();
  arch_impl_tlb_flush_local();
  arch_impl_isb();

  nxhal_diag_info("%s", "AArch64 VMM: Kernel PGD active (TTBR1).\n");
}

/* idle_user_pgd_phys - PA of the empty TTBR0 table for kernel threads;
 * 0 if not yet allocated (pre-VMM). */
static uint64_t idle_user_pgd_phys(void) {
  return aarch64_idle_user_pgd ? virt_to_phys(aarch64_idle_user_pgd) : 0;
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
 * PGD and each process PGD (via arch_vmm_create_process_pgd ->
 * arch_vmm_map_mmio).
 */
void arch_vmm_map_mmio(uint64_t *pgd) {
  /* Map MMIO (UART, GIC, VirtIO) at its direct-map VA (phys_to_virt;
   * identity while KERNEL_VIRT_BASE == 0).
   * 0x08000000 to 0x0A800000 covers typical QEMU virt devices. */
  /* This one range carries the GIC, the PL011 UART and all 32 VirtIO-MMIO
   * slots.  If it is not mapped there is no interrupt controller, no console
   * and no device of any kind -- and the failure would appear as a system that
   * boots into silence, which is indistinguishable from a hang.  The amd64
   * counterpart in arch/amd64/mm/mmu.c makes the same call fatal for the same
   * reason; keeping the two arches identical HERE is what stops one platform
   * from silently tolerating what the other refuses. */
  if (arch_vmm_map_range(virt_to_phys(pgd),
                         (uint64_t)phys_to_virt(0x08000000UL), 0x08000000UL,
                         0x02800000UL, PAGE_DEVICE) != 0)
    nxhal_diag_panic("arch_vmm_map_mmio: cannot map the 0x08000000..0x0A800000 MMIO "
          "window — GIC, UART and VirtIO are all unreachable");
}

/*
 * arch_cpu_switch_context - perform the architecture-specific address-space
 * switch.
 *
 * Parameters:
 *   next  Process descriptor for the task being scheduled in.
 *
 * Writes TTBR0_EL1 with the physical address of next->page_table (the process's
 * L0 PGD).  The caller (schedule / ctx_switch) is responsible for saving and
 * restoring the general-purpose register context (via ctx_switch in context.S).
 *
 * A kernel thread (e.g. idle) has no private address space (page_table ==
 * NULL): switch it onto the shared kernel_pgd instead of leaving the PREVIOUS
 * process's PGD active in TTBR0_EL1.
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
 * (the redundant hal_vmm_set_pgd block in nxhal_core_schedule() was removed), so the TLB
 * maintenance the scheduler block used to issue is performed here:
 * hal_vmm_set_pgd is a bare "msr ttbr0_el1" (arch.h) with no implicit
 * barriers, and hal_tlb_flush_all already ends with DSB ISH + ISB (arch.h),
 * which completes the switch — no further barrier is needed after it.
 */
void nxhal_provider_cpu_switch_context(
    const struct nxhal_cpu_context_switch *context) {
  uint64_t pgd;
  uint64_t asid;

  if (context == NULL)
    hal_cpu_halt();

  pgd = (context->flags & NXHAL_CPU_CONTEXT_HAS_USER_ROOT)
            ? context->user_root_phys
            : idle_user_pgd_phys();
  asid = (context->flags & NXHAL_CPU_CONTEXT_HAS_USER_ROOT)
             ? (uint64_t)context->asid
             : 0U;
  if (pgd != 0U) {
    hal_vmm_set_pgd(pgd | (asid << 48));
    hal_isb();
  }
}
