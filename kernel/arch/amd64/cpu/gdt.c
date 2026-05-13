/*
 * kernel/arch/amd64/cpu/gdt.c
 * Global Descriptor Table + Task State Segment for x86-64
 */
#include <kernel/types.h>
#include <kernel/string.h>
#include <kernel/cpu.h>
#include <arch/arch.h>
#include <arch/amd64_internal.h>
#include <kernel/printk.h>
#include "gdt_defs.h"

/* ─── GDT Segment Selectors ─── */
#define GDT_NULL      0x00
#ifndef GDT_KERN_CODE
#endif /* GDT_KERN_CODE */
#ifndef GDT_KERN_DATA
#endif /* GDT_KERN_DATA */
#ifndef GDT_USER_DATA
#endif /* GDT_USER_DATA */
#ifndef GDT_USER_CODE
#endif /* GDT_USER_CODE */
#define GDT_TSS       0x28
#define GDT_ENTRIES   7

/* ─── TSS ─── */
struct tss64 {
  uint32_t reserved0;
  uint64_t rsp0;
  uint64_t rsp1;
  uint64_t rsp2;
  uint64_t reserved1;
  uint64_t ist[7];
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iopb_offset;
} __packed;

/* ─── GDT Entry ─── */
struct gdt_entry {
  uint16_t limit_lo;
  uint16_t base_lo;
  uint8_t  base_mid;
  uint8_t  access;
  uint8_t  granularity;
  uint8_t  base_hi;
} __packed;

struct gdt_system_entry {
  uint16_t limit_lo;
  uint16_t base_lo;
  uint8_t  base_mid;
  uint8_t  access;
  uint8_t  granularity;
  uint8_t  base_hi;
  uint32_t base_upper;
  uint32_t reserved;
} __packed;

struct gdtr {
  uint16_t limit;
  uint64_t base;
} __packed;

static struct tss64 tss_data[MAX_CPUS] __aligned(16);
static uint64_t gdt_raw[MAX_CPUS][GDT_ENTRIES] __aligned(16);

static void gdt_set_entry(uint64_t *gdt, int index, uint8_t access, uint8_t gran) {
  struct gdt_entry *e = (struct gdt_entry *)&gdt[index];
  e->limit_lo    = 0xFFFF;
  e->base_lo     = 0;
  e->base_mid    = 0;
  e->access      = access;
  e->granularity = gran;
  e->base_hi     = 0;
}

static void gdt_set_tss(uint64_t *gdt, int index, uint64_t base, uint32_t limit) {
  struct gdt_system_entry *e = (struct gdt_system_entry *)&gdt[index];
  e->limit_lo    = (uint16_t)(limit & 0xFFFF);
  e->base_lo     = (uint16_t)(base & 0xFFFF);
  e->base_mid    = (uint8_t)((base >> 16) & 0xFF);
  e->access      = 0x89;
  e->granularity = (uint8_t)((limit >> 16) & 0x0F);
  e->base_hi     = (uint8_t)((base >> 24) & 0xFF);
  e->base_upper  = (uint32_t)(base >> 32);
  e->reserved    = 0;
}

void gdt_init(void) {
  uint32_t cpu_id = arch_get_cpu_id();
  if (cpu_id >= MAX_CPUS) return;

  uint64_t *my_gdt = gdt_raw[cpu_id];
  struct tss64 *my_tss = &tss_data[cpu_id];

  memset(my_gdt, 0, GDT_ENTRIES * 8);
  memset(my_tss, 0, sizeof(struct tss64));

  gdt_set_entry(my_gdt, 1, 0x9A, 0xAF);
  gdt_set_entry(my_gdt, 2, 0x92, 0xCF);
  gdt_set_entry(my_gdt, 3, 0xF2, 0xCF);
  gdt_set_entry(my_gdt, 4, 0xFA, 0xAF);

  my_tss->iopb_offset = sizeof(struct tss64);
  gdt_set_tss(my_gdt, 5, (uint64_t)my_tss, sizeof(struct tss64) - 1);

  struct gdtr gdtr = {
    .limit = (GDT_ENTRIES * 8) - 1,
    .base  = (uint64_t)my_gdt
  };

  pr_info("GDT: Loading GDTR...\n");
  __asm__ __volatile__(
    "lgdt %0\n\t"
    "pushq $0x08\n\t"
    "leaq 1f(%%rip), %%rax\n\t"
    "pushq %%rax\n\t"
    "lretq\n\t"
    "1:\n\t"
    "movw $0x10, %%ax\n\t"
    "movw %%ax, %%ds\n\t"
    "movw %%ax, %%es\n\t"
    "movw %%ax, %%fs\n\t"
    "movw %%ax, %%ss\n\t"
    :
    : "m"(gdtr)
    : "rax", "memory"
  );

  pr_info("GDT: Loading TSS...\n");
  __asm__ __volatile__("ltr %0" :: "r"((uint16_t)GDT_TSS));
  pr_info("GDT: Done\n");
}

void gdt_set_rsp0(uint64_t rsp0) {
  uint32_t cpu_id = arch_get_cpu_id();
  if (cpu_id < MAX_CPUS) {
    tss_data[cpu_id].rsp0 = rsp0;
  }
}
