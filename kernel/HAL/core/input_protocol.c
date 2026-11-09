#include <hal/core.h>
#include <hal/input_protocol.h>
#include <stddef.h>
#include <stdint.h>

enum nxhal_input_state {
  NXHAL_INPUT_UNBOUND = 0,
  NXHAL_INPUT_BINDING = 1,
  NXHAL_INPUT_BOUND = 2
};

static struct nxhal_input_protocol_v1 input_protocol;
static uint32_t input_state;

static void nxhal_input_stop(void) __attribute__((noreturn));
static void nxhal_input_stop(void) {
  hal_irq_disable_all();
  hal_cpu_halt();
}

static int nxhal_input_protocol_valid(
    const struct nxhal_input_protocol_v1 *protocol) {
  if (protocol == NULL)
    return NXHAL_INPUT_BIND_E_NULL;
  if (protocol->abi_version != NXHAL_INPUT_PROTOCOL_ABI_VERSION)
    return NXHAL_INPUT_BIND_E_VERSION;
  if (protocol->struct_size != sizeof(struct nxhal_input_protocol_v1))
    return NXHAL_INPUT_BIND_E_SIZE;
  if (protocol->flags != 0U || protocol->reserved0 != 0U)
    return NXHAL_INPUT_BIND_E_RESERVED;
  if (protocol->rx_ready == NULL)
    return NXHAL_INPUT_BIND_E_MISSING;
  return NXHAL_INPUT_BIND_OK;
}

int nxhal_input_bind(const struct nxhal_input_protocol_v1 *protocol) {
  int valid = nxhal_input_protocol_valid(protocol);
  uint32_t expected = NXHAL_INPUT_UNBOUND;

  if (valid != NXHAL_INPUT_BIND_OK)
    return valid;
  if (!__atomic_compare_exchange_n(&input_state, &expected,
                                   NXHAL_INPUT_BINDING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return NXHAL_INPUT_BIND_E_STATE;

  input_protocol.abi_version = protocol->abi_version;
  input_protocol.struct_size = protocol->struct_size;
  input_protocol.flags = protocol->flags;
  input_protocol.reserved0 = protocol->reserved0;
  input_protocol.rx_ready = protocol->rx_ready;
  __atomic_store_n(&input_state, NXHAL_INPUT_BOUND, __ATOMIC_RELEASE);
  return NXHAL_INPUT_BIND_OK;
}

int nxhal_input_is_bound(void) {
  return __atomic_load_n(&input_state, __ATOMIC_ACQUIRE) == NXHAL_INPUT_BOUND;
}

void nxhal_input_rx_ready(void) {
  if (!nxhal_input_is_bound())
    nxhal_input_stop();
  input_protocol.rx_ready();
}
