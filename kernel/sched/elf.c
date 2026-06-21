/*
 * kernel/sched/elf.c
 * ELF Loader (Identity Map / MMU-less optimized)
 */
#include <kernel/arch.h>
#include <kernel/elf.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>

/* Largest argv vector marshalled onto a new task's stack.  Kept in lockstep
 * with SPAWN_MAX_ARGS in the syscall dispatcher (the source clamps argc). */
#define ELF_MAX_ARGS 16

/*
 * setup_user_args - lay out argc/argv at the top of the user stack.
 *
 * top_kaddr is the kernel direct-map pointer to the page backing the highest
 * stack page [stack_top-4096, stack_top).  We copy the argument strings just
 * below stack_top, then an array of argc+1 user pointers (NULL-terminated),
 * and return a 16-byte-aligned user SP pointing at that array.  *out_argv_uva
 * receives the user virtual address of the argv array (== the returned SP).
 *
 * argc is assumed already clamped to <= ELF_MAX_ARGS; the whole block fits in
 * one page (16 * 128 strings + 17 pointers < 4096).
 */
static uint64_t setup_user_args(void *top_kaddr, uint64_t stack_top, int argc,
                                char *const kargv[], uint64_t *out_argv_uva) {
  const uint64_t page_uva = stack_top - 4096;
  uint8_t *kpage = (uint8_t *)top_kaddr;
  uint64_t str_uva[ELF_MAX_ARGS];
  uint64_t uptr = stack_top;

  /* Strings, packed downward from the top of the page. */
  for (int i = argc - 1; i >= 0; i--) {
    size_t len = strlen(kargv[i]) + 1;
    uptr -= len;
    memcpy(kpage + (uptr - page_uva), kargv[i], len);
    str_uva[i] = uptr;
  }

  /* argv[] array of (argc + 1) user pointers, 16-byte aligned == the SP. */
  uptr &= ~(uint64_t)7;
  uptr -= (uint64_t)(argc + 1) * sizeof(uint64_t);
  uptr &= ~(uint64_t)15;
  uint64_t *uargv = (uint64_t *)(kpage + (uptr - page_uva));
  for (int i = 0; i < argc; i++)
    uargv[i] = str_uva[i];
  uargv[argc] = 0;

  *out_argv_uva = uptr;
  return uptr;
}

int process_load_elf(struct process *proc, const char *path) {
  return process_load_elf_args(proc, path, 0, NULL);
}

int process_load_elf_args(struct process *proc, const char *path, int argc,
                          char *const kargv[]) {
  if (argc < 0)
    argc = 0;
  if (argc > ELF_MAX_ARGS)
    argc = ELF_MAX_ARGS;
  struct vfs_node exe;
  if (vfs_open(path, &exe) != 0) {
    pr_err("ELF: File not found: %s\n", path);
    return -1;
  }

  /* 1. Read ELF Header */
  Elf64_Ehdr ehdr;
  if (vfs_read(&exe, 0, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
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
  uint64_t max_vaddr = 0;
  for (int i = 0; i < ehdr.e_phnum; i++) {
    Elf64_Phdr phdr;
    uint32_t ph_off = ehdr.e_phoff + (i * ehdr.e_phentsize);

    if (vfs_read(&exe, ph_off, &phdr, sizeof(phdr)) != sizeof(phdr)) {
      pr_err("ELF: Failed to read PHDR %d\n", i);
      return -1;
    }

    if (phdr.p_type == PT_LOAD) {
      /* ELF-01: reject a PT_LOAD whose VA is not wholly inside the user window
       * [0x70000000, 0xC0000000).  User ELFs link at 0x80000000 (Makefile
       * -Wl,-Ttext); their lowest segment is the header page at 0x7ffff000 and
       * the user stack base is 0xC0000000.  With the higher-half split the
       * kernel no longer lives in the low half at all, but the window check
       * stays: it rejects mis-linked binaries outright, keeps segments clear
       * of the stack area, and on amd64 prevents a crafted p_vaddr with high
       * bits set from indexing the COPIED kernel PML4 entries (256..511) and
       * writing through the shared kernel PDPTs. */
      if (phdr.p_vaddr < 0x70000000UL || phdr.p_vaddr >= 0xC0000000UL ||
          phdr.p_memsz > 0xC0000000UL - phdr.p_vaddr) {
        pr_err("ELF: PT_LOAD vaddr 0x%lx (memsz 0x%lx) outside user range "
               "[0x70000000,0xC0000000) - rejecting (mis-linked binary?)\n",
               phdr.p_vaddr, phdr.p_memsz);
        return -1;
      }

      /* User segment protection via the arch-neutral VMM page profiles
       * (HAL conformance #140 / DIR-06): the ELF loader is arch-neutral core
       * and must NOT hand-encode per-arch PTE bits. The VMM contract (vmm.h)
       * supplies the four W^X user profiles; the loader only maps ELF PF_W/PF_X
       * onto them. W^X (ELF-02): a segment is executable only when PF_X is set. */
      uint64_t flags;
      if (phdr.p_flags & PF_W)
        flags = (phdr.p_flags & PF_X) ? PAGE_USER : PAGE_USER_DATA;  /* RW+X : RW */
      else
        flags = (phdr.p_flags & PF_X) ? PAGE_USER_RX : PAGE_USER_RO; /* RO+X : RO */

      pr_debug("ELF: Mapping Segment at 0x%lx (FileSz: 0x%lx, MemSz: 0x%lx)\n",
              phdr.p_vaddr, phdr.p_filesz, phdr.p_memsz); /* hot path: demoted (perf §1) */

      /* Allocate Pages for Memory Segment */
      uint64_t start_vpage = phdr.p_vaddr & ~(0xFFF);
      uint64_t end_vpage = (phdr.p_vaddr + phdr.p_memsz + 4095) & ~(0xFFF);

      if (end_vpage > max_vaddr) {
        max_vaddr = end_vpage;
      }

      for (uint64_t vaddr = start_vpage; vaddr < end_vpage; vaddr += 4096) {
        /* Allocate physical page */
        void *paddr = pmm_alloc_page();
        if (!paddr) {
          pr_err("ELF: Failed to allocate physical page for vaddr 0x%lx\n",
                 vaddr);
          return -1;
        }

        /* Map page in process address space (PTE wants the frame's
         * PHYSICAL address; pmm returned the kernel pointer). */
        if (vmm_map_page(proc->page_table, vaddr, virt_to_phys(paddr),
                         flags) != 0) {
          pr_err("ELF: Failed to map page at 0x%lx\n", vaddr);
          pmm_free_page(paddr);
          return -1;
        }

        /* Kernel virtual address for writing (direct-map pointer) */
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

          vfs_read(&exe, offset_in_file, (uint8_t *)kaddr + offset_in_page,
                   copy_len);
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
  uint64_t stack_top = stack_base + stack_size;
  void *top_kaddr = NULL; /* kernel pointer to the highest stack page */

  for (uint64_t vaddr = stack_base; vaddr < stack_top; vaddr += 4096) {
    void *paddr = pmm_alloc_page();
    if (!paddr) {
      pr_err("%s", "ELF: Failed to allocate stack page\n");
      return -1;
    }
    /* PAGE_USER_DATA: the user stack is never executable (W^X, ELF-02). */
    if (vmm_map_page(proc->page_table, vaddr, virt_to_phys(paddr),
                     PAGE_USER_DATA) != 0) {
      pr_err("%s", "ELF: Failed to map stack page\n");
      pmm_free_page(paddr);
      return -1;
    }
    if (vaddr == stack_top - 4096)
      top_kaddr = paddr; /* argv block goes here (direct-map writable) */
  }

  proc->heap_start = max_vaddr;
  proc->heap_end = max_vaddr;

  proc->user_entry = ehdr.e_entry;
  proc->user_stack = stack_top;

  /* Marshal argc/argv onto the top stack page (no-op when argc == 0). */
  uint64_t argv_uva = 0;
  if (argc > 0 && top_kaddr) {
    proc->user_stack =
        setup_user_args(top_kaddr, stack_top, argc, kargv, &argv_uva);
  }

  /* Initialize Saved Context for Scheduler */
  /* proc->context already points to the top of the kernel stack (set in
   * process_create) */
  if (proc->context) {
    /* pr_info("ELF: Before init - PID %d context=%p ELR=0x%lx SP_EL0=0x%lx\n",
            proc->pid, (void*)proc->context, proc->context->elr,
       proc->context->sp_el0); */

    memset(proc->context, 0, sizeof(struct pt_regs));
    pt_regs_init_user_task(proc->context, proc->user_entry, proc->user_stack);
    /* main(argc, argv) — argv_uva is 0 (NULL) when no arguments were given. */
    pt_regs_set_user_args(proc->context, (uint64_t)argc, argv_uva);

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
