#ifndef AMD64_INTERNAL_H
#define AMD64_INTERNAL_H

#include <kernel/types.h>
#include <arch/pt_regs.h>

void idt_init(void);
struct pt_regs *amd64_isr_dispatch(struct pt_regs *regs);
void gdt_init(void);
void gdt_set_rsp0(uint64_t rsp0);
void amd64_syscall_init(void);
struct pt_regs *amd64_syscall_handler(struct pt_regs *frame);
void arch_vmm_init(void);
void pic_init(void);
void pic_send_eoi(uint8_t irq);
void pic_unmask(uint8_t irq);
void pit_init(void);
void uart_init(void);
struct pt_regs *amd64_timer_interrupt(struct pt_regs *regs);
struct pt_regs *amd64_keyboard_interrupt(struct pt_regs *regs);

uint64_t timer_get_us(void);
void udelay(uint32_t us);
void arch_pci_init(void);

#endif
