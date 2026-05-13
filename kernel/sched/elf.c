/*
 * kernel/sched/elf.c
 * ELF Loader (Identity Map / MMU-less optimized)
 */
#include <kernel/arch.h>
#include <kernel/elf.h>
#include <kernel/ext4.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

int process_load_elf(struct process *proc, const char *path) {
  uint32_t ino;
  if (ext4_find_inode(path, &ino) != 0) {
    pr_err("ELF: File not found: %s\n", path);
    return -1;
  }

  /* 1. Read ELF Header */
  Elf64_Ehdr ehdr;
  if (ext4_read_inode(ino, 0, (uint8_t *)&ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
    pr_err("%s", "ELF: Failed to read header\n");
    return -1;
  }

  /* 2. Verify Header */
  if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 ||
      ehdr.e_ident[EI_CLASS] != ELFCLASS64 || ehdr.e_machine != ARCH_TYPE) {
    pr_err("ELF: Invalid format (Machine: 0x%x, Expected: 0x%x)\n", 
           ehdr.e_machine, ARCH_TYPE);
    return -1;
  }

  /* 3. Load Segments */
  for (int i = 0; i < ehdr.e_phnum; i++) {
    Elf64_Phdr phdr;
    uint32_t ph_off = ehdr.e_phoff + (i * ehdr.e_phentsize);

    if (ext4_read_inode(ino, ph_off, (uint8_t *)&phdr, sizeof(phdr)) !=
        sizeof(phdr)) {
      pr_err("ELF: Failed to read PHDR %d\n", i);
      return -1;
    }

    if (phdr.p_type == PT_LOAD) {
      /* Generic User Mapping Flags */
      uint64_t flags = PTE_USER | PTE_VALID;

      if (phdr.p_flags & PF_W) {
        flags |= PTE_RW;
      } else {
        flags |= PTE_RO;
      }

      /* AArch64 specific UXN/PXN handled via HAL-neutral flags if defined */
#ifdef ARCH_AARCH64
      flags |= PTE_VALID | PTE_PAGE | PTE_AF | PTE_INNER_SHARE;
      if (!(phdr.p_flags & PF_X)) {
        flags |= PTE_UXN;
      }
#endif

      pr_info("ELF: Mapping Segment at 0x%lx (FileSz: 0x%lx, MemSz: 0x%lx)\n",
              phdr.p_vaddr, phdr.p_filesz, phdr.p_memsz);

      /* Allocate Pages for Memory Segment */
      uint64_t start_vpage = phdr.p_vaddr & ~(0xFFF);
      uint64_t end_vpage = (phdr.p_vaddr + phdr.p_memsz + 4095) & ~(0xFFF);

      for (uint64_t vaddr = start_vpage; vaddr < end_vpage; vaddr += 4096) {
        /* Allocate physical page */
        void *paddr = pmm_alloc_page();
        if (!paddr) {
          pr_err("ELF: Failed to allocate physical page for vaddr 0x%lx\n",
                 vaddr);
          return -1;
        }

        /* Map page in process address space */
        if (vmm_map_page(proc->page_table, vaddr, (uint64_t)paddr, flags) !=
            0) {
          pr_err("ELF: Failed to map page at 0x%lx\n", vaddr);
          pmm_free_page(paddr);
          return -1;
        }

        /* Get Kernel Virtual Address for writing (Use Identity Map) */
        void *kaddr = paddr;

        /* Zero the page content */
        memset(kaddr, 0, 4096);

        /* Copy data from file if within bounds */
        uint64_t seg_vstart = phdr.p_vaddr;
        uint64_t seg_vend_file = seg_vstart + phdr.p_filesz;
        uint64_t page_start = vaddr;
        uint64_t page_end = vaddr + 4096;

        uint64_t copy_start =
            (page_start > seg_vstart) ? page_start : seg_vstart;
        uint64_t copy_end =
            (page_end < seg_vend_file) ? page_end : seg_vend_file;

        if (copy_start < copy_end) {
          uint64_t copy_len = copy_end - copy_start;
          uint64_t offset_in_page = copy_start - page_start;
          uint64_t offset_in_file = phdr.p_offset + (copy_start - seg_vstart);

          ext4_read_inode(ino, offset_in_file,
                          (uint8_t *)kaddr + offset_in_page, copy_len);
        }

        /* Clean DC to PoU and invalid IC for executable pages */
        if (phdr.p_flags & PF_X) {
          arch_cache_sync_icache(kaddr, 4096);
        }
      }
    }
  }

  /* 4. Setup Stack (1MB at 0xC0000000) */
  uint64_t stack_base = 0xC0000000;
  uint64_t stack_size = 0x100000; // 1MB

  for (uint64_t vaddr = stack_base; vaddr < stack_base + stack_size;
       vaddr += 4096) {
    void *paddr = pmm_alloc_page();
    if (!paddr) {
      pr_err("%s", "ELF: Failed to allocate stack page\n");
      return -1;
    }
    if (vmm_map_page(proc->page_table, vaddr, (uint64_t)paddr, PAGE_USER) !=
        0) {
      pr_err("%s", "ELF: Failed to map stack page\n");
      pmm_free_page(paddr);
      return -1;
    }
  }

  proc->user_entry = ehdr.e_entry;
  proc->user_stack = stack_base + stack_size;

  /* Initialize Saved Context for Scheduler */
  /* proc->context already points to the top of the kernel stack (set in
   * process_create) */
  if (proc->context) {
    /* pr_info("ELF: Before init - PID %d context=%p ELR=0x%lx SP_EL0=0x%lx\n",
            proc->pid, (void*)proc->context, proc->context->elr,
       proc->context->sp_el0); */

    memset(proc->context, 0, sizeof(struct pt_regs));
    pt_regs_init_user_task(proc->context, proc->user_entry, proc->user_stack);

    /* pr_info("ELF: After init - PID %d\n", proc->pid);
    pr_info("ELF:   ELR=0x%lx\n", proc->context->elr);
    pr_info("ELF:   SP_EL0=0x%lx\n", proc->context->sp_el0);
    pr_info("ELF:   SPSR=0x%lx\n", proc->context->spsr); */

    /* CRITICAL: Flush the register frame to PoC so the scheduled CPU sees it */
    arch_cache_clean_range(proc->context, sizeof(struct pt_regs));
  } else {
    pr_err("%s", "ELF: proc->context is NULL!\n");
  }

  /* Ensure context is visible before enqueueing */
  arch_mb();
  arch_isb();

  return 0;
}
