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
 * The copies hold IRQs-masked + uaccess_active + current_process->mm_lock.
 * The faulting context is being discarded (process terminated), so the flag is
 * cleared and the lock dropped; schedule() returns a frame whose saved RFLAGS
 * govern the next task, so IRQ state needs no explicit restore here.
 *
 * HAL-0 (2026-07-23): mm_lock is NEW on this side.  Dropping it here is not
 * optional bookkeeping — a #PF inside a copy window would otherwise leave
 * mm_lock held by a process that is being torn down, and the next acquirer of
 * that lock would wait forever.  This mirrors aarch64's
 * arch_uaccess_fault_fixup exactly; the two providers now differ only where a
 * written contract says they may (aarch64 additionally restores TTBR0 — it
 * swaps address spaces, amd64 does not).
 */
void arch_uaccess_fault_fixup(void) {
  struct cpu_info *ci = arch_cpu_info_fault_safe();
  if (ci) {
    ci->uaccess_active = 0;
    if (ci->current_task)
      spin_unlock(&ci->current_task->mm_lock);
  }
}

/*
 * AMD64 uses a unified address space: the user PML4 is already in CR3 when
 * a syscall handler runs.  No TTBR switch (unlike aarch64) is needed.
 *
 * UACC-AMD64-01 (CLOSED 2026-07-23, HAL-0): the note recorded that a comment
 * claimed stac/clac SMAP bracketing that is not emitted.  Verified: no
 * stac/clac anywhere in this file and CR4.SMAP is not enabled, so there is no
 * SMAP protection to bracket — the claim was the defect, and the misleading
 * comment is gone.  Stated positively so nobody re-derives it: user memory is
 * reachable from kernel mode here because SMAP is OFF; the protection that
 * actually holds is the explicit vmm_is_user_addr + vmm_check_range validation
 * under mm_lock below.  Enabling CR4.SMAP (and then genuinely bracketing these
 * copies with stac/clac) is a hardening step in its own right, tracked with the
 * other amd64 CPU-feature work rather than pretended here.
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
 * UACC-AMD64-02 (FIXED 2026-07-23, HAL-0): the check and the copy now run
 * under current_process->mm_lock, closing the window in which a concurrent
 * unmap on another CPU could free a page between vmm_check_range() and the
 * memcpy.  aarch64 has always held mm_lock here; amd64 answering the same
 * concurrency question differently, in a security-sensitive primitive, was the
 * divergence HAL-0 exists to remove.  What legitimately still differs is the
 * TTBR0 swap (aarch64 splits address spaces, amd64 does not) — that is written
 * down in kernel/hal_uaccess.h rather than left implicit.
 *
 * Returns 0 on success, -1 on any validation failure.
 */
int arch_copy_from_user(void *dest, const void *src, size_t n) {
  uint64_t src_addr = (uint64_t)src;
  /* UACC-AMD64-05 (CLOSED 2026-07-23, HAL-0): the wrap check rejects an
   * overflowing src+n, and the range check below is applied to the LAST BYTE
   * ACTUALLY TOUCHED (src+n-1), not to the one-past-the-end address.  Checking
   * src+n was the recorded edge case: for a region ending exactly at the user
   * boundary the one-past address is the first non-user byte, so a perfectly
   * legal copy was refused; and n == 0 made src+n == src, checking a byte the
   * copy never reads.  n == 0 is now short-circuited (nothing to validate). */
  if (src_addr + n < src_addr) return -1;
  if (n == 0) return 0;
  if (!vmm_is_user_addr(src_addr) || !vmm_is_user_addr(src_addr + n - 1))
    return -1;
  if (!current_process || !current_process->page_table) return -1;

  /* uaccess window (Phase A step 9/10): flag the copy so the fault classifier
   * treats a #PF on a user VA here as recoverable (terminate the process)
   * rather than a kernel bug.  IRQs are masked so the flag cannot leak to a
   * different task via preemption; aarch64 already masks for its TTBR swap.
   *
   * mm_lock is taken BEFORE the range check so validation and use are one
   * critical section — checking first and locking after would leave exactly the
   * TOCTOU this fixes.  arch_uaccess_fault_fixup() drops it if the copy faults.
   */
  uint64_t uflags = local_irq_save();
  spin_lock(&current_process->mm_lock);

  if (vmm_check_range(current_process->page_table, src_addr, n, PTE_VALID) !=
      0) {
    spin_unlock(&current_process->mm_lock);
    local_irq_restore(uflags);
    return -1;
  }

  get_cpu_info()->uaccess_active = 1;

  memcpy(dest, src, n); /* NOTE(UACC-AMD64-01): no stac/clac bracketing */

  get_cpu_info()->uaccess_active = 0;
  spin_unlock(&current_process->mm_lock);
  local_irq_restore(uflags);

  return 0;
}

/*
 * arch_copy_to_user - copy 'n' bytes from kernel 'src' to user-space 'dest'.
 *
 * Same validation sequence as arch_copy_from_user with dest as the address.
 * UACC-AMD64-03 (FIXED 2026-07-23, HAL-0): the write-side mirror of
 * UACC-AMD64-02 — check and copy now share one mm_lock critical section, so a
 * concurrent unmap can no longer land this write in a newly-allocated kernel
 * page.
 *
 * Returns 0 on success, -1 on any validation failure.
 */
int arch_copy_to_user(void *dest, const void *src, size_t n) {
  uint64_t dest_addr = (uint64_t)dest;
  /* UACC-AMD64-05: last byte touched, not one-past-the-end.  See
   * arch_copy_from_user. */
  if (dest_addr + n < dest_addr) return -1;
  if (n == 0) return 0;
  if (!vmm_is_user_addr(dest_addr) || !vmm_is_user_addr(dest_addr + n - 1))
    return -1;
  if (!current_process || !current_process->page_table) return -1;

  /* uaccess window — see arch_copy_from_user */
  uint64_t uflags = local_irq_save();
  spin_lock(&current_process->mm_lock);

  if (vmm_check_range(current_process->page_table, dest_addr, n, PTE_VALID) !=
      0) {
    spin_unlock(&current_process->mm_lock);
    local_irq_restore(uflags);
    return -1;
  }

  get_cpu_info()->uaccess_active = 1;

  memcpy(dest, src, n); /* NOTE(UACC-AMD64-01): no stac/clac bracketing */

  get_cpu_info()->uaccess_active = 0;
  spin_unlock(&current_process->mm_lock);
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

  /* uaccess window — see arch_copy_from_user.  mm_lock spans the whole walk:
   * the string is validated page by page AS IT IS READ, so the lock must cover
   * every check/read pair, not just one (UACC-AMD64-02 for the string path). */
  uint64_t uflags = local_irq_save();
  spin_lock(&current_process->mm_lock);
  get_cpu_info()->uaccess_active = 1;

  size_t i;
  int ret = 0;

  /* UACC-AMD64-04 (FIXED 2026-07-23, HAL-0): validate the FIRST page before
   * reading a single byte.  The loop below only checks when the address is
   * page-ALIGNED, which correctly validates each new page at its first byte —
   * but the page containing src itself is never page-aligned unless src is, so
   * for any unaligned src the starting page went entirely unchecked (only
   * vmm_is_user_addr had looked at it).  A user could point src at an unmapped
   * page and the first read would fault inside the kernel. */
  if (vmm_check_range(current_process->page_table, (uint64_t)src, 1,
                      PTE_VALID) != 0) {
    ret = -1;
    goto out;
  }

  for (i = 0; i < max_len - 1; i++) {
    /* Per-page validation: each new page is checked at its first byte. */
    if (((uint64_t)&src[i] & 0xFFF) == 0) {
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
  if (out_len) *out_len = i;

out:
  dest[max_len - 1] = '\0';
  get_cpu_info()->uaccess_active = 0;
  spin_unlock(&current_process->mm_lock);
  local_irq_restore(uflags);

  return ret;
}

/* Legacy tolerant spelling — truncation is success.  See kernel/hal_uaccess.h. */
int arch_copy_string_from_user(char *dest, const char *src, size_t max_len) {
  return arch_copy_string_from_user_n(dest, src, max_len, (size_t *)0,
                                      (int *)0);
}
