#include <hal/irq.h>
#include <stdint.h>
#include <stdlib.h>

static unsigned global_init_calls;
static unsigned percpu_init_calls;
static unsigned enable_calls;
static unsigned disable_calls;
static unsigned priority_calls;
static unsigned ipi_calls;
static unsigned end_calls;
static unsigned register_calls;
static unsigned unregister_calls;
static uint32_t last_irq;

void hal_irq_disable_all(void) { abort(); }
void hal_cpu_halt(void) { abort(); }

static void test_global_init(void) { ++global_init_calls; }
static void test_percpu_init(void) { ++percpu_init_calls; }
static void test_enable(uint32_t irq) { ++enable_calls; last_irq = irq; }
static void test_disable(uint32_t irq) { ++disable_calls; last_irq = irq; }
static void test_priority(uint32_t irq, uint8_t priority) {
  if (irq == UINT32_C(9) && priority == UINT8_C(0x55))
    ++priority_calls;
}
static void test_ipi(void) { ++ipi_calls; }
static uint32_t test_ack(void) { return UINT32_C(0x123); }
static void test_end(uint32_t irq) { ++end_calls; last_irq = irq; }

static void dummy_handler(uint32_t irq, void *data) {
  (void)irq;
  (void)data;
}

static int core_register(uint32_t irq, nxhal_irq_handler_t handler, void *data) {
  if (irq == UINT32_C(7) && handler == dummy_handler &&
      data == (void *)(uintptr_t)UINT64_C(0x1234))
    ++register_calls;
  return 19;
}
static void core_unregister(uint32_t irq) {
  if (irq == UINT32_C(7))
    ++unregister_calls;
}

static const struct nxhal_irq_controller_ops controller = {
    .init_global = test_global_init,
    .init_percpu = test_percpu_init,
    .enable = test_enable,
    .disable = test_disable,
    .set_priority = test_priority,
    .send_ipi_all = test_ipi,
    .acknowledge = test_ack,
    .end = test_end,
};

static const struct nxhal_irq_controller_ops other_controller = {
    .init_global = test_global_init,
    .init_percpu = test_percpu_init,
    .enable = test_enable,
    .disable = test_disable,
    .set_priority = test_priority,
    .send_ipi_all = test_ipi,
    .acknowledge = test_ack,
    .end = test_end,
};

static struct nxhal_irq_core_protocol_v1 valid_core_protocol(void) {
  struct nxhal_irq_core_protocol_v1 protocol = {
      .abi_version = NXHAL_IRQ_CORE_PROTOCOL_ABI_VERSION,
      .struct_size = sizeof(struct nxhal_irq_core_protocol_v1),
      .flags = 0U,
      .reserved0 = 0U,
      .register_handler = core_register,
      .unregister_handler = core_unregister,
  };
  return protocol;
}

int main(void) {
  struct nxhal_irq_controller_ops incomplete = controller;
  struct nxhal_irq_core_protocol_v1 protocol = valid_core_protocol();

  if (nxhal_irq_controller_install(NULL) != NXHAL_IRQ_CONTROLLER_E_NULL)
    return 1;
  incomplete.end = NULL;
  if (nxhal_irq_controller_install(&incomplete) != NXHAL_IRQ_CONTROLLER_E_MISSING)
    return 2;
  if (hal_irq_controller_available())
    return 3;
  if (nxhal_irq_controller_install(&controller) != NXHAL_IRQ_CONTROLLER_OK)
    return 4;
  if (!hal_irq_controller_available())
    return 5;
  if (nxhal_irq_controller_install(&controller) != NXHAL_IRQ_CONTROLLER_OK)
    return 6;
  if (nxhal_irq_controller_install(&other_controller) !=
      NXHAL_IRQ_CONTROLLER_E_STATE)
    return 7;

  hal_irq_controller_global_init();
  hal_irq_controller_percpu_init();
  hal_irq_line_enable(UINT32_C(33));
  hal_irq_line_disable(UINT32_C(34));
  hal_irq_set_priority(UINT32_C(9), UINT8_C(0x55));
  hal_irq_send_ipi_all();
  if (hal_irq_acknowledge() != UINT32_C(0x123))
    return 8;
  hal_irq_end(UINT32_C(35));
  if (global_init_calls != 1U || percpu_init_calls != 1U ||
      enable_calls != 1U || disable_calls != 1U ||
      priority_calls != 1U || ipi_calls != 1U || end_calls != 1U ||
      last_irq != UINT32_C(35))
    return 9;

  if (nxhal_irq_core_bind(NULL) != NXHAL_IRQ_CORE_BIND_E_NULL)
    return 10;
  protocol.abi_version = 0U;
  if (nxhal_irq_core_bind(&protocol) != NXHAL_IRQ_CORE_BIND_E_VERSION)
    return 11;
  protocol = valid_core_protocol();
  protocol.struct_size -= 1U;
  if (nxhal_irq_core_bind(&protocol) != NXHAL_IRQ_CORE_BIND_E_SIZE)
    return 12;
  protocol = valid_core_protocol();
  protocol.flags = 1U;
  if (nxhal_irq_core_bind(&protocol) != NXHAL_IRQ_CORE_BIND_E_RESERVED)
    return 13;
  protocol = valid_core_protocol();
  protocol.register_handler = NULL;
  if (nxhal_irq_core_bind(&protocol) != NXHAL_IRQ_CORE_BIND_E_MISSING)
    return 14;

  protocol = valid_core_protocol();
  if (nxhal_irq_core_bind(&protocol) != NXHAL_IRQ_CORE_BIND_OK)
    return 15;
  if (!nxhal_irq_core_is_bound())
    return 16;
  if (nxhal_irq_core_bind(&protocol) != NXHAL_IRQ_CORE_BIND_E_STATE)
    return 17;

  if (nxhal_irq_register_handler(UINT32_C(7), dummy_handler,
                                 (void *)(uintptr_t)UINT64_C(0x1234)) != 19)
    return 18;
  nxhal_irq_unregister_handler(UINT32_C(7));
  if (register_calls != 1U || unregister_calls != 1U)
    return 19;

  return 0;
}
