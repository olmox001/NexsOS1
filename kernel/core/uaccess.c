#include <hal/uaccess.h>
#include <kernel/cpu.h>
#include <kernel/fault.h>
#include <kernel/hal_uaccess.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/vmm.h>
#include <stddef.h>
#include <stdint.h>

struct uaccess_guard {
  struct process *process;
  struct cpu_info *cpu;
  uint64_t irq_flags;
  struct nxhal_uaccess_hw_state hw;
};

static int uaccess_range_valid(uint64_t address, size_t size) {
  if (size == 0U)
    return 1;
  if (address + size < address)
    return 0;
  return vmm_is_user_addr(address) &&
         vmm_is_user_addr(address + size - 1U);
}

static int uaccess_begin(struct uaccess_guard *guard, uint64_t address,
                         size_t size) {
  struct process *process;

  if (guard == NULL || !uaccess_range_valid(address, size))
    return -1;

  guard->irq_flags = local_irq_save();
  process = current_process;
  if (process == NULL || process->page_table == NULL) {
    local_irq_restore(guard->irq_flags);
    return -1;
  }

  spin_lock(&process->mm_lock);
  if (vmm_check_range(process->page_table, address, size, PTE_VALID) != 0) {
    spin_unlock(&process->mm_lock);
    local_irq_restore(guard->irq_flags);
    return -1;
  }

  guard->process = process;
  guard->cpu = get_cpu_info();
  hal_uaccess_enter(virt_to_phys(process->page_table), &guard->hw);
  guard->cpu->uaccess_active = 1U;
  return 0;
}

static void uaccess_end(struct uaccess_guard *guard) {
  guard->cpu->uaccess_active = 0U;
  hal_uaccess_exit(&guard->hw);
  spin_unlock(&guard->process->mm_lock);
  local_irq_restore(guard->irq_flags);
}

int hal_copy_from_user(void *dest, const void *src, size_t size) {
  struct uaccess_guard guard;
  uint64_t address = (uint64_t)(uintptr_t)src;

  if (size == 0U)
    return 0;
  if (dest == NULL || uaccess_begin(&guard, address, size) != 0)
    return -1;

  memcpy(dest, src, size);
  uaccess_end(&guard);
  return 0;
}

int hal_copy_to_user(void *dest, const void *src, size_t size) {
  struct uaccess_guard guard;
  uint64_t address = (uint64_t)(uintptr_t)dest;

  if (size == 0U)
    return 0;
  if (src == NULL || uaccess_begin(&guard, address, size) != 0)
    return -1;

  memcpy(dest, src, size);
  uaccess_end(&guard);
  return 0;
}

int hal_copy_string_from_user_n(char *dest, const char *src, size_t max_len,
                                size_t *out_len, int *out_truncated) {
  struct uaccess_guard guard;
  struct process *process;
  uint64_t address;
  size_t index = 0U;
  int result = 0;

  if (out_len != NULL)
    *out_len = 0U;
  if (out_truncated != NULL)
    *out_truncated = 0;
  if (dest == NULL || src == NULL || max_len == 0U)
    return -1;

  address = (uint64_t)(uintptr_t)src;
  if (!vmm_is_user_addr(address))
    return -1;

  guard.irq_flags = local_irq_save();
  process = current_process;
  if (process == NULL || process->page_table == NULL) {
    local_irq_restore(guard.irq_flags);
    return -1;
  }

  spin_lock(&process->mm_lock);
  guard.process = process;
  guard.cpu = get_cpu_info();
  hal_uaccess_enter(virt_to_phys(process->page_table), &guard.hw);
  guard.cpu->uaccess_active = 1U;

  for (index = 0U; index < max_len - 1U; ++index) {
    uint64_t byte_address = (uint64_t)(uintptr_t)&src[index];

    if (index == 0U || (byte_address & UINT64_C(0xfff)) == 0U) {
      if (vmm_check_range(process->page_table, byte_address, 1U,
                          PTE_VALID) != 0) {
        result = -1;
        break;
      }
    }

    dest[index] = src[index];
    if (src[index] == '\0')
      break;
  }

  if (result == 0 && index == max_len - 1U && out_truncated != NULL)
    *out_truncated = 1;
  if (out_len != NULL)
    *out_len = index;

  dest[max_len - 1U] = '\0';
  uaccess_end(&guard);
  return result;
}

int hal_copy_string_from_user(char *dest, const char *src, size_t max_len) {
  return hal_copy_string_from_user_n(dest, src, max_len, NULL, NULL);
}

void uaccess_fault_fixup(void) {
  struct cpu_info *cpu = cpu_info_fault_safe();

  if (cpu != NULL) {
    cpu->uaccess_active = 0U;
    if (cpu->current_task != NULL)
      spin_unlock(&cpu->current_task->mm_lock);
  }

  hal_uaccess_fault_abort();
}
