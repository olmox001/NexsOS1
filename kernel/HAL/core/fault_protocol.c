#include <hal/core.h>
#include <hal/fault_protocol.h>
#include <hal/frame.h>
#include <hal/nxpax.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

enum nxhal_fault_protocol_state {
  NXHAL_FAULT_STATE_UNBOUND = 0,
  NXHAL_FAULT_STATE_BINDING = 1,
  NXHAL_FAULT_STATE_BOUND = 2
};

static struct nxhal_fault_protocol_v1 fault_protocol;
static uint32_t fault_protocol_state;

static void nxhal_fault_protocol_stop(void) __attribute__((noreturn));
static void nxhal_fault_protocol_stop(void) {
  hal_irq_disable_all();
  hal_cpu_halt();
}

static int nxhal_fault_protocol_valid(
    const struct nxhal_fault_protocol_v1 *protocol) {
  if (protocol == NULL)
    return NXHAL_FAULT_BIND_E_NULL;
  if (protocol->abi_version != NXHAL_FAULT_PROTOCOL_ABI_VERSION)
    return NXHAL_FAULT_BIND_E_VERSION;
  if (protocol->struct_size != sizeof(struct nxhal_fault_protocol_v1))
    return NXHAL_FAULT_BIND_E_SIZE;
  if (protocol->flags != 0U || protocol->reserved0 != 0U)
    return NXHAL_FAULT_BIND_E_RESERVED;
  if (protocol->enter == NULL || protocol->exit == NULL ||
      protocol->handle == NULL || protocol->backtrace_regs == NULL ||
      protocol->backtrace_scan == NULL)
    return NXHAL_FAULT_BIND_E_MISSING;
  return NXHAL_FAULT_BIND_OK;
}

int nxhal_fault_bind(const struct nxhal_fault_protocol_v1 *protocol) {
  int valid = nxhal_fault_protocol_valid(protocol);
  uint32_t expected = NXHAL_FAULT_STATE_UNBOUND;

  if (valid != NXHAL_FAULT_BIND_OK)
    return valid;
  if (!__atomic_compare_exchange_n(&fault_protocol_state, &expected,
                                   NXHAL_FAULT_STATE_BINDING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return NXHAL_FAULT_BIND_E_STATE;

  fault_protocol.abi_version = protocol->abi_version;
  fault_protocol.struct_size = protocol->struct_size;
  fault_protocol.flags = protocol->flags;
  fault_protocol.reserved0 = protocol->reserved0;
  fault_protocol.enter = protocol->enter;
  fault_protocol.exit = protocol->exit;
  fault_protocol.handle = protocol->handle;
  fault_protocol.backtrace_regs = protocol->backtrace_regs;
  fault_protocol.backtrace_scan = protocol->backtrace_scan;

  __atomic_store_n(&fault_protocol_state, NXHAL_FAULT_STATE_BOUND,
                   __ATOMIC_RELEASE);
  return NXHAL_FAULT_BIND_OK;
}

int nxhal_fault_is_bound(void) {
  return __atomic_load_n(&fault_protocol_state, __ATOMIC_ACQUIRE) ==
         NXHAL_FAULT_STATE_BOUND;
}

static const struct nxhal_fault_protocol_v1 *nxhal_fault_protocol(void) {
  if (!nxhal_fault_is_bound())
    nxhal_fault_protocol_stop();
  return &fault_protocol;
}

unsigned nxhal_fault_enter(void) {
  return nxhal_fault_protocol()->enter();
}

void nxhal_fault_exit(void) {
  nxhal_fault_protocol()->exit();
}

hal_frame_t *nxhal_fault_handle(hal_frame_t *frame, int user_mode,
                                uint64_t fault_addr, uint64_t fault_pc,
                                const char *description, uint64_t syndrome) {
  nxhal_nxpax_fault(fault_pc, fault_addr, syndrome, user_mode,
                   hal_cpu_id_fault_safe());
  return nxhal_fault_protocol()->handle(frame, user_mode, fault_addr, fault_pc,
                                        description, syndrome);
}

void nxhal_fault_backtrace_regs(uint64_t pc, uint64_t fp) {
  nxhal_fault_protocol()->backtrace_regs(pc, fp);
}

void nxhal_fault_backtrace_scan(uint64_t sp) {
  nxhal_fault_protocol()->backtrace_scan(sp);
}


static struct nxhal_fault_output_protocol_v1 fault_output_protocol;
static uint32_t fault_output_state;

int nxhal_fault_output_bind(const struct nxhal_fault_output_protocol_v1 *protocol) {
  if (protocol == NULL)
    return NXHAL_FAULT_OUTPUT_BIND_E_NULL;
  if (protocol->abi_version != NXHAL_FAULT_OUTPUT_ABI_VERSION)
    return NXHAL_FAULT_OUTPUT_BIND_E_VERSION;
  if (protocol->struct_size != sizeof(struct nxhal_fault_output_protocol_v1))
    return NXHAL_FAULT_OUTPUT_BIND_E_SIZE;
  if (protocol->flags != 0U || protocol->reserved0 != 0U)
    return NXHAL_FAULT_OUTPUT_BIND_E_RESERVED;
  if (protocol->vprint == NULL || protocol->vpanic == NULL)
    return NXHAL_FAULT_OUTPUT_BIND_E_MISSING;
  uint32_t expected = NXHAL_FAULT_STATE_UNBOUND;
  if (!__atomic_compare_exchange_n(&fault_output_state, &expected,
                                   NXHAL_FAULT_STATE_BINDING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return NXHAL_FAULT_OUTPUT_BIND_E_STATE;
  fault_output_protocol = *protocol;
  __atomic_store_n(&fault_output_state, NXHAL_FAULT_STATE_BOUND,
                   __ATOMIC_RELEASE);
  return NXHAL_FAULT_OUTPUT_BIND_OK;
}

int nxhal_fault_output_is_bound(void) {
  return __atomic_load_n(&fault_output_state, __ATOMIC_ACQUIRE) ==
         NXHAL_FAULT_STATE_BOUND;
}

void nxhal_fault_print(const char *fmt, ...) {
  va_list args;
  if (!nxhal_fault_output_is_bound())
    nxhal_fault_protocol_stop();
  va_start(args, fmt);
  fault_output_protocol.vprint(fmt, args);
  va_end(args);
}

void nxhal_fault_panic(const char *fmt, ...) {
  va_list args;
  if (!nxhal_fault_output_is_bound())
    nxhal_fault_protocol_stop();
  va_start(args, fmt);
  fault_output_protocol.vpanic(fmt, args);
  va_end(args);
  nxhal_fault_protocol_stop();
}
