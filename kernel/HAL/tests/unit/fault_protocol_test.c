#include <hal/fault_protocol.h>
#include <stdint.h>
#include <stdlib.h>

static unsigned enter_calls;
static unsigned exit_calls;
static unsigned handle_calls;
static unsigned regs_calls;
static unsigned scan_calls;
static hal_frame_t *expected_frame;

int32_t hal_cpu_id_fault_safe(void) { return 5; }

void hal_irq_disable_all(void) {
  abort();
}

void hal_cpu_halt(void) {
  abort();
}

static unsigned test_enter(void) {
  ++enter_calls;
  return 7U;
}

static void test_exit(void) {
  ++exit_calls;
}

static hal_frame_t *test_handle(hal_frame_t *frame, int user_mode,
                                uint64_t fault_addr, uint64_t fault_pc,
                                const char *description, uint64_t syndrome) {
  if (frame != expected_frame || user_mode != 1 ||
      fault_addr != UINT64_C(0x1234) || fault_pc != UINT64_C(0x5678) ||
      description == NULL || description[0] != 'f' ||
      syndrome != UINT64_C(0x9abc))
    return NULL;
  ++handle_calls;
  return frame;
}

static void test_backtrace_regs(uint64_t pc, uint64_t fp) {
  if (pc == UINT64_C(0x1111) && fp == UINT64_C(0x2222))
    ++regs_calls;
}

static void test_backtrace_scan(uint64_t sp) {
  if (sp == UINT64_C(0x3333))
    ++scan_calls;
}

static struct nxhal_fault_protocol_v1 valid_protocol(void) {
  struct nxhal_fault_protocol_v1 protocol = {
      .abi_version = NXHAL_FAULT_PROTOCOL_ABI_VERSION,
      .struct_size = sizeof(struct nxhal_fault_protocol_v1),
      .flags = 0U,
      .reserved0 = 0U,
      .enter = test_enter,
      .exit = test_exit,
      .handle = test_handle,
      .backtrace_regs = test_backtrace_regs,
      .backtrace_scan = test_backtrace_scan,
  };
  return protocol;
}

int main(void) {
  struct nxhal_fault_protocol_v1 protocol = valid_protocol();

  if (nxhal_fault_bind(NULL) != NXHAL_FAULT_BIND_E_NULL)
    return 1;

  protocol.abi_version = 0U;
  if (nxhal_fault_bind(&protocol) != NXHAL_FAULT_BIND_E_VERSION)
    return 2;

  protocol = valid_protocol();
  protocol.struct_size -= 1U;
  if (nxhal_fault_bind(&protocol) != NXHAL_FAULT_BIND_E_SIZE)
    return 3;

  protocol = valid_protocol();
  protocol.flags = 1U;
  if (nxhal_fault_bind(&protocol) != NXHAL_FAULT_BIND_E_RESERVED)
    return 4;

  protocol = valid_protocol();
  protocol.handle = NULL;
  if (nxhal_fault_bind(&protocol) != NXHAL_FAULT_BIND_E_MISSING)
    return 5;

  if (nxhal_fault_is_bound())
    return 6;

  protocol = valid_protocol();
  if (nxhal_fault_bind(&protocol) != NXHAL_FAULT_BIND_OK)
    return 7;
  if (!nxhal_fault_is_bound())
    return 8;
  if (nxhal_fault_bind(&protocol) != NXHAL_FAULT_BIND_E_STATE)
    return 9;

  expected_frame = (hal_frame_t *)(uintptr_t)UINT64_C(0x1000);
  if (nxhal_fault_enter() != 7U || enter_calls != 1U)
    return 10;

  nxhal_fault_exit();
  if (exit_calls != 1U)
    return 11;

  if (nxhal_fault_handle(expected_frame, 1, UINT64_C(0x1234),
                         UINT64_C(0x5678), "fault", UINT64_C(0x9abc)) !=
      expected_frame)
    return 12;
  if (handle_calls != 1U)
    return 13;

  nxhal_fault_backtrace_regs(UINT64_C(0x1111), UINT64_C(0x2222));
  nxhal_fault_backtrace_scan(UINT64_C(0x3333));
  if (regs_calls != 1U || scan_calls != 1U)
    return 14;

  return 0;
}
