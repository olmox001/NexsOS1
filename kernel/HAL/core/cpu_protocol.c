#include <hal/core.h>
#include <hal/cpu_protocol.h>
#include <hal/provider.h>
#include <stddef.h>
#include <stdint.h>

enum nxhal_cpu_state {
  NXHAL_CPU_UNBOUND = 0,
  NXHAL_CPU_BINDING = 1,
  NXHAL_CPU_BOUND = 2
};

static struct nxhal_cpu_protocol_v1 cpu_protocol;
static uint32_t cpu_state;

static void nxhal_cpu_stop(void) __attribute__((noreturn));
static void nxhal_cpu_stop(void) {
  hal_irq_disable_all();
  hal_cpu_halt();
}

static int nxhal_cpu_protocol_valid(
    const struct nxhal_cpu_protocol_v1 *protocol) {
  if (protocol == NULL)
    return NXHAL_CPU_BIND_E_NULL;
  if (protocol->abi_version != NXHAL_CPU_PROTOCOL_ABI_VERSION)
    return NXHAL_CPU_BIND_E_VERSION;
  if (protocol->struct_size != sizeof(struct nxhal_cpu_protocol_v1))
    return NXHAL_CPU_BIND_E_SIZE;
  if (protocol->flags != 0U || protocol->reserved0 != 0U)
    return NXHAL_CPU_BIND_E_RESERVED;
  if (protocol->online == NULL || protocol->is_online == NULL)
    return NXHAL_CPU_BIND_E_MISSING;
  return NXHAL_CPU_BIND_OK;
}

int nxhal_cpu_bind(const struct nxhal_cpu_protocol_v1 *protocol) {
  int valid = nxhal_cpu_protocol_valid(protocol);
  uint32_t expected = NXHAL_CPU_UNBOUND;

  if (valid != NXHAL_CPU_BIND_OK)
    return valid;
  if (!__atomic_compare_exchange_n(&cpu_state, &expected, NXHAL_CPU_BINDING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return NXHAL_CPU_BIND_E_STATE;

  cpu_protocol.abi_version = protocol->abi_version;
  cpu_protocol.struct_size = protocol->struct_size;
  cpu_protocol.flags = protocol->flags;
  cpu_protocol.reserved0 = protocol->reserved0;
  cpu_protocol.online = protocol->online;
  cpu_protocol.is_online = protocol->is_online;
  __atomic_store_n(&cpu_state, NXHAL_CPU_BOUND, __ATOMIC_RELEASE);
  return NXHAL_CPU_BIND_OK;
}

int nxhal_cpu_is_bound(void) {
  return __atomic_load_n(&cpu_state, __ATOMIC_ACQUIRE) == NXHAL_CPU_BOUND;
}

static const struct nxhal_cpu_protocol_v1 *nxhal_cpu_protocol(void) {
  if (!nxhal_cpu_is_bound())
    nxhal_cpu_stop();
  return &cpu_protocol;
}

int nxhal_cpu_online(uint32_t cpu_id) {
  return nxhal_cpu_protocol()->online(cpu_id);
}

int nxhal_cpu_is_online(uint32_t cpu_id) {
  return nxhal_cpu_protocol()->is_online(cpu_id);
}

void hal_cpu_switch_context(const struct nxhal_cpu_context_switch *context) {
  if (context == NULL)
    nxhal_cpu_stop();
  nxhal_provider_cpu_switch_context(context);
}
