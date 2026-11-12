#include <hal/core.h>
#include <hal/timer_protocol.h>
#include <stddef.h>
#include <stdint.h>

enum nxhal_timer_bind_state {
  NXHAL_TIMER_UNBOUND = 0,
  NXHAL_TIMER_BINDING = 1,
  NXHAL_TIMER_BOUND = 2
};

static struct nxhal_timer_protocol_v1 timer_protocol;
static uint32_t timer_bound;

static void nxhal_timer_unbound(void) __attribute__((noreturn));
static void nxhal_timer_unbound(void) {
  hal_irq_disable_all();
  hal_cpu_halt();
}

int nxhal_timer_bind(const struct nxhal_timer_protocol_v1 *protocol) {
  if (protocol == NULL)
    return NXHAL_TIMER_BIND_E_NULL;
  if (protocol->abi_version != NXHAL_TIMER_PROTOCOL_ABI_VERSION)
    return NXHAL_TIMER_BIND_E_VERSION;
  if (protocol->struct_size < sizeof(struct nxhal_timer_protocol_v1))
    return NXHAL_TIMER_BIND_E_SIZE;
  if (protocol->flags != 0U || protocol->reserved0 != 0U)
    return NXHAL_TIMER_BIND_E_RESERVED;
  if (protocol->arm_current == NULL || protocol->tick_current == NULL)
    return NXHAL_TIMER_BIND_E_MISSING;
  uint32_t expected = NXHAL_TIMER_UNBOUND;
  if (!__atomic_compare_exchange_n(&timer_bound, &expected,
                                   NXHAL_TIMER_BINDING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return NXHAL_TIMER_BIND_E_STATE;

  timer_protocol = *protocol;
  __atomic_store_n(&timer_bound, NXHAL_TIMER_BOUND, __ATOMIC_RELEASE);
  return NXHAL_TIMER_BIND_OK;
}

int nxhal_timer_is_bound(void) {
  return __atomic_load_n(&timer_bound, __ATOMIC_ACQUIRE) ==
         NXHAL_TIMER_BOUND;
}

uint64_t nxhal_timer_arm_current(void) {
  if (!nxhal_timer_is_bound())
    nxhal_timer_unbound();
  return timer_protocol.arm_current();
}

uint64_t nxhal_timer_tick_current(void) {
  if (!nxhal_timer_is_bound())
    nxhal_timer_unbound();
  return timer_protocol.tick_current();
}
