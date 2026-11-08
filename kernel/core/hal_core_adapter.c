#include <hal/core.h>
#include <hal/core_protocol.h>
#include <hal/fault_protocol.h>
#include <hal/diagnostic_protocol.h>
#include <hal/timer_protocol.h>
#include <kernel/hal_core_adapter.h>
#include <kernel/fault.h>
#include <kernel/printk.h>
#include <kernel/irq.h>
#include <kernel/sched.h>
#include <drivers/timer.h>
#include <stdint.h>

extern hal_frame_t *kernel_syscall_dispatcher(hal_frame_t *frame);
extern hal_frame_t *kernel_timer_tick(hal_frame_t *frame);
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
  return kernel_timer_tick(frame);
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
  return timer_percpu_arm_current();
}

static uint64_t core_timer_account_current(void) {
  return timer_percpu_tick_current();
}

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

  kernel_main(boot_arg0, boot_arg1);
  hal_cpu_halt();
}
