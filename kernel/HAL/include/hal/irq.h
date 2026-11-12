#ifndef NXHAL_IRQ_H
#define NXHAL_IRQ_H

#include <stdint.h>

#define NXHAL_IRQ_CORE_PROTOCOL_ABI_VERSION UINT32_C(1)
#define NXHAL_IRQ_SPURIOUS UINT32_C(1023)

typedef void (*nxhal_irq_handler_t)(uint32_t irq, void *data);

struct nxhal_irq_controller_ops {
  void (*init_global)(void);
  void (*init_percpu)(void);
  void (*enable)(uint32_t irq);
  void (*disable)(uint32_t irq);
  void (*set_priority)(uint32_t irq, uint8_t priority);
  void (*send_ipi_all)(void);
  int (*send_reschedule)(uint32_t cpu_id);
  uint32_t reschedule_irq;
  uint32_t (*acknowledge)(void);
  void (*end)(uint32_t irq);
};

enum nxhal_irq_controller_status {
  NXHAL_IRQ_CONTROLLER_OK = 0,
  NXHAL_IRQ_CONTROLLER_E_NULL = -1,
  NXHAL_IRQ_CONTROLLER_E_MISSING = -2,
  NXHAL_IRQ_CONTROLLER_E_STATE = -3
};

int nxhal_irq_controller_install(const struct nxhal_irq_controller_ops *ops);
int hal_irq_controller_available(void);
void hal_irq_controller_global_init(void);
void hal_irq_controller_percpu_init(void);
void hal_irq_line_enable(uint32_t irq);
void hal_irq_line_disable(uint32_t irq);
void hal_irq_set_priority(uint32_t irq, uint8_t priority);
void hal_irq_send_ipi_all(void);
int hal_irq_send_reschedule(uint32_t cpu_id);
int hal_irq_is_reschedule(uint32_t irq);
uint32_t hal_irq_acknowledge(void);
void hal_irq_end(uint32_t irq);

enum nxhal_irq_core_bind_status {
  NXHAL_IRQ_CORE_BIND_OK = 0,
  NXHAL_IRQ_CORE_BIND_E_NULL = -1,
  NXHAL_IRQ_CORE_BIND_E_VERSION = -2,
  NXHAL_IRQ_CORE_BIND_E_SIZE = -3,
  NXHAL_IRQ_CORE_BIND_E_RESERVED = -4,
  NXHAL_IRQ_CORE_BIND_E_MISSING = -5,
  NXHAL_IRQ_CORE_BIND_E_STATE = -6
};

struct nxhal_irq_core_protocol_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t reserved0;

  int (*register_handler)(uint32_t irq, nxhal_irq_handler_t handler, void *data);
  void (*unregister_handler)(uint32_t irq);
};

int nxhal_irq_core_bind(const struct nxhal_irq_core_protocol_v1 *protocol);
int nxhal_irq_core_is_bound(void);
int nxhal_irq_register_handler(uint32_t irq, nxhal_irq_handler_t handler,
                               void *data);
void nxhal_irq_unregister_handler(uint32_t irq);

#endif /* NXHAL_IRQ_H */
