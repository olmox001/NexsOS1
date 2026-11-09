#include <hal/core.h>
#include <hal/device_protocol.h>

#include <stddef.h>
#include <stdint.h>

enum nxhal_device_protocol_state {
  NXHAL_DEVICE_STATE_UNBOUND = 0,
  NXHAL_DEVICE_STATE_BINDING = 1,
  NXHAL_DEVICE_STATE_BOUND = 2
};

static struct nxhal_device_protocol_v1 device_protocol;
static uint32_t device_protocol_state;

static void nxhal_device_protocol_stop(void) __attribute__((noreturn));
static void nxhal_device_protocol_stop(void) {
  hal_irq_disable_all();
  hal_cpu_halt();
}

static int nxhal_device_protocol_valid(
    const struct nxhal_device_protocol_v1 *protocol) {
  if (protocol == NULL)
    return NXHAL_DEVICE_BIND_E_NULL;
  if (protocol->abi_version != NXHAL_DEVICE_PROTOCOL_ABI_VERSION)
    return NXHAL_DEVICE_BIND_E_VERSION;
  if (protocol->struct_size != sizeof(struct nxhal_device_protocol_v1))
    return NXHAL_DEVICE_BIND_E_SIZE;
  if (protocol->flags != 0U || protocol->reserved0 != 0U)
    return NXHAL_DEVICE_BIND_E_RESERVED;
  if (protocol->publish == NULL || protocol->get_count == NULL ||
      protocol->get == NULL)
    return NXHAL_DEVICE_BIND_E_MISSING;
  return NXHAL_DEVICE_BIND_OK;
}

int nxhal_device_bind(const struct nxhal_device_protocol_v1 *protocol) {
  int valid = nxhal_device_protocol_valid(protocol);
  uint32_t expected = NXHAL_DEVICE_STATE_UNBOUND;

  if (valid != NXHAL_DEVICE_BIND_OK)
    return valid;
  if (!__atomic_compare_exchange_n(&device_protocol_state, &expected,
                                   NXHAL_DEVICE_STATE_BINDING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return NXHAL_DEVICE_BIND_E_STATE;
  device_protocol = *protocol;
  __atomic_store_n(&device_protocol_state, NXHAL_DEVICE_STATE_BOUND,
                   __ATOMIC_RELEASE);
  return NXHAL_DEVICE_BIND_OK;
}

int nxhal_device_is_bound(void) {
  return __atomic_load_n(&device_protocol_state, __ATOMIC_ACQUIRE) ==
         NXHAL_DEVICE_STATE_BOUND;
}

static const struct nxhal_device_protocol_v1 *nxhal_device_protocol(void) {
  if (!nxhal_device_is_bound())
    nxhal_device_protocol_stop();
  return &device_protocol;
}

void nxhal_device_publish(const struct nxhal_device_desc *device) {
  nxhal_device_protocol()->publish(device);
}

int nxhal_device_get_count(void) {
  return nxhal_device_protocol()->get_count();
}

int nxhal_device_get(int index, struct nxhal_device_desc *device) {
  return nxhal_device_protocol()->get(index, device);
}
