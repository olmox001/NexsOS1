/*
 * kernel/arch/amd64/mm/uaccess.c
 * Safe User Memory Access via SMAP/SMEP instructions (stac/clac)
 */
#include <kernel/types.h>
#include <kernel/vmm.h>
#include <kernel/string.h>
#include <kernel/sched.h>
#include <arch/arch.h>

/*
 * Note: Since AMD64 uses a unified address space (unlike AArch64 TTBR0/1),
 * if a process is running, its PML4 is already loaded in CR3.
 * We only need to bypass SMAP (stac) to access user pages from Ring 0.
 */

int arch_copy_from_user(void *dest, const void *src, size_t n) {
  uint64_t src_addr = (uint64_t)src;
  if (src_addr + n < src_addr) return -1;
  if (!vmm_is_user_addr(src_addr) || !vmm_is_user_addr(src_addr + n)) return -1;
  if (!current_process || !current_process->page_table) return -1;

  if (vmm_check_range(current_process->page_table, src_addr, n, PTE_VALID) != 0)
    return -1;

  memcpy(dest, src, n);

  return 0;
}

int arch_copy_to_user(void *dest, const void *src, size_t n) {
  uint64_t dest_addr = (uint64_t)dest;
  if (dest_addr + n < dest_addr) return -1;
  if (!vmm_is_user_addr(dest_addr) || !vmm_is_user_addr(dest_addr + n)) return -1;
  if (!current_process || !current_process->page_table) return -1;

  if (vmm_check_range(current_process->page_table, dest_addr, n, PTE_VALID) != 0)
    return -1;

  memcpy(dest, src, n);

  return 0;
}

int arch_copy_string_from_user(char *dest, const char *src, size_t max_len) {
  if (!vmm_is_user_addr((uint64_t)src)) return -1;
  if (!current_process || !current_process->page_table) return -1;

  size_t i;
  int ret = 0;
  for (i = 0; i < max_len - 1; i++) {
    if (((uint64_t)&src[i] & 0xFFF) == 0) {
       if (vmm_check_range(current_process->page_table, (uint64_t)&src[i], 1, PTE_VALID) != 0) {
         ret = -1;
         break;
       }
    }
    dest[i] = src[i];
    if (src[i] == '\0') break;
  }
  dest[max_len - 1] = '\0';

  return ret;
}
