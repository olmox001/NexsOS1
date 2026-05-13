/*
 * kernel/arch/aarch64/cpu/cpu.c
 * CPU and exception handling for AArch64
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
 * Initialize CPU subsystem (HAL implementation)
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

  /* Enable FPU/SIMD (NEON) - set CPACR_EL1.FPEN = 0b11 */
  uint64_t cpacr = arch_impl_get_cpacr();
  cpacr |= (3 << 20); /* FPEN bits [21:20] = 0b11 */
  arch_impl_set_cpacr(cpacr);
  arch_impl_mb();
  arch_impl_isb();

  /* Install exception vector table */
  exception_vectors_install();

  pr_info("CPU: Vector Table set to 0x%lx\n", arch_impl_get_vbar());
}

/* Global flags for safe memory probing */
volatile bool probe_in_progress = false;
volatile bool probe_failed = false;

/*
 * Synchronous exception handler
 */
struct pt_regs *sync_handler(struct pt_regs *frame) {
  uint64_t esr, far, elr;
  uint32_t ec;

  if (!frame)
    return NULL;

  /* Read exception syndrome */
  esr = arch_get_fault_status();
  far = arch_get_fault_address();
  elr = frame->elr;

  ec = (esr >> 26) & 0x3F;

  /* Check if this is a Data Abort or Instruction Abort during a safe probe */
  if (probe_in_progress && (ec == 0x24 || ec == 0x25 || ec == 0x20 || ec == 0x21)) {
    probe_failed = true;
    /* Skip the faulting instruction (increment ELR by 4) */
    frame->elr += 4;
    return frame;
  }

  switch (ec) {
  case 0x00: /* Unknown exception */
    pr_err("Unknown exception at 0x%016lx\n", elr);
    break;

  case 0x15: /* SVC instruction (syscall) */
    return syscall_handler(frame);

  case 0x20: /* Instruction abort from lower EL */
  case 0x21: /* Instruction abort from same EL */
    pr_err("Instruction abort at 0x%016lx, FAR=0x%016lx\n", elr, far);
    break;

  case 0x24: /* Data abort from lower EL */
  case 0x25: /* Data abort from same EL */
    pr_err("Data abort at 0x%016lx, FAR=0x%016lx\n", elr, far);
    break;

  case 0x26: /* SP alignment fault */
    pr_err("SP alignment fault at 0x%016lx\n", elr);
    break;

  default:
    pr_err("Unhandled exception EC=0x%x at 0x%016lx\n", ec, elr);
    break;
  }
  
  if (ec != 0x15) {
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

void *arch_secondary_stacks[MAX_CPUS] = {0};
extern char __kernel_stack[];

/*
 * Setup stacks for all detected cores.
 * Dynamically allocates stacks beyond the first 8 cores.
 */
void arch_smp_setup_stacks(uint32_t cpu_count) {
    if (cpu_count > MAX_CPUS) cpu_count = MAX_CPUS;
    
    for (uint32_t i = 0; i < cpu_count; i++) {
        if (i < 8) {
            /* First 8 cores use the static BSS stack area (512KB reserved) */
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
 * Get kernel stack for a given CPU
 */
void *arch_get_kernel_stack(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return NULL;
    
    /* If dynamic stacks are set up, use them */
    if (arch_secondary_stacks[cpu_id]) return arch_secondary_stacks[cpu_id];
    
    /* Bootstrap fallback */
    return (void *)&__kernel_stack[(uint64_t)(cpu_id + 1) * 131072];
}

/*
 * Set PGD for secondary CPUs (used during boot)
 */
extern uint64_t secondary_ttbr0;
void arch_vmm_set_secondary_pgd(uint64_t pgd) {
    secondary_ttbr0 = pgd;
    arch_cache_clean_range(&secondary_ttbr0, sizeof(secondary_ttbr0));
    arch_mb();
}

void arch_vmm_init_hw(uint64_t kernel_pgd) {
  pr_info("AArch64 VMM: Setting up MAIR (PGD at 0x%lx)\n", kernel_pgd);
  
  /* Ensure all previous writes are visible */
  arch_impl_mb();
  arch_impl_isb();

  /* 1. Setup MAIR_EL1 (Memory Attribute Indirection Register) */
  /* Index 0: Normal Memory WB/WA, Index 1: Device Memory nGnRE */
  uint64_t mair = (0xFFUL << 0) | (0x04UL << 8);
  arch_impl_set_mair(mair);
  arch_impl_isb();

  pr_info("%s", "AArch64 VMM: Setting up TCR\n");
  /* 2. Setup TCR_EL1 (Translation Control Register) */
  /* T0SZ=16 (48-bit VA), SH0=3 (Inner), ORGN0=1 (WB/WA), IRGN0=1 (WB/WA) */
  /* IPS=0 (32-bit PA, enough for 2GB at 0x40000000), TG0=0 (4KB), EPD1=1 (Disable TTBR1) */
  uint64_t tcr = (16UL << 0) | (3UL << 12) | (1UL << 10) | (1UL << 8) | (0UL << 32) | (0UL << 14) | (1UL << 23);
  arch_impl_set_tcr(tcr);
  arch_impl_isb();

  pr_info("%s", "AArch64 VMM: Setting TTBR0\n");
  /* 3. Set TTBR0_EL1 */
  arch_impl_tlb_flush_local();
  arch_vmm_set_pgd(kernel_pgd);
  arch_impl_mb();
  arch_impl_isb();

  pr_info("%s", "AArch64 VMM: Enabling SCTLR bits (MMU, Caches)\n");
  /* 4. Enable MMU in SCTLR_EL1 */
  uint64_t sctlr = arch_impl_get_sctlr();
  
  /* Mandatory RES1 bits for EL1 SCTLR */
  sctlr |= (1UL << 29) | (1UL << 28) | (1UL << 23) | (1UL << 22) | (1UL << 20) | (1UL << 11);
  
  /* Enable MMU (M), Instruction Cache (I), Data Cache (C) */
  sctlr |= (1UL << 0) | (1UL << 12) | (1UL << 2);
  
  /* Disable Alignment Check (A) and Stack Alignment Check (SA) for now to be safe */
  sctlr &= ~((1UL << 1) | (1UL << 3));
  
  arch_impl_mb();
  arch_impl_set_sctlr(sctlr);
  arch_impl_mb();
  arch_impl_isb();

  pr_info("AArch64 VMM: MMU Enabled. SCTLR=0x%lx\n", sctlr);
}

void arch_vmm_map_mmio(uint64_t *pgd) {
  /* Identity Map MMIO (UART, GIC, VirtIO) */
  /* 0x08000000 to 0x0A800000 covers typical QEMU virt devices */
  arch_vmm_map_range((uint64_t)pgd, 0x08000000UL, 0x08000000UL, 
                     0x02800000UL, PAGE_DEVICE);
}

void arch_cpu_switch_context(struct process *next) {
    /* Switch address space */
    if (next->page_table) {
        arch_vmm_set_pgd((uint64_t)next->page_table);
    }
}
