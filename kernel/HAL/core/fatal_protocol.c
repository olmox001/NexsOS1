#include <hal/core.h>
#include <hal/fatal_protocol.h>
#include <hal/frame.h>
#include <stddef.h>
#include <stdint.h>

enum nxhal_fatal_state {
  NXHAL_FATAL_UNBOUND = 0,
  NXHAL_FATAL_BINDING = 1,
  NXHAL_FATAL_BOUND = 2
};

static struct nxhal_fatal_protocol_v1 fatal_protocol;
static uint32_t fatal_state;

static void nxhal_fatal_stop(void) __attribute__((noreturn));
static void nxhal_fatal_stop(void) {
  hal_irq_disable_all();
  hal_cpu_halt();
}

static int nxhal_fatal_protocol_valid(
    const struct nxhal_fatal_protocol_v1 *protocol) {
  if (protocol == NULL)
    return NXHAL_FATAL_BIND_E_NULL;
  if (protocol->abi_version != NXHAL_FATAL_PROTOCOL_ABI_VERSION)
    return NXHAL_FATAL_BIND_E_VERSION;
  if (protocol->struct_size != sizeof(struct nxhal_fatal_protocol_v1))
    return NXHAL_FATAL_BIND_E_SIZE;
  if (protocol->flags != 0U || protocol->reserved0 != 0U)
    return NXHAL_FATAL_BIND_E_RESERVED;
  if (protocol->serror == NULL)
    return NXHAL_FATAL_BIND_E_MISSING;
  return NXHAL_FATAL_BIND_OK;
}

int nxhal_fatal_bind(const struct nxhal_fatal_protocol_v1 *protocol) {
  int valid = nxhal_fatal_protocol_valid(protocol);
  uint32_t expected = NXHAL_FATAL_UNBOUND;

  if (valid != NXHAL_FATAL_BIND_OK)
    return valid;
  if (!__atomic_compare_exchange_n(&fatal_state, &expected,
                                   NXHAL_FATAL_BINDING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return NXHAL_FATAL_BIND_E_STATE;

  fatal_protocol.abi_version = protocol->abi_version;
  fatal_protocol.struct_size = protocol->struct_size;
  fatal_protocol.flags = protocol->flags;
  fatal_protocol.reserved0 = protocol->reserved0;
  fatal_protocol.serror = protocol->serror;
  __atomic_store_n(&fatal_state, NXHAL_FATAL_BOUND, __ATOMIC_RELEASE);
  return NXHAL_FATAL_BIND_OK;
}

int nxhal_fatal_is_bound(void) {
  return __atomic_load_n(&fatal_state, __ATOMIC_ACQUIRE) == NXHAL_FATAL_BOUND;
}

hal_frame_t *nxhal_fatal_serror(hal_frame_t *frame) {
  if (!nxhal_fatal_is_bound())
    nxhal_fatal_stop();
  return fatal_protocol.serror(frame);
}
