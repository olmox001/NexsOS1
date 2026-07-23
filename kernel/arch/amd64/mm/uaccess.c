/*
 * kernel/arch/amd64/mm/uaccess.c
 * Kernel-to-User and User-to-Kernel Memory Copy Primitives (AMD64)
 *
 * Purpose:
 *   Provide arch_copy_from_user, arch_copy_to_user, and
 *   arch_copy_string_from_user.  These are called by syscall handlers that
 *   need to read or write user-space buffers.  On AMD64, the user PML4 is
 *   already loaded in CR3 (unified address space), so no TTBR switch is needed
 *   (unlike aarch64 which must swap TTBR0_EL1).  Validation is done via
 *   vmm_is_user_addr (range check) and vmm_check_range (page-table walk).
 *
 * Invariants:
 *   - current_process != NULL and current_process->page_table != NULL before
 *     any copy function is called (each function checks and returns -1).
 *   - Source/destination addresses must satisfy vmm_is_user_addr for every
 *     byte in the range.
 *
 * Known issues:
 *   UACC-AMD64-01 (W2 DOC/MISSING) The file header and the module-level
 *     comment below claim "stac/clac" SMAP protection.  No stac/clac
 *     instructions are emitted.  CR4.SMAP is NOT enabled in cpu.c:36-39 (only
 *     OSFXSR/OSXMMEXCPT are set).  Currently harmless because SMAP is off, but
 *     the comment creates a false security guarantee.  If SMAP is enabled in
 *     the future without adding stac/clac, every uaccess call traps.
 *   UACC-AMD64-02 (W3 SECURITY/TOCTOU) arch_copy_from_user: vmm_check_range
 *     runs at :23, memcpy runs at :26 — no lock between them.  On SMP, a
 *     concurrent munmap between the check and the copy allows the copy to read
 *     a freed or remapped page.  Fix: hold mm_lock + disable IRQs around both.
 *   UACC-AMD64-03 (W3 SECURITY/TOCTOU) arch_copy_to_user: same TOCTOU window
 *     at :37-41.
 *   UACC-AMD64-04 (W3 SECURITY) arch_copy_string_from_user: the first byte
 *     of 'src' is not validated via vmm_check_range before reading if it is
 *     not at a page boundary (the per-page check fires only at 0-mod-0x1000).
 *     A user can place src at the last byte of a valid page so the initial
 *     vmm_is_user_addr passes, then the string extends unmapped into the next
 *     page.  Also shares the TOCTOU window of UACC-AMD64-02.
 *   UACC-AMD64-05 (W2 BUG) Overflow check at :19 'src_addr + n < src_addr'
 *     does not catch src_addr + n == 0 (the exact canonical boundary); the
 *     vmm_is_user_addr(0) check catches the 0 case, but a range whose end
 *     equals exactly the top of user VA (0xFFFF000000000000) passes the
 *     overflow test and is only caught by the second vmm_is_user_addr call.
 *     Edge case; not immediately exploitable.
 */
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <kernel/string.h>
#include <kernel/sched.h>
#include <kernel/fault.h>
#include <arch/arch.h>

/*
 * arch_uaccess_fault_fixup - release the uaccess critical section after a
 * #PF inside one of the copy windows below (kernel/core/fault.c).
 *
 * On amd64 the copies hold only IRQs-masked + uaccess_active (unified address
 * space: no TTBR swap, no mm_lock).  The faulting context is being discarded
 * (process terminated) and schedule() returns a frame whose saved RFLAGS
 * govern the next task, so only the flag needs clearing.
 */
void arch_uaccess_fault_fixup(void) {
  struct cpu_info *ci = arch_cpu_info_fault_safe();
  if (ci)
    ci->uaccess_active = 0;
}

/*
 * AMD64 uses a unified address space: the user PML4 is already in CR3 when
 * a syscall handler runs.  No TTBR switch (unlike aarch64) is needed.
 * NOTE(UACC-AMD64-01): The original comment claims "we only need to bypass
 * SMAP (stac)" — but no stac/clac instructions are emitted here and CR4.SMAP
 * is not enabled in cpu.c.  The comment overstates the security protection.
 */

/*
 * arch_copy_from_user - copy 'n' bytes from user-space 'src' to kernel 'dest'.
 *
 * Validation steps (in order):
 *   1. Wrap-around check: src+n < src would indicate integer overflow.
 *   2. Range check: both src and src+n must be in the canonical user VA range
 *      (vmm_is_user_addr, typically [0x1000, 0xFFFF000000000000)).
 *      NOTE(UACC-AMD64-05): src+n == canonical boundary is an edge case.
 *   3. Null process/page_table guard.
 *   4. vmm_check_range: walks the page table to confirm all pages are present.
 *   5. memcpy: the actual copy.
 *
 * NOTE(UACC-AMD64-02): No lock is held between vmm_check_range (:23) and
 * memcpy (:26).  On SMP a concurrent munmap between these two steps allows
 * the memcpy to access a freed page.  The aarch64 equivalent holds mm_lock
 * and disables IRQs around both.
 *
 * Returns 0 on success, -1 on any validation failure.
 */
int arch_copy_from_user(void *dest, const void *src, size_t n) {
  uint64_t src_addr = (uint64_t)src;
  /* NOTE(UACC-AMD64-05): wrap-around check; does not catch src+n == boundary */
  if (src_addr + n < src_addr) return -1;
  if (!vmm_is_user_addr(src_addr) || !vmm_is_user_addr(src_addr + n)) return -1;
  if (!current_process || !current_process->page_table) return -1;

  /* NOTE(UACC-AMD64-02): TOCTOU window between this check and the memcpy below */
  if (vmm_check_range(current_process->page_table, src_addr, n, PTE_VALID) != 0)
    return -1;

  /* uaccess window (Phase A step 9/10): flag the copy so the fault classifier
   * treats a #PF on a user VA here as recoverable (terminate the process)
   * rather than a kernel bug.  IRQs are masked so the flag cannot leak to a
   * different task via preemption; aarch64 already masks for its TTBR swap. */
  uint64_t uflags = local_irq_save();
  get_cpu_info()->uaccess_active = 1;

  memcpy(dest, src, n); /* NOTE(UACC-AMD64-01): no stac/clac bracketing */

  get_cpu_info()->uaccess_active = 0;
  local_irq_restore(uflags);

  return 0;
}

/*
 * arch_copy_to_user - copy 'n' bytes from kernel 'src' to user-space 'dest'.
 *
 * Same validation sequence as arch_copy_from_user with dest as the address.
 * NOTE(UACC-AMD64-03): Same TOCTOU window as UACC-AMD64-02 — between the
 * vmm_check_range at :37 and the memcpy at :41.  A concurrent munmap by
 * another CPU or kernel thread can unmap dest between the check and the copy,
 * causing the write to land in a newly-allocated kernel page.
 *
 * Returns 0 on success, -1 on any validation failure.
 */
int arch_copy_to_user(void *dest, const void *src, size_t n) {
  uint64_t dest_addr = (uint64_t)dest;
  if (dest_addr + n < dest_addr) return -1;
  if (!vmm_is_user_addr(dest_addr) || !vmm_is_user_addr(dest_addr + n)) return -1;
  if (!current_process || !current_process->page_table) return -1;

  /* NOTE(UACC-AMD64-03): TOCTOU window between check and copy */
  if (vmm_check_range(current_process->page_table, dest_addr, n, PTE_VALID) != 0)
    return -1;

  /* uaccess window — see arch_copy_from_user */
  uint64_t uflags = local_irq_save();
  get_cpu_info()->uaccess_active = 1;

  memcpy(dest, src, n); /* NOTE(UACC-AMD64-01): no stac/clac bracketing */

  get_cpu_info()->uaccess_active = 0;
  local_irq_restore(uflags);

  return 0;
}

/*
 * arch_copy_string_from_user - copy a NUL-terminated string from user space.
 *
 * Copies up to max_len-1 characters from 'src' to 'dest', always NUL-terminates.
 * Per-page validation: vmm_check_range is called only when '&src[i]' crosses a
 * 4KB page boundary ((uint64_t)&src[i] & 0xFFF == 0).
 *
 * NOTE(UACC-AMD64-04): The very first byte is NOT individually validated by
 * vmm_check_range — only vmm_is_user_addr guards the starting address.  A user
 * can set src to the last byte of a valid page; the initial check passes, but
 * the next page (potentially unmapped) is read without a boundary check.  The
 * boundary check fires at page-aligned offsets i, not at the crossing point
 * (i.e., at i = 0, 4096, 8192, …) — so the crossing from page N to page N+1
 * is caught at the first i that is 0-mod-0x1000, which is the START of page
 * N+1, not the boundary.  For src at offset 1 of a page: i=4095 crosses the
 * boundary but is not 0-mod-0x1000 (4096 mod 4096 == 0 is the check for i=4095
 * pointing to &src[4095] = src+4095 ... actually at i=4095 addr=src+4095 mod
 * 0xFFF is != 0 if src is not page-aligned — so the check may not fire at the
 * crossing point).  Also has the same TOCTOU window as UACC-AMD64-02.
 *
 * Returns 0 on success, -1 if a page boundary check fails (string truncated).
 */
/*
 * arch_copy_string_from_user_n - HAL uaccess provider (kernel/hal_uaccess.h).
 * Mirror of the aarch64 provider; kept deliberately symmetric so the two
 * architectures cannot drift on semantics the way they did while this had no
 * written contract at all.
 */
int arch_copy_string_from_user_n(char *dest, const char *src, size_t max_len,
                                 size_t *out_len, int *out_truncated) {
  if (out_len) *out_len = 0;
  if (out_truncated) *out_truncated = 0;
  if (!vmm_is_user_addr((uint64_t)src)) return -1;
  if (!current_process || !current_process->page_table) return -1;
  /* max_len 0 would underflow `max_len - 1` below and run away; a zero-sized
   * destination cannot hold even the terminator, so refuse it outright. */
  if (max_len == 0)
    return -1;

  /* uaccess window — see arch_copy_from_user */
  uint64_t uflags = local_irq_save();
  get_cpu_info()->uaccess_active = 1;

  size_t i;
  int ret = 0;
  for (i = 0; i < max_len - 1; i++) {
    /* Per-page validation: check each page as we cross its start address.
     * NOTE(UACC-AMD64-04): first byte and the byte at crossing points between
     * non-zero offsets are not individually re-validated by vmm_check_range. */
    if (((uint64_t)&src[i] & 0xFFF) == 0) {
       /* NOTE(UACC-AMD64-02/04): also subject to TOCTOU between this check
        * and the read of src[i] below. */
       if (vmm_check_range(current_process->page_table, (uint64_t)&src[i], 1, PTE_VALID) != 0) {
         ret = -1;
         break;
       }
    }
    dest[i] = src[i];
    if (src[i] == '\0') break;
  }
  /* Ran the destination out before the terminator: the source is longer.
   * i == max_len - 1 only when the loop exhausted its bound rather than
   * breaking on the NUL. */
  if (ret == 0 && i == max_len - 1 && out_truncated)
    *out_truncated = 1;
  dest[max_len - 1] = '\0';
  if (out_len) *out_len = i;

  get_cpu_info()->uaccess_active = 0;
  local_irq_restore(uflags);

  return ret;
}

/* Legacy tolerant spelling — truncation is success.  See kernel/hal_uaccess.h. */
int arch_copy_string_from_user(char *dest, const char *src, size_t max_len) {
  return arch_copy_string_from_user_n(dest, src, max_len, (size_t *)0,
                                      (int *)0);
}
