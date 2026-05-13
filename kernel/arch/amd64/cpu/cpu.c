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

struct cpu_info *get_cpu_info(void) {
  uint32_t id = arch_get_cpu_id();
  if (id >= MAX_CPUS) return &cpu_data[0];
  return &cpu_data[id];
}

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
