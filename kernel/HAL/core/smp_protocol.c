#include <hal/core.h>
#include <hal/smp_protocol.h>
#include <stddef.h>
#include <stdint.h>

static struct nxhal_smp_protocol_v1 smp_protocol;
static volatile uint32_t smp_bound;

static void nxhal_smp_unbound(void) __attribute__((noreturn));
static void nxhal_smp_unbound(void) {
  hal_irq_disable_all();
  hal_cpu_halt();
}

int nxhal_smp_bind(const struct nxhal_smp_protocol_v1 *protocol) {
  if (protocol == NULL)
    return NXHAL_SMP_BIND_E_NULL;
  if (protocol->abi_version != NXHAL_SMP_PROTOCOL_ABI_VERSION)
    return NXHAL_SMP_BIND_E_VERSION;
  if (protocol->struct_size < sizeof(struct nxhal_smp_protocol_v1))
    return NXHAL_SMP_BIND_E_SIZE;
  if (protocol->flags != 0U || protocol->reserved0 != 0U)
    return NXHAL_SMP_BIND_E_RESERVED;
  if (protocol->bringup_secondary == NULL)
    return NXHAL_SMP_BIND_E_MISSING;
  if (!__sync_bool_compare_and_swap(&smp_bound, 0U, 1U))
    return NXHAL_SMP_BIND_E_STATE;

  smp_protocol = *protocol;
  __sync_synchronize();
  smp_bound = 2U;
  return NXHAL_SMP_BIND_OK;
}

int nxhal_smp_is_bound(void) {
  return smp_bound == 2U;
}

int nxhal_smp_bringup_secondary(uint32_t cpu_id, void *stack_top) {
  if (!nxhal_smp_is_bound())
    nxhal_smp_unbound();
  return smp_protocol.bringup_secondary(cpu_id, stack_top);
}
