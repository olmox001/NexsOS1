/*
 * kernel/arch/amd64/cpu/gdt.c
 * Global Descriptor Table + Task State Segment for x86-64
 */
#include <kernel/types.h>
#include <kernel/string.h>
#include <arch/arch.h>
#include <arch/amd64_internal.h>

/* ─── GDT Segment Selectors ─── */
#define GDT_NULL      0x00
#define GDT_KERN_CODE 0x08
#define GDT_KERN_DATA 0x10
#define GDT_USER_DATA 0x18  /* Must be before USER_CODE for SYSRET */
#define GDT_USER_CODE 0x20
#define GDT_TSS       0x28
#define GDT_ENTRIES   7     /* null + kcode + kdata + udata + ucode + tss(2) */

/* ─── TSS ─── */
struct tss64 {
  uint32_t reserved0;
  uint64_t rsp0;    /* Ring 0 stack pointer */
  uint64_t rsp1;
  uint64_t rsp2;
  uint64_t reserved1;
  uint64_t ist[7];  /* Interrupt Stack Table */
  uint64_t reserved2;
  uint16_t reserved3;
  uint16_t iopb_offset;
} __packed;

static struct tss64 tss __aligned(16);

/* ─── GDT Entry ─── */
struct gdt_entry {
  uint16_t limit_lo;
  uint16_t base_lo;
  uint8_t  base_mid;
  uint8_t  access;
  uint8_t  granularity;
  uint8_t  base_hi;
} __packed;

/* System segment (TSS) descriptor is 16 bytes in Long Mode */
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

/* Raw GDT storage (7 * 8 = 56 bytes, but TSS takes 2 entries = 16 bytes) */
static uint64_t gdt_raw[GDT_ENTRIES] __aligned(16);

static void gdt_set_entry(int index, uint8_t access, uint8_t gran) {
  struct gdt_entry *e = (struct gdt_entry *)&gdt_raw[index];
  e->limit_lo    = 0xFFFF;
  e->base_lo     = 0;
  e->base_mid    = 0;
  e->access      = access;
  e->granularity = gran;
  e->base_hi     = 0;
}

static void gdt_set_tss(int index, uint64_t base, uint32_t limit) {
  struct gdt_system_entry *e = (struct gdt_system_entry *)&gdt_raw[index];
  e->limit_lo    = (uint16_t)(limit & 0xFFFF);
  e->base_lo     = (uint16_t)(base & 0xFFFF);
  e->base_mid    = (uint8_t)((base >> 16) & 0xFF);
  e->access      = 0x89; /* Present, 64-bit TSS (Available) */
  e->granularity = (uint8_t)((limit >> 16) & 0x0F);
  e->base_hi     = (uint8_t)((base >> 24) & 0xFF);
  e->base_upper  = (uint32_t)(base >> 32);
  e->reserved    = 0;
}

void gdt_init(void) {
  memset(gdt_raw, 0, sizeof(gdt_raw));

  /* 0x00: Null descriptor */
  gdt_raw[0] = 0;

  /* 0x08: Kernel Code — 64-bit, DPL=0, Execute/Read */
  gdt_set_entry(1, 0x9A, 0xAF); /* P=1, DPL=0, S=1, Type=A(Exec/Read), L=1(64-bit) */

  /* 0x10: Kernel Data — DPL=0, Read/Write */
  gdt_set_entry(2, 0x92, 0xCF); /* P=1, DPL=0, S=1, Type=2(Read/Write) */

  /* 0x18: User Data — DPL=3, Read/Write */
  gdt_set_entry(3, 0xF2, 0xCF); /* P=1, DPL=3, S=1, Type=2(Read/Write) */

  /* 0x20: User Code — 64-bit, DPL=3, Execute/Read */
  gdt_set_entry(4, 0xFA, 0xAF); /* P=1, DPL=3, S=1, Type=A(Exec/Read), L=1 */

  /* 0x28: TSS (occupies 2 GDT slots = 16 bytes) */
  memset(&tss, 0, sizeof(tss));
  tss.iopb_offset = sizeof(tss);
  gdt_set_tss(5, (uint64_t)&tss, sizeof(tss) - 1);

  /* Load GDT */
  struct gdtr gdtr = {
    .limit = sizeof(gdt_raw) - 1,
    .base  = (uint64_t)gdt_raw
  };

  __asm__ __volatile__(
    "lgdt %0\n\t"
    /* Reload CS via far return */
    "pushq $0x08\n\t"       /* Kernel Code selector */
    "leaq 1f(%%rip), %%rax\n\t"
    "pushq %%rax\n\t"
    "lretq\n\t"
    "1:\n\t"
    /* Reload data segments */
    "movw $0x10, %%ax\n\t"  /* Kernel Data selector */
    "movw %%ax, %%ds\n\t"
    "movw %%ax, %%es\n\t"
    "movw %%ax, %%fs\n\t"
    "movw %%ax, %%gs\n\t"
    "movw %%ax, %%ss\n\t"
    :
    : "m"(gdtr)
    : "rax", "memory"
  );

  /* Load TSS */
  __asm__ __volatile__("ltr %0" :: "r"((uint16_t)GDT_TSS));
}

/*
 * Update RSP0 in TSS — called on every context switch
 * so Ring 3 -> Ring 0 transitions use the correct kernel stack.
 */
void gdt_set_rsp0(uint64_t rsp0) {
  tss.rsp0 = rsp0;
}
