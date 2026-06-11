/*
 * kernel/cpu.c
 * Generic CPU management logic
 */
#include <kernel/cpu.h>
#include <kernel/arch.h>
#include <kernel/fault.h>
#include <kernel/printk.h>

/* CPU info array */
struct cpu_info cpu_data[MAX_CPUS];
uint32_t nr_cpus = 0;

/* Generic implementation (weak - can be overridden by arch-specific) */
__attribute__((weak))
struct cpu_info *get_cpu_info(void) {
  uint32_t id = arch_get_cpu_id();
  if (id >= MAX_CPUS) {
    /* Critical failure if CPU ID is out of bounds */
    while (1);
  }
  return &cpu_data[id];
}

/*
 * Generic Exception Wrappers
 */
struct pt_regs *serror_handler(struct pt_regs *frame) {
  /* SError = asynchronous external abort: the machine state is suspect by
   * definition.  Recursion guard + lock-free output only (kernel/fault.h);
   * panic() sees fault_depth() > 0 and uses its fault-safe mode. */
  if (fault_enter() > 1) {
    fault_printf("\n[FATAL] NESTED SError frame=%016lx — halting\n", (uint64_t)frame);
    arch_cpu_halt();
  }
  fault_printf("SError at frame=0x%016lx\n", (uint64_t)frame);
  panic("SError exception");
  return frame;
}
