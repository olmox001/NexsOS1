#include <hal/smp_protocol.h>
#include <stdint.h>
#include <stdlib.h>

static unsigned calls;
static uint32_t last_cpu;
static void *last_stack;

void hal_irq_disable_all(void) {
  abort();
}

void hal_cpu_halt(void) {
  abort();
}

static int test_bringup(uint32_t cpu_id, void *stack_top) {
  ++calls;
  last_cpu = cpu_id;
  last_stack = stack_top;
  return -2;
}

static struct nxhal_smp_protocol_v1 valid_protocol(void) {
  struct nxhal_smp_protocol_v1 protocol = {
      .abi_version = NXHAL_SMP_PROTOCOL_ABI_VERSION,
      .struct_size = sizeof(struct nxhal_smp_protocol_v1),
      .flags = 0U,
      .reserved0 = 0U,
      .bringup_secondary = test_bringup,
  };
  return protocol;
}

int main(void) {
  struct nxhal_smp_protocol_v1 protocol = valid_protocol();
  void *stack = (void *)(uintptr_t)UINT64_C(0x12345000);

  if (nxhal_smp_bind(NULL) != NXHAL_SMP_BIND_E_NULL)
    return 1;
  protocol.abi_version = 0U;
  if (nxhal_smp_bind(&protocol) != NXHAL_SMP_BIND_E_VERSION)
    return 2;
  protocol = valid_protocol();
  protocol.struct_size -= 1U;
  if (nxhal_smp_bind(&protocol) != NXHAL_SMP_BIND_E_SIZE)
    return 3;
  protocol = valid_protocol();
  protocol.flags = 1U;
  if (nxhal_smp_bind(&protocol) != NXHAL_SMP_BIND_E_RESERVED)
    return 4;
  protocol = valid_protocol();
  protocol.bringup_secondary = NULL;
  if (nxhal_smp_bind(&protocol) != NXHAL_SMP_BIND_E_MISSING)
    return 5;
  if (nxhal_smp_is_bound())
    return 6;

  protocol = valid_protocol();
  if (nxhal_smp_bind(&protocol) != NXHAL_SMP_BIND_OK)
    return 7;
  if (!nxhal_smp_is_bound())
    return 8;
  if (nxhal_smp_bind(&protocol) != NXHAL_SMP_BIND_E_STATE)
    return 9;
  if (nxhal_smp_bringup_secondary(7U, stack) != -2)
    return 10;
  if (calls != 1U || last_cpu != 7U || last_stack != stack)
    return 11;

  return 0;
}
