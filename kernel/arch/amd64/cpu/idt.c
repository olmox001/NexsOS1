/*
 * kernel/arch/amd64/cpu/idt.c
 * Interrupt Descriptor Table for x86-64
 */
#include <kernel/types.h>
#include <kernel/string.h>
#include <kernel/printk.h>
#include <arch/pt_regs.h>
#include <arch/arch.h>
#include <arch/amd64_internal.h>
#include <kernel/arch.h>

#define IDT_ENTRIES 256

struct idt_entry {
  uint16_t offset_lo;
  uint16_t selector;
  uint8_t  ist;       /* Interrupt Stack Table offset */
  uint8_t  type_attr; /* Type and Attributes */
  uint16_t offset_mid;
  uint32_t offset_hi;
  uint32_t zero;
} __packed;

struct idtr {
  uint16_t limit;
  uint64_t base;
} __packed;

static struct idt_entry idt[IDT_ENTRIES] __aligned(16);
/* static struct idtr idtr; - Removed, using local idtr in idt_init */

/* Defined in isr_stubs.S */
extern uint64_t isr_stub_table[];

static void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist) {
  idt[num].offset_lo  = (uint16_t)(base & 0xFFFF);
  idt[num].selector   = sel;
  idt[num].ist        = ist;
  idt[num].type_attr  = flags;
  idt[num].offset_mid = (uint16_t)((base >> 16) & 0xFFFF);
  idt[num].offset_hi  = (uint32_t)(base >> 32);
  idt[num].zero       = 0;
}

static int idt_initialized = 0;

void idt_init(void) {
  if (arch_get_cpu_id() == 0) {
      memset(&idt, 0, sizeof(struct idt_entry) * IDT_ENTRIES);

      /* Set up IDT gates */
      for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E, 0);
      }

      /* Syscall via int 0x80 */
      idt_set_gate(0x80, isr_stub_table[0x80], 0x08, 0xEE, 0);
      
      idt_initialized = 1;
  }

  /* Wait for CPU 0 to finish if needed (not strictly necessary with sequential boot) */
  while (!idt_initialized) arch_nop();

  struct idtr local_idtr;
  local_idtr.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
  local_idtr.base  = (uint64_t)&idt;

  __asm__ __volatile__("lidt %0" : : "m"(local_idtr));
}

/* Detailed Register Dump */
static void amd64_dump_regs(struct pt_regs *regs) {
  pr_err("RIP: %016lx CS: %02lx RFLAGS: %016lx\n", regs->rip, regs->cs, regs->rflags);
  pr_err("RAX: %016lx RBX: %016lx RCX: %016lx RDX: %016lx\n", regs->rax, regs->rbx, regs->rcx, regs->rdx);
  pr_err("RSI: %016lx RDI: %016lx RBP: %016lx RSP: %016lx\n", regs->rsi, regs->rdi, regs->rbp, regs->rsp);
  pr_err("R8:  %016lx R9:  %016lx R10: %016lx R11: %016lx\n", regs->r8, regs->r9, regs->r10, regs->r11);
  pr_err("R12: %016lx R13: %016lx R14: %016lx R15: %016lx\n", regs->r12, regs->r13, regs->r14, regs->r15);
  pr_err("Vector: %ld, Error Code: %lx\n", regs->vec, regs->err);
}

/* Page Fault Handler */
static void amd64_page_fault_handler(struct pt_regs *regs) {
  uint64_t cr2;
  __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
  
  uint64_t error_code = regs->err;
  
  pr_err("\n[C%d] PAGE FAULT: Access to 0x%lx\n", arch_get_cpu_id(), cr2);
  pr_err("Error Code: 0x%lx (P:%d, W:%d, U:%d, R:%d, I:%d)\n",
         error_code,
         (error_code & 1) ? 1 : 0,
         (error_code & 2) ? 1 : 0,
         (error_code & 4) ? 1 : 0,
         (error_code & 8) ? 1 : 0,
         (error_code & 16) ? 1 : 0);
  
  amd64_dump_regs(regs);
  arch_cpu_halt();
}

/* General Protection Fault Handler */
static void amd64_gpf_handler(struct pt_regs *regs) {
  pr_err("\n[C%d] GENERAL PROTECTION FAULT\n", arch_get_cpu_id());
  amd64_dump_regs(regs);
  arch_cpu_halt();
}

/* Double Fault Handler */
static void amd64_double_fault_handler(struct pt_regs *regs) {
  pr_err("\n[C%d] DOUBLE FAULT\n", arch_get_cpu_id());
  amd64_dump_regs(regs);
  arch_cpu_halt();
}

extern struct pt_regs *kernel_syscall_dispatcher(struct pt_regs *regs);
extern struct pt_regs *kernel_timer_tick(struct pt_regs *regs);

/* Main Exception/Interrupt Dispatcher (called from isr_stubs.S) */
struct pt_regs *amd64_isr_dispatch(struct pt_regs *regs) {
  uint64_t vec = regs->vec;

  if (vec < 32) {
    /* Handle exceptions */
    switch (vec) {
      case 8: /* Double Fault */
        amd64_double_fault_handler(regs);
        break;
      case 13: /* General Protection Fault */
        amd64_gpf_handler(regs);
        break;
      case 14: /* Page Fault */
        amd64_page_fault_handler(regs);
        break;
      default:
        pr_err("\n[C%d] Unhandled CPU Exception: %ld\n", arch_get_cpu_id(), vec);
        amd64_dump_regs(regs);
        arch_cpu_halt();
        break;
    }
  } else if (vec == 0x80) {
    /* Legacy syscall */
    return kernel_syscall_dispatcher(regs);
  } else {
    /* Hardware interrupts (32-255) */
    struct pt_regs *ret_regs = regs;

    if (vec == 32) {
        /* Timer Interrupt (PIT or LAPIC) */
        ret_regs = kernel_timer_tick(regs);
    } else {
        /* All other Hardware interrupts - route via generic system */
        if (vec != 32) {
            pr_debug("AMD64: Hardware Interrupt Vector %lu triggered!\n", vec);
        }
        extern struct pt_regs *irq_dispatch(uint32_t irq, struct pt_regs * regs);
        ret_regs = irq_dispatch(vec, regs);
    }

    /* Acknowledge LAPIC for all HW interrupts */
    lapic_eoi();

    /* Also acknowledge legacy PIC for its range (32-47) if active */
    if (vec >= 32 && vec < 48) {
        pic_send_eoi(vec - 32);
    }

    return ret_regs;
  }
  
  return regs;
}
