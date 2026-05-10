/*
 * kernel/arch/aarch64/cpu/cpu.c
 * CPU and exception handling for AArch64
 */
#include <drivers/gic.h>
#include <kernel/printk.h>
#include <kernel/types.h>

/* Exception frame structure */
#include <kernel/cpu.h>
#include <kernel/sched.h>

#include <kernel/arch.h>

/* CPU info array (max 8 CPUs) */
struct cpu_info cpu_data[8];
uint32_t nr_cpus = 0;

/* External functions from assembly */
extern void exception_vectors_install(void);

/*
 * Get current CPU ID
 */
uint32_t cpu_id(void) { return arch_get_cpu_id(); }

/*
 * Get current CPU info
 */
struct cpu_info *get_cpu_info(void) {
  uint32_t id = cpu_id();
  if (id >= 8) {
    /* Manual panic to avoid infinite recursion if panic uses get_cpu_info logic
     */
    /* Just hang or try to output something simple */
    /* uart_puts("CRITICAL: CPU ID OUT OF BOUNDS!\n"); */
    while (1) {
    }
  }
  return &cpu_data[id];
}

/*
 * Initialize CPU subsystem
 */
void cpu_init(void) {
  uint32_t id = cpu_id();

  cpu_data[id].cpu_id = id;
  cpu_data[id].online = 1;

  if (id == 0) {
    nr_cpus = 1;
    pr_info("CPU: Primary core %u initialized\n", id);
  } else {
    /* Avoid pr_info here as it might cause lock contention with primary core
     * boot logs */
    nr_cpus++;
  }

  /* Enable FPU/SIMD (NEON) - set CPACR_EL1.FPEN = 0b11 */
  uint64_t cpacr = arch_get_cpacr();
  cpacr |= (3 << 20); /* FPEN bits [21:20] = 0b11 */
  arch_set_cpacr(cpacr);
  arch_isb();

  /* Install exception vector table */
  exception_vectors_install();

  pr_info("CPU: VBAR_EL1 set to 0x%lx\n", arch_get_vbar());
}

/*
 * Synchronous exception handler
 */
struct pt_regs *sync_handler(struct pt_regs *frame) {
  uint64_t esr, far, elr;
  uint32_t ec;

  if (!frame)
    return NULL;

  /* Read exception syndrome */
  esr = arch_get_esr();
  far = arch_get_far();
  elr = frame->elr;

  ec = (esr >> 26) & 0x3F;

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
    /* If fault came from EL0 (User Mode), terminate process instead of panic */
    if ((frame->spsr & 0xF) == 0) {
      pr_err("USER FAULT: EC=0x%x (0x%lx) FAR=0x%016lx ELR=0x%016lx\n", ec, esr,
             far, elr);
      pr_err("Terminating PID %d\n", current_process->pid);

      /* Optional: notify server */
      /* ... same logic as syscall_handler ... */

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
 * System error handler
 */
struct pt_regs *serror_handler(struct pt_regs *frame) {
  pr_err("SError at ELR=0x%016lx ESR=0x%016lx\n", frame->elr, arch_get_esr());
  panic("SError exception");
}

/*
 * Syscall handler is defined in syscall.c
 */
extern struct pt_regs *syscall_handler(struct pt_regs *frame);

/*
 * Enable interrupts (only IRQ, keep SError masked)
 */
void local_irq_enable(void) { arch_local_irq_enable(); }

/*
 * Disable interrupts
 */
void local_irq_disable(void) { arch_local_irq_disable(); }

/*
 * Save and disable interrupts
 */
uint64_t local_irq_save(void) {
  uint64_t flags;
  arch_local_irq_save(&flags);
  return flags;
}

/*
 * Restore interrupt state
 */
void local_irq_restore(uint64_t flags) { arch_local_irq_restore(flags); }
