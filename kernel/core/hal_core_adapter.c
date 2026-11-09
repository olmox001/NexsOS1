#include <hal/core.h>
#include <hal/core_protocol.h>
#include <hal/fault_protocol.h>
#include <hal/diagnostic_protocol.h>
#include <hal/timer_protocol.h>
#include <hal/smp_protocol.h>
#include <hal/device_protocol.h>
#include <hal/irq.h>
#include <hal/input_protocol.h>
#include <hal/fatal_protocol.h>
#include <hal/cpu_protocol.h>
#include <hal/memory_protocol.h>
#include <kernel/hal_core_adapter.h>
#include <kernel/fault.h>
#include <kernel/printk.h>
#include <kernel/irq.h>
#include <kernel/pmm.h>
#include <kernel/sched.h>
#include <kernel/hal.h>
#include <drivers/timer.h>
#include <drivers/keyboard.h>
#include <stddef.h>
#include <stdint.h>

extern hal_frame_t *kernel_syscall_dispatcher(hal_frame_t *frame);
extern void kernel_main(uint64_t boot_arg0, uint64_t boot_arg1);
extern void kernel_secondary_main(void);

static hal_frame_t *core_schedule(hal_frame_t *frame) {
  return schedule(frame);
}

static hal_frame_t *core_syscall(hal_frame_t *frame) {
  return kernel_syscall_dispatcher(frame);
}

static hal_frame_t *core_irq_entry(hal_frame_t *frame) {
  return irq_handler(frame);
}

static hal_frame_t *core_irq_dispatch(uint32_t irq, hal_frame_t *frame) {
  return irq_dispatch(irq, frame);
}

static hal_frame_t *core_timer_tick(hal_frame_t *frame) {
  return OS1libkernel_timer_tick(frame);
}

static void core_secondary_entry(void) {
  kernel_secondary_main();
}


static unsigned core_fault_enter(void) {
  return fault_enter();
}

static void core_fault_exit(void) {
  fault_exit();
}

static hal_frame_t *core_fault_handle(hal_frame_t *frame, int user_mode,
                                      uint64_t fault_addr, uint64_t fault_pc,
                                      const char *description,
                                      uint64_t syndrome) {
  return fault_handle_user_or_panic(frame, user_mode, fault_addr, fault_pc,
                                    description, syndrome);
}

static void core_fault_backtrace_regs(uint64_t pc, uint64_t fp) {
  backtrace_regs(pc, fp);
}

static void core_fault_backtrace_scan(uint64_t sp) {
  backtrace_scan(sp);
}


static const char *core_diag_prefix(uint32_t level) {
  switch (level) {
  case NXHAL_DIAG_EMERG:
    return "[EMERG] ";
  case NXHAL_DIAG_ALERT:
    return "[ALERT] ";
  case NXHAL_DIAG_CRIT:
    return "[CRIT] ";
  case NXHAL_DIAG_ERROR:
    return "[ERROR] ";
  case NXHAL_DIAG_WARNING:
    return "[WARN] ";
  case NXHAL_DIAG_NOTICE:
    return "[NOTICE] ";
  case NXHAL_DIAG_INFO:
    return "[INFO] ";
  case NXHAL_DIAG_DEBUG:
    return "[DEBUG] ";
  default:
    return "[HAL] ";
  }
}

static int core_diag_vlog(uint32_t level, const char *fmt, va_list args) {
  if (level > NXHAL_DIAG_DEBUG)
    level = NXHAL_DIAG_DEBUG;
  return vprintk_level((int)level, core_diag_prefix(level), fmt, args);
}

static void core_diag_vfault(const char *fmt, va_list args) {
  fault_vprintf(fmt, args);
}

static void core_diag_vpanic(const char *fmt, va_list args) {
  vpanic(fmt, args);
}

static const struct nxhal_diagnostic_protocol_v1 diagnostic_protocol = {
    .abi_version = NXHAL_DIAGNOSTIC_PROTOCOL_ABI_VERSION,
    .struct_size = sizeof(struct nxhal_diagnostic_protocol_v1),
    .flags = 0U,
    .reserved0 = 0U,
    .vlog = core_diag_vlog,
    .vfault = core_diag_vfault,
    .vpanic = core_diag_vpanic,
};


static uint64_t core_timer_arm_current(void) {
  return OS1libkernel_timer_percpu_arm();
}

static uint64_t core_timer_account_current(void) {
  return OS1libkernel_timer_percpu_tick();
}


static int core_smp_bringup_secondary(uint32_t cpu_id, void *stack_top) {
  return smp_bringup_secondary(cpu_id, stack_top);
}

static void core_device_copy_name(char dst[NXHAL_DEVICE_NAME_CAPACITY],
                                  const char src[NXHAL_DEVICE_NAME_CAPACITY]) {
  size_t index;
  for (index = 0U; index < NXHAL_DEVICE_NAME_CAPACITY; ++index)
    dst[index] = src[index];
  dst[NXHAL_DEVICE_NAME_CAPACITY - 1U] = '\0';
}

static void core_device_publish(const struct nxhal_device_desc *source) {
  struct hal_device device = {0};

  if (source == NULL)
    return;

  core_device_copy_name(device.name, source->name);
  device.bus_type = (hal_bus_type_t)source->bus_type;
  device.base = source->base;
  device.irq = source->irq;
  device.vendor_id = source->vendor_id;
  device.device_id = source->device_id;
  device.class_code = source->class_code;
  device.subclass = source->subclass;
  device.prog_if = source->prog_if;
  device.header_type = source->header_type;
  device.pci_bdf = source->pci_bdf;
  device.priv = NULL;
  device.driver = NULL;
  hal_register_device(&device);
}

static int core_device_get_count(void) {
  return hal_device_get_count();
}

static int core_device_get(int index, struct nxhal_device_desc *target) {
  struct hal_device *source;
  size_t name_index;

  if (target == NULL)
    return -1;

  source = hal_device_get(index);
  if (source == NULL)
    return -1;

  for (name_index = 0U; name_index < NXHAL_DEVICE_NAME_CAPACITY; ++name_index)
    target->name[name_index] = source->name[name_index];
  target->name[NXHAL_DEVICE_NAME_CAPACITY - 1U] = '\0';
  target->bus_type = (uint32_t)source->bus_type;
  target->base = source->base;
  target->irq = source->irq;
  target->vendor_id = source->vendor_id;
  target->device_id = source->device_id;
  target->class_code = source->class_code;
  target->subclass = source->subclass;
  target->prog_if = source->prog_if;
  target->header_type = source->header_type;
  target->pci_bdf = source->pci_bdf;
  return 0;
}

static int core_irq_register_handler(uint32_t irq, nxhal_irq_handler_t handler,
                                     void *data) {
  return irq_register(irq, handler, data);
}

static void core_irq_unregister_handler(uint32_t irq) {
  irq_unregister(irq);
}

static void core_input_rx_ready(void) {
  wake_up(&keyboard_wait_queue);
}

static hal_frame_t *core_fatal_serror(hal_frame_t *frame) {
  return serror_handler(frame);
}

static int core_cpu_online(uint32_t cpu_id) {
  uint32_t was_online;

  if (cpu_id >= MAX_CPUS)
    return -1;

  cpu_data[cpu_id].self = &cpu_data[cpu_id];
  cpu_data[cpu_id].cpu_id = cpu_id;
  was_online = __atomic_exchange_n(&cpu_data[cpu_id].online, 1U,
                                    __ATOMIC_ACQ_REL);
  if (was_online == 0U)
    (void)__sync_fetch_and_add(&nr_cpus, 1U);
  return 0;
}

static int core_cpu_is_online(uint32_t cpu_id) {
  if (cpu_id >= MAX_CPUS)
    return 0;
  return __atomic_load_n(&cpu_data[cpu_id].online, __ATOMIC_ACQUIRE) != 0U;
}

static void *core_memory_alloc_page(void) {
  return pmm_alloc_page();
}

static void *core_memory_alloc_pages(size_t count) {
  return pmm_alloc_pages(count);
}

static void core_memory_free_page(void *page) {
  pmm_free_page(page);
}

static void core_memory_free_pages(void *page, size_t count) {
  pmm_free_pages(page, count);
}

static const struct nxhal_memory_protocol_v1 memory_protocol = {
    .abi_version = NXHAL_MEMORY_PROTOCOL_ABI_VERSION,
    .struct_size = sizeof(struct nxhal_memory_protocol_v1),
    .flags = 0U,
    .reserved0 = 0U,
    .alloc_page = core_memory_alloc_page,
    .alloc_pages = core_memory_alloc_pages,
    .free_page = core_memory_free_page,
    .free_pages = core_memory_free_pages,
};

static const struct nxhal_cpu_protocol_v1 cpu_protocol = {
    .abi_version = NXHAL_CPU_PROTOCOL_ABI_VERSION,
    .struct_size = sizeof(struct nxhal_cpu_protocol_v1),
    .flags = 0U,
    .reserved0 = 0U,
    .online = core_cpu_online,
    .is_online = core_cpu_is_online,
};

static const struct nxhal_fatal_protocol_v1 fatal_protocol = {
    .abi_version = NXHAL_FATAL_PROTOCOL_ABI_VERSION,
    .struct_size = sizeof(struct nxhal_fatal_protocol_v1),
    .flags = 0U,
    .reserved0 = 0U,
    .serror = core_fatal_serror,
};

static const struct nxhal_input_protocol_v1 input_protocol = {
    .abi_version = NXHAL_INPUT_PROTOCOL_ABI_VERSION,
    .struct_size = sizeof(struct nxhal_input_protocol_v1),
    .flags = 0U,
    .reserved0 = 0U,
    .rx_ready = core_input_rx_ready,
};

static const struct nxhal_irq_core_protocol_v1 irq_core_protocol = {
    .abi_version = NXHAL_IRQ_CORE_PROTOCOL_ABI_VERSION,
    .struct_size = sizeof(struct nxhal_irq_core_protocol_v1),
    .flags = 0U,
    .reserved0 = 0U,
    .register_handler = core_irq_register_handler,
    .unregister_handler = core_irq_unregister_handler,
};

static const struct nxhal_device_protocol_v1 device_protocol = {
    .abi_version = NXHAL_DEVICE_PROTOCOL_ABI_VERSION,
    .struct_size = sizeof(struct nxhal_device_protocol_v1),
    .flags = 0U,
    .reserved0 = 0U,
    .publish = core_device_publish,
    .get_count = core_device_get_count,
    .get = core_device_get,
};

static const struct nxhal_smp_protocol_v1 smp_protocol = {
    .abi_version = NXHAL_SMP_PROTOCOL_ABI_VERSION,
    .struct_size = sizeof(struct nxhal_smp_protocol_v1),
    .flags = 0U,
    .reserved0 = 0U,
    .bringup_secondary = core_smp_bringup_secondary,
};

static const struct nxhal_timer_protocol_v1 timer_protocol = {
    .abi_version = NXHAL_TIMER_PROTOCOL_ABI_VERSION,
    .struct_size = sizeof(struct nxhal_timer_protocol_v1),
    .flags = 0U,
    .reserved0 = 0U,
    .arm_current = core_timer_arm_current,
    .tick_current = core_timer_account_current,
};

static const struct nxhal_fault_protocol_v1 fault_protocol = {
    .abi_version = NXHAL_FAULT_PROTOCOL_ABI_VERSION,
    .struct_size = sizeof(struct nxhal_fault_protocol_v1),
    .flags = 0U,
    .reserved0 = 0U,
    .enter = core_fault_enter,
    .exit = core_fault_exit,
    .handle = core_fault_handle,
    .backtrace_regs = core_fault_backtrace_regs,
    .backtrace_scan = core_fault_backtrace_scan,
};

static const struct nxhal_core_protocol_v1 core_protocol = {
    .abi_version = NXHAL_CORE_PROTOCOL_ABI_VERSION,
    .struct_size = sizeof(struct nxhal_core_protocol_v1),
    .flags = 0U,
    .reserved0 = 0U,
    .schedule = core_schedule,
    .syscall = core_syscall,
    .irq_entry = core_irq_entry,
    .irq_dispatch = core_irq_dispatch,
    .timer_tick = core_timer_tick,
    .secondary_entry = core_secondary_entry,
};

void nxcore_hal_boot_entry(uint64_t boot_arg0, uint64_t boot_arg1) {
  if (nxhal_core_bind(&core_protocol) != NXHAL_CORE_BIND_OK)
    hal_cpu_halt();
  if (nxhal_fault_bind(&fault_protocol) != NXHAL_FAULT_BIND_OK)
    hal_cpu_halt();
  if (nxhal_diagnostic_bind(&diagnostic_protocol) != NXHAL_DIAGNOSTIC_BIND_OK)
    hal_cpu_halt();
  if (nxhal_timer_bind(&timer_protocol) != NXHAL_TIMER_BIND_OK)
    hal_cpu_halt();
  if (nxhal_smp_bind(&smp_protocol) != NXHAL_SMP_BIND_OK)
    hal_cpu_halt();
  if (nxhal_device_bind(&device_protocol) != NXHAL_DEVICE_BIND_OK)
    hal_cpu_halt();
  if (nxhal_irq_core_bind(&irq_core_protocol) != NXHAL_IRQ_CORE_BIND_OK)
    hal_cpu_halt();
  if (nxhal_input_bind(&input_protocol) != NXHAL_INPUT_BIND_OK)
    hal_cpu_halt();
  if (nxhal_fatal_bind(&fatal_protocol) != NXHAL_FATAL_BIND_OK)
    hal_cpu_halt();
  if (nxhal_cpu_bind(&cpu_protocol) != NXHAL_CPU_BIND_OK)
    hal_cpu_halt();
  if (nxhal_memory_bind(&memory_protocol) != NXHAL_MEMORY_BIND_OK)
    hal_cpu_halt();

  kernel_main(boot_arg0, boot_arg1);
  hal_cpu_halt();
}
