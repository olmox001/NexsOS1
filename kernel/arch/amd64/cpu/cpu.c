/*
 * kernel/arch/amd64/cpu/cpu.c
 * Architecture-specific CPU initialization and structures for x86-64.
 */
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <arch/amd64_internal.h>

#include <cpuid.h>

extern struct cpu_info cpu_data[MAX_CPUS];
extern uint32_t nr_cpus;

extern void gdt_init(void);
extern void idt_init(void);
extern void amd64_syscall_init(void);

void arch_cpu_init(void) {
  uint32_t id = arch_get_cpu_id();
  
  if (id >= MAX_CPUS) {
    panic("CPU ID %u exceeds MAX_CPUS", id);
  }

  gdt_init();
  idt_init();
  amd64_syscall_init();

  struct cpu_info *cpu = &cpu_data[id];
  memset(cpu, 0, sizeof(struct cpu_info));
  cpu->cpu_id = id;
  cpu->online = 1;
  spin_lock_init(&cpu->sched_lock);
  
  if (id == 0) {
      nr_cpus = 1;
  } else {
      __sync_fetch_and_add(&nr_cpus, 1);
  }

  /* Enable SSE (Required for modern compilers emitting xmm instructions) */
  uint64_t cr0, cr4;
  __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
  cr0 &= ~(1 << 2); /* Clear EM (Emulation) */
  cr0 |= (1 << 1);  /* Set MP (Monitor co-processor) */
  __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0));

  __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
  cr4 |= (1 << 9);  /* OSFXSR (OS support for fxsave/fxrstor) */
  cr4 |= (1 << 10); /* OSXMMEXCPT (OS support for unmasked simd fp exceptions) */
  __asm__ __volatile__("mov %0, %%cr4" :: "r"(cr4));
  
  /* Set up GS base for per-CPU data access */
  uint64_t cpu_info_ptr = (uint64_t)cpu;
  wrmsr(0xC0000101, cpu_info_ptr); /* IA32_GS_BASE */
  wrmsr(0xC0000102, cpu_info_ptr); /* IA32_KERNEL_GS_BASE */

  pr_info("AMD64 CPU %u initialized (GDT, IDT, Syscall, SSE, GS enabled)\n", id);
}

struct cpu_info *get_cpu_info(void) {
  uint32_t id = arch_get_cpu_id();
  if (id >= MAX_CPUS) return &cpu_data[0]; // Fallback
  return &cpu_data[id];
}

void arch_cpu_switch_context(struct process *next) {
  struct cpu_info *cpu = get_cpu_info();
  
  /* Update per-CPU data for assembly stubs (syscall.S / isr_stubs.S) */
  cpu->stack_top = next->kernel_stack;
  cpu->current_task = next;

  /* Update TSS RSP0 for interrupt stack switching */
  gdt_set_rsp0(next->kernel_stack);
}
