/*
 * kernel/cpu.c
 * Generic CPU management logic
 */
#include <kernel/cpu.h>
#include <kernel/arch.h>
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
  pr_err("SError at ELR=0x%016lx\n", (uint64_t)frame);
  panic("SError exception");
  return frame;
}
