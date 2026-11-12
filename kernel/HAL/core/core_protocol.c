#include <hal/core.h>
#include <hal/core_protocol.h>
#include <stddef.h>

enum nxhal_core_protocol_state {
  NXHAL_CORE_STATE_UNBOUND = 0,
  NXHAL_CORE_STATE_BINDING = 1,
  NXHAL_CORE_STATE_BOUND = 2
};

static struct nxhal_core_protocol_v1 core_protocol;
static uint32_t core_protocol_state;

static void nxhal_core_protocol_stop(void) __attribute__((noreturn));
static void nxhal_core_protocol_stop(void) {
  hal_irq_disable_all();
  hal_cpu_halt();
}

static int nxhal_core_protocol_valid(
    const struct nxhal_core_protocol_v1 *protocol) {
  if (protocol == NULL)
    return NXHAL_CORE_BIND_E_NULL;
  if (protocol->abi_version != NXHAL_CORE_PROTOCOL_ABI_VERSION)
    return NXHAL_CORE_BIND_E_VERSION;
  if (protocol->struct_size != sizeof(struct nxhal_core_protocol_v1))
    return NXHAL_CORE_BIND_E_SIZE;
  if (protocol->flags != 0U || protocol->reserved0 != 0U)
    return NXHAL_CORE_BIND_E_RESERVED;
  if (protocol->schedule == NULL || protocol->syscall == NULL ||
      protocol->irq_entry == NULL || protocol->irq_dispatch == NULL ||
      protocol->timer_tick == NULL || protocol->secondary_entry == NULL)
    return NXHAL_CORE_BIND_E_MISSING;
  return NXHAL_CORE_BIND_OK;
}

int nxhal_core_bind(const struct nxhal_core_protocol_v1 *protocol) {

  int valid = nxhal_core_protocol_valid(protocol);
  uint32_t expected = NXHAL_CORE_STATE_UNBOUND;

  if (valid != NXHAL_CORE_BIND_OK)
    return valid;

  if (!__atomic_compare_exchange_n(&core_protocol_state, &expected,
                                   NXHAL_CORE_STATE_BINDING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return NXHAL_CORE_BIND_E_STATE;


  /* Copy field-by-field: no libc/runtime dependency and no partial publish. */
  core_protocol.abi_version = protocol->abi_version;
  core_protocol.struct_size = protocol->struct_size;
  core_protocol.flags = protocol->flags;
  core_protocol.reserved0 = protocol->reserved0;
  core_protocol.schedule = protocol->schedule;
  core_protocol.syscall = protocol->syscall;
  core_protocol.irq_entry = protocol->irq_entry;
  core_protocol.irq_dispatch = protocol->irq_dispatch;
  core_protocol.timer_tick = protocol->timer_tick;
  core_protocol.secondary_entry = protocol->secondary_entry;


  __atomic_store_n(&core_protocol_state, NXHAL_CORE_STATE_BOUND,
                   __ATOMIC_RELEASE);
  return NXHAL_CORE_BIND_OK;
}

int nxhal_core_is_bound(void) {
  return __atomic_load_n(&core_protocol_state, __ATOMIC_ACQUIRE) ==
         NXHAL_CORE_STATE_BOUND;
}

static const struct nxhal_core_protocol_v1 *nxhal_core_protocol(void) {
  if (!nxhal_core_is_bound())
    nxhal_core_protocol_stop();
  return &core_protocol;
}

hal_frame_t *nxhal_core_schedule(hal_frame_t *frame) {
  return nxhal_core_protocol()->schedule(frame);
}

hal_frame_t *nxhal_core_syscall(hal_frame_t *frame) {
  return nxhal_core_protocol()->syscall(frame);
}

hal_frame_t *nxhal_core_irq_entry(hal_frame_t *frame) {
  return nxhal_core_protocol()->irq_entry(frame);
}

hal_frame_t *nxhal_core_irq_dispatch(uint32_t irq, hal_frame_t *frame) {
  return nxhal_core_protocol()->irq_dispatch(irq, frame);
}

hal_frame_t *nxhal_core_timer_tick(hal_frame_t *frame) {
  return nxhal_core_protocol()->timer_tick(frame);
}

void nxhal_core_secondary_entry(void) {
  nxhal_core_protocol()->secondary_entry();
  nxhal_core_protocol_stop();
}
