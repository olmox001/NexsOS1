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
static struct idtr idtr;

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

void idt_init(void) {
  memset(&idt, 0, sizeof(struct idt_entry) * IDT_ENTRIES);

  /* Set up IDT gates */
  for (int i = 0; i < IDT_ENTRIES; i++) {
    /* 0x8E = Present(1), DPL(00), Storage(0), GateType(1110 -> 32-bit Interrupt Gate)
     * For x86-64, GateType 1110 = 64-bit Interrupt Gate */
    idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E, 0);
  }

  /* Syscall via int 0x80 (Legacy fallback, though we use MSR syscall) */
  /* 0xEE = Present, DPL=3, Interrupt Gate */
  idt_set_gate(0x80, isr_stub_table[0x80], 0x08, 0xEE, 0);

  idtr.limit = sizeof(struct idt_entry) * IDT_ENTRIES - 1;
  idtr.base  = (uint64_t)&idt;

  __asm__ __volatile__("lidt %0" : : "m"(idtr));
}

/* Page Fault Handler */
static void amd64_page_fault_handler(struct pt_regs *regs) {
  uint64_t cr2;
  __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
  
  uint64_t error_code = regs->err;
  
  pr_err("PAGE FAULT: Access to 0x%lx\n", cr2);
  pr_err("Error Code: 0x%lx (P:%d, W:%d, U:%d, R:%d, I:%d)\n",
         error_code,
         (error_code & 1) ? 1 : 0,
         (error_code & 2) ? 1 : 0,
         (error_code & 4) ? 1 : 0,
         (error_code & 8) ? 1 : 0,
         (error_code & 16) ? 1 : 0);
  pr_err("RIP: 0x%lx\n", regs->rip);
  
  __arch_cpu_halt();
}

/* General Protection Fault Handler */
static void amd64_gpf_handler(struct pt_regs *regs) {
  pr_err("GENERAL PROTECTION FAULT\n");
  pr_err("Error Code: 0x%lx\n", regs->err);
  pr_err("RIP: 0x%lx\n", regs->rip);
  __arch_cpu_halt();
}

/* Double Fault Handler */
static void amd64_double_fault_handler(struct pt_regs *regs) {
  pr_err("DOUBLE FAULT\n");
  pr_err("Error Code: 0x%lx\n", regs->err);
  pr_err("RIP: 0x%lx\n", regs->rip);
  __arch_cpu_halt();
}

extern struct pt_regs *kernel_syscall_dispatcher(struct pt_regs *regs);
extern void amd64_timer_interrupt(void);
extern void amd64_keyboard_interrupt(void);

/* Main Exception/Interrupt Dispatcher (called from isr_stubs.S) */
struct pt_regs *amd64_isr_dispatch(struct pt_regs *regs) {
  uint64_t vec = regs->vec;

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
    case 0x80: /* Legacy syscall (if used) */
      return kernel_syscall_dispatcher(regs);
    case 32: /* PIT Timer */
      amd64_timer_interrupt();
      break;
    case 33: /* Keyboard */
      amd64_keyboard_interrupt();
      break;
    default:
      if (vec < 32) {
        pr_err("Unhandled CPU Exception: %ld\n", vec);
        pr_err("RIP: 0x%lx, Error Code: 0x%lx\n", regs->rip, regs->err);
        __arch_cpu_halt();
      } else {
        /* Hardware interrupt - to be routed via APIC/PIC */
      }
      break;
  }
  
  return regs;
}
