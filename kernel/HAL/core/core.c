#include <hal/core.h>
#include <hal/config.h>
#include <hal/provider.h>
#include <arch/arch.h>

/*
 * The only translation point between the public HAL language and the selected
 * provider's ISA primitives.  NX-CORE never includes arch/arch.h.
 */

uint32_t hal_cpu_id(void) { return arch_impl_get_cpu_id(); }
int32_t hal_cpu_id_fault_safe(void) { return nxhal_provider_cpu_id_fault_safe(); }
void hal_cpu_local_init(void) { nxhal_provider_cpu_local_init(); }
void hal_cpu_nop(void) { arch_impl_nop(); }
void hal_cpu_idle(void) { arch_impl_idle(); }
void hal_cpu_yield(void) { arch_impl_yield(); }
void hal_cpu_notify(void) { arch_impl_cpu_notify(); }
void hal_bus_scan(void) { nxhal_provider_bus_scan(); }
void hal_irq_controller_init(void) { nxhal_provider_irq_init(); }
void hal_timer_global_init(void) { nxhal_provider_timer_global_init(); }
void hal_timer_percpu_init(void) { nxhal_provider_timer_percpu_init(); }
int hal_vmm_map_device(uint64_t base, uint64_t size) {
  return nxhal_provider_vmm_map_device(base, size);
}
int hal_smp_wake_secondary(uint64_t cpu_id, void *stack_top) {
  return arch_impl_smp_wake_secondary(cpu_id, stack_top);
}

void hal_cpu_halt(void) {
  arch_impl_halt();
}

void hal_reboot(void) {
  arch_impl_reboot();
  arch_impl_halt();
}

void hal_irq_enable(void) { arch_impl_irq_enable(); }
void hal_irq_disable(void) { arch_impl_irq_disable(); }

uint64_t hal_irq_save_val(void) {
  uint64_t flags = 0;
  arch_impl_irq_save(&flags);
  return flags;
}

void hal_irq_save(uint64_t *flags) { arch_impl_irq_save(flags); }
void hal_irq_restore(uint64_t flags) { arch_impl_irq_restore(flags); }
void hal_irq_save_all(uint64_t *flags) { arch_impl_irq_save_all(flags); }
void hal_irq_restore_all(uint64_t flags) { arch_impl_irq_restore_all(flags); }
void hal_irq_disable_all(void) { arch_impl_irq_disable_all(); }

void hal_isb(void) { arch_impl_isb(); }
void hal_mb(void) { arch_impl_mb(); }
void hal_rmb(void) { arch_impl_rmb(); }
void hal_wmb(void) { arch_impl_wmb(); }

int hal_phys_range_valid(uint64_t base, uint64_t end) {
  if (end < base || end > HAL_PHYS_MAP_LIMIT)
    return 0;
#if HAL_RAM_START != 0
  if (base < HAL_RAM_START)
    return 0;
#else
  (void)base;
#endif
  return 1;
}

static int hal_mmio_pointer(uint64_t address, size_t width,
                            volatile void **out_pointer) {
  uint64_t virtual_address;

  if (out_pointer == NULL || width == 0U ||
      address > UINT64_MAX - (uint64_t)width ||
      address > UINT64_MAX - (uint64_t)HAL_DIRECT_MAP_BASE ||
      (address % (uint64_t)width) != 0U)
    return -1;
  virtual_address = address + (uint64_t)HAL_DIRECT_MAP_BASE;
  *out_pointer = (volatile void *)(uintptr_t)virtual_address;
  return 0;
}

int hal_mmio_read8(uint64_t address, uint8_t *out_value) {
  volatile void *pointer;

  if (out_value == NULL || hal_mmio_pointer(address, 1U, &pointer) != 0)
    return -1;
  *out_value = *(volatile uint8_t *)pointer;
  hal_rmb();
  return 0;
}

int hal_mmio_read16(uint64_t address, uint16_t *out_value) {
  volatile void *pointer;

  if (out_value == NULL || hal_mmio_pointer(address, 2U, &pointer) != 0)
    return -1;
  *out_value = *(volatile uint16_t *)pointer;
  hal_rmb();
  return 0;
}

int hal_mmio_read32(uint64_t address, uint32_t *out_value) {
  volatile void *pointer;

  if (out_value == NULL || hal_mmio_pointer(address, 4U, &pointer) != 0)
    return -1;
  *out_value = *(volatile uint32_t *)pointer;
  hal_rmb();
  return 0;
}

int hal_mmio_write8(uint64_t address, uint8_t value) {
  volatile void *pointer;

  if (hal_mmio_pointer(address, 1U, &pointer) != 0)
    return -1;
  hal_wmb();
  *(volatile uint8_t *)pointer = value;
  hal_wmb();
  return 0;
}

int hal_mmio_write16(uint64_t address, uint16_t value) {
  volatile void *pointer;

  if (hal_mmio_pointer(address, 2U, &pointer) != 0)
    return -1;
  hal_wmb();
  *(volatile uint16_t *)pointer = value;
  hal_wmb();
  return 0;
}

int hal_mmio_write32(uint64_t address, uint32_t value) {
  volatile void *pointer;

  if (hal_mmio_pointer(address, 4U, &pointer) != 0)
    return -1;
  hal_wmb();
  *(volatile uint32_t *)pointer = value;
  hal_wmb();
  return 0;
}

static uint64_t nxhal_kernel_root_phys;

void hal_vmm_set_pgd(uint64_t pgd) { arch_impl_set_pgd(pgd); }
uint64_t hal_vmm_get_pgd(void) { return arch_impl_get_pgd(); }
void hal_vmm_set_kernel_pgd(uint64_t pgd) {
  arch_impl_set_kernel_pgd(pgd);
  __atomic_store_n(&nxhal_kernel_root_phys, pgd, __ATOMIC_RELEASE);
}
uint64_t hal_vmm_get_kernel_pgd(void) {
  uint64_t root = __atomic_load_n(&nxhal_kernel_root_phys, __ATOMIC_ACQUIRE);
  return root != 0U ? root : arch_impl_get_kernel_pgd();
}

void hal_tlb_flush_local(void) { arch_impl_tlb_flush_local(); }
void hal_tlb_flush_all(void) { arch_impl_tlb_flush_all(); }
void hal_tlb_flush_va(uintptr_t va) { arch_impl_tlb_flush_va(va); }
void hal_tlb_shootdown_va(uintptr_t va) { arch_impl_tlb_shootdown_va(va); }
void hal_tlb_shootdown_all(void) { arch_impl_tlb_shootdown_all(); }

void hal_cache_clean(void *va, size_t size) {
  arch_impl_cache_clean_range(va, size);
}

void hal_cache_invalidate(void *va, size_t size) {
  arch_impl_cache_invalidate_range(va, size);
}

void hal_cache_sync_icache(void *va, size_t size) {
  arch_impl_cache_sync_icache(va, size);
}

uint64_t hal_fault_address(void) { return arch_impl_get_fault_address(); }
uint64_t hal_fault_status(void) { return arch_impl_get_fault_status(); }
int hal_hw_random(uint64_t *out) { return arch_impl_hw_random(out); }

uint64_t hal_timer_get_freq(void) { return arch_impl_timer_get_freq(); }
uint64_t hal_timer_get_count(void) { return arch_impl_timer_get_count(); }
void hal_timer_set_compare(uint64_t value) {
  arch_impl_timer_set_compare(value);
}
void hal_timer_control(uint32_t value) { arch_impl_timer_control(value); }

void hal_spin_lock(volatile uint32_t *lock) { arch_impl_spin_lock(lock); }
void hal_spin_unlock(volatile uint32_t *lock) { arch_impl_spin_unlock(lock); }
int hal_spin_trylock(volatile uint32_t *lock) {
  return arch_impl_spin_trylock(lock);
}

#ifdef ARCH_AMD64
int hal_ioport_available(void) { return 1; }
uint8_t hal_inb(uint16_t port) { return inb(port); }
uint16_t hal_inw(uint16_t port) { return inw(port); }
uint32_t hal_inl(uint16_t port) { return inl(port); }
void hal_outb(uint16_t port, uint8_t value) { outb(port, value); }
void hal_outw(uint16_t port, uint16_t value) { outw(port, value); }
void hal_outl(uint16_t port, uint32_t value) { outl(port, value); }
#else
int hal_ioport_available(void) { return 0; }
uint8_t hal_inb(uint16_t port) { (void)port; return 0; }
uint16_t hal_inw(uint16_t port) { (void)port; return 0; }
uint32_t hal_inl(uint16_t port) { (void)port; return 0; }
void hal_outb(uint16_t port, uint8_t value) { (void)port; (void)value; }
void hal_outw(uint16_t port, uint16_t value) { (void)port; (void)value; }
void hal_outl(uint16_t port, uint32_t value) { (void)port; (void)value; }
#endif
