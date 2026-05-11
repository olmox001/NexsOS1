/*
 * kernel/arch/amd64/cpu/cpu.c
 * Architecture-specific CPU initialization and structures for x86-64.
 */
#include <kernel/cpu.h>
#include <kernel/printk.h>
#include <kernel/string.h>

extern void gdt_init(void);
extern void idt_init(void);
extern void amd64_syscall_init(void);

static struct cpu_info boot_cpu;

void arch_cpu_init(void) {
  gdt_init();
  idt_init();
  amd64_syscall_init();

  memset(&boot_cpu, 0, sizeof(boot_cpu));
  spin_lock_init(&boot_cpu.sched_lock);
  
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
  
  /* Set up GS base for per-CPU data access (required for syscall stack switching) */
  uint64_t cpu_info_ptr = (uint64_t)&boot_cpu;
  wrmsr(0xC0000101, cpu_info_ptr); /* IA32_GS_BASE */
  wrmsr(0xC0000102, cpu_info_ptr); /* IA32_KERNEL_GS_BASE */

  pr_info("AMD64 CPU initialized (GDT, IDT, Syscall, SSE, GS enabled)\n");
}

struct cpu_info *get_cpu_info(void) {
  /* For single-core, always return boot_cpu.
   * For SMP, we would read the LAPIC ID and index an array.
   */
  return &boot_cpu;
}
