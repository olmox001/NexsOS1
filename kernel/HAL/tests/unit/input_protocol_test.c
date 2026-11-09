#include <hal/input_protocol.h>
#include <stdlib.h>

static unsigned ready_calls;

void hal_irq_disable_all(void) { abort(); }
void hal_cpu_halt(void) { abort(); }

static void test_ready(void) { ++ready_calls; }

static struct nxhal_input_protocol_v1 valid_protocol(void) {
  struct nxhal_input_protocol_v1 protocol = {
      .abi_version = NXHAL_INPUT_PROTOCOL_ABI_VERSION,
      .struct_size = sizeof(struct nxhal_input_protocol_v1),
      .flags = 0U,
      .reserved0 = 0U,
      .rx_ready = test_ready,
  };
  return protocol;
}

int main(void) {
  struct nxhal_input_protocol_v1 protocol = valid_protocol();

  if (nxhal_input_bind(NULL) != NXHAL_INPUT_BIND_E_NULL)
    return 1;
  protocol.abi_version = 0U;
  if (nxhal_input_bind(&protocol) != NXHAL_INPUT_BIND_E_VERSION)
    return 2;
  protocol = valid_protocol();
  protocol.struct_size -= 1U;
  if (nxhal_input_bind(&protocol) != NXHAL_INPUT_BIND_E_SIZE)
    return 3;
  protocol = valid_protocol();
  protocol.flags = 1U;
  if (nxhal_input_bind(&protocol) != NXHAL_INPUT_BIND_E_RESERVED)
    return 4;
  protocol = valid_protocol();
  protocol.rx_ready = NULL;
  if (nxhal_input_bind(&protocol) != NXHAL_INPUT_BIND_E_MISSING)
    return 5;

  protocol = valid_protocol();
  if (nxhal_input_bind(&protocol) != NXHAL_INPUT_BIND_OK)
    return 6;
  if (!nxhal_input_is_bound())
    return 7;
  if (nxhal_input_bind(&protocol) != NXHAL_INPUT_BIND_E_STATE)
    return 8;

  nxhal_input_rx_ready();
  nxhal_input_rx_ready();
  if (ready_calls != 2U)
    return 9;
  return 0;
}
