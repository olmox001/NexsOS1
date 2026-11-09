#include <hal/core.h>
#include <hal/irq.h>
#include <stddef.h>
#include <stdint.h>

static const struct nxhal_irq_controller_ops *irq_controller;

enum nxhal_irq_core_state {
  NXHAL_IRQ_CORE_UNBOUND = 0,
  NXHAL_IRQ_CORE_BINDING = 1,
  NXHAL_IRQ_CORE_BOUND = 2
};

static struct nxhal_irq_core_protocol_v1 irq_core_protocol;
static uint32_t irq_core_state;

static void nxhal_irq_stop(void) __attribute__((noreturn));
static void nxhal_irq_stop(void) {
  hal_irq_disable_all();
  hal_cpu_halt();
}

int nxhal_irq_controller_install(const struct nxhal_irq_controller_ops *ops) {
  const struct nxhal_irq_controller_ops *installed;

  if (ops == NULL)
    return NXHAL_IRQ_CONTROLLER_E_NULL;
  if (ops->enable == NULL || ops->disable == NULL ||
      ops->acknowledge == NULL || ops->end == NULL)
    return NXHAL_IRQ_CONTROLLER_E_MISSING;

  installed = __atomic_load_n(&irq_controller, __ATOMIC_ACQUIRE);
  if (installed == ops)
    return NXHAL_IRQ_CONTROLLER_OK;
  if (installed != NULL)
    return NXHAL_IRQ_CONTROLLER_E_STATE;

  if (!__atomic_compare_exchange_n(&irq_controller, &installed, ops, 0,
                                   __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
    if (installed == ops)
      return NXHAL_IRQ_CONTROLLER_OK;
    return NXHAL_IRQ_CONTROLLER_E_STATE;
  }
  return NXHAL_IRQ_CONTROLLER_OK;
}

int hal_irq_controller_available(void) {
  return __atomic_load_n(&irq_controller, __ATOMIC_ACQUIRE) != NULL;
}

static const struct nxhal_irq_controller_ops *nxhal_irq_controller(void) {
  return __atomic_load_n(&irq_controller, __ATOMIC_ACQUIRE);
}

void hal_irq_controller_global_init(void) {
  const struct nxhal_irq_controller_ops *ops = nxhal_irq_controller();
  if (ops != NULL && ops->init_global != NULL)
    ops->init_global();
}

void hal_irq_controller_percpu_init(void) {
  const struct nxhal_irq_controller_ops *ops = nxhal_irq_controller();
  if (ops != NULL && ops->init_percpu != NULL)
    ops->init_percpu();
}

void hal_irq_line_enable(uint32_t irq) {
  const struct nxhal_irq_controller_ops *ops = nxhal_irq_controller();
  if (ops != NULL)
    ops->enable(irq);
}

void hal_irq_line_disable(uint32_t irq) {
  const struct nxhal_irq_controller_ops *ops = nxhal_irq_controller();
  if (ops != NULL)
    ops->disable(irq);
}

void hal_irq_set_priority(uint32_t irq, uint8_t priority) {
  const struct nxhal_irq_controller_ops *ops = nxhal_irq_controller();
  if (ops != NULL && ops->set_priority != NULL)
    ops->set_priority(irq, priority);
}

void hal_irq_send_ipi_all(void) {
  const struct nxhal_irq_controller_ops *ops = nxhal_irq_controller();
  if (ops != NULL && ops->send_ipi_all != NULL)
    ops->send_ipi_all();
}

uint32_t hal_irq_acknowledge(void) {
  const struct nxhal_irq_controller_ops *ops = nxhal_irq_controller();
  if (ops == NULL)
    return NXHAL_IRQ_SPURIOUS;
  return ops->acknowledge();
}

void hal_irq_end(uint32_t irq) {
  const struct nxhal_irq_controller_ops *ops = nxhal_irq_controller();
  if (ops != NULL)
    ops->end(irq);
}

static int nxhal_irq_core_protocol_valid(
    const struct nxhal_irq_core_protocol_v1 *protocol) {
  if (protocol == NULL)
    return NXHAL_IRQ_CORE_BIND_E_NULL;
  if (protocol->abi_version != NXHAL_IRQ_CORE_PROTOCOL_ABI_VERSION)
    return NXHAL_IRQ_CORE_BIND_E_VERSION;
  if (protocol->struct_size != sizeof(struct nxhal_irq_core_protocol_v1))
    return NXHAL_IRQ_CORE_BIND_E_SIZE;
  if (protocol->flags != 0U || protocol->reserved0 != 0U)
    return NXHAL_IRQ_CORE_BIND_E_RESERVED;
  if (protocol->register_handler == NULL ||
      protocol->unregister_handler == NULL)
    return NXHAL_IRQ_CORE_BIND_E_MISSING;
  return NXHAL_IRQ_CORE_BIND_OK;
}

int nxhal_irq_core_bind(const struct nxhal_irq_core_protocol_v1 *protocol) {
  int valid = nxhal_irq_core_protocol_valid(protocol);
  uint32_t expected = NXHAL_IRQ_CORE_UNBOUND;

  if (valid != NXHAL_IRQ_CORE_BIND_OK)
    return valid;
  if (!__atomic_compare_exchange_n(&irq_core_state, &expected,
                                   NXHAL_IRQ_CORE_BINDING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return NXHAL_IRQ_CORE_BIND_E_STATE;

  irq_core_protocol.abi_version = protocol->abi_version;
  irq_core_protocol.struct_size = protocol->struct_size;
  irq_core_protocol.flags = protocol->flags;
  irq_core_protocol.reserved0 = protocol->reserved0;
  irq_core_protocol.register_handler = protocol->register_handler;
  irq_core_protocol.unregister_handler = protocol->unregister_handler;

  __atomic_store_n(&irq_core_state, NXHAL_IRQ_CORE_BOUND, __ATOMIC_RELEASE);
  return NXHAL_IRQ_CORE_BIND_OK;
}

int nxhal_irq_core_is_bound(void) {
  return __atomic_load_n(&irq_core_state, __ATOMIC_ACQUIRE) ==
         NXHAL_IRQ_CORE_BOUND;
}

static const struct nxhal_irq_core_protocol_v1 *nxhal_irq_core(void) {
  if (!nxhal_irq_core_is_bound())
    nxhal_irq_stop();
  return &irq_core_protocol;
}

int nxhal_irq_register_handler(uint32_t irq, nxhal_irq_handler_t handler,
                               void *data) {
  return nxhal_irq_core()->register_handler(irq, handler, data);
}

void nxhal_irq_unregister_handler(uint32_t irq) {
  nxhal_irq_core()->unregister_handler(irq);
}
