#include <hal/timer_protocol.h>
#include <stdint.h>
#include <stdlib.h>

static unsigned arm_calls;
static unsigned tick_calls;

void hal_irq_disable_all(void) {
  abort();
}

void hal_cpu_halt(void) {
  abort();
}

static uint64_t test_arm_current(void) {
  ++arm_calls;
  return UINT64_C(0x12345678);
}

static uint64_t test_tick_current(void) {
  ++tick_calls;
  return UINT64_C(0xabcdef01);
}

static struct nxhal_timer_protocol_v1 valid_protocol(void) {
  struct nxhal_timer_protocol_v1 protocol = {
      .abi_version = NXHAL_TIMER_PROTOCOL_ABI_VERSION,
      .struct_size = sizeof(struct nxhal_timer_protocol_v1),
      .flags = 0U,
      .reserved0 = 0U,
      .arm_current = test_arm_current,
      .tick_current = test_tick_current,
  };
  return protocol;
}

int main(void) {
  struct nxhal_timer_protocol_v1 protocol = valid_protocol();

  if (nxhal_timer_bind(NULL) != NXHAL_TIMER_BIND_E_NULL)
    return 1;
  protocol.abi_version = 0U;
  if (nxhal_timer_bind(&protocol) != NXHAL_TIMER_BIND_E_VERSION)
    return 2;
  protocol = valid_protocol();
  protocol.struct_size -= 1U;
  if (nxhal_timer_bind(&protocol) != NXHAL_TIMER_BIND_E_SIZE)
    return 3;
  protocol = valid_protocol();
  protocol.flags = 1U;
  if (nxhal_timer_bind(&protocol) != NXHAL_TIMER_BIND_E_RESERVED)
    return 4;
  protocol = valid_protocol();
  protocol.arm_current = NULL;
  if (nxhal_timer_bind(&protocol) != NXHAL_TIMER_BIND_E_MISSING)
    return 5;
  if (nxhal_timer_is_bound())
    return 6;

  protocol = valid_protocol();
  if (nxhal_timer_bind(&protocol) != NXHAL_TIMER_BIND_OK)
    return 7;
  if (!nxhal_timer_is_bound())
    return 8;
  if (nxhal_timer_bind(&protocol) != NXHAL_TIMER_BIND_E_STATE)
    return 9;
  if (nxhal_timer_arm_current() != UINT64_C(0x12345678) || arm_calls != 1U)
    return 10;
  if (nxhal_timer_tick_current() != UINT64_C(0xabcdef01) || tick_calls != 1U)
    return 11;

  return 0;
}
