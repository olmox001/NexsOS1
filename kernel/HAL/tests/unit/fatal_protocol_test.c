#include <hal/fatal_protocol.h>
#include <stdint.h>
#include <stdlib.h>

static unsigned serror_calls;
static hal_frame_t *expected_frame;

void hal_irq_disable_all(void) { abort(); }
void hal_cpu_halt(void) { abort(); }

static hal_frame_t *test_serror(hal_frame_t *frame) {
  if (frame == expected_frame)
    ++serror_calls;
  return frame;
}

static struct nxhal_fatal_protocol_v1 valid_protocol(void) {
  struct nxhal_fatal_protocol_v1 protocol = {
      .abi_version = NXHAL_FATAL_PROTOCOL_ABI_VERSION,
      .struct_size = sizeof(struct nxhal_fatal_protocol_v1),
      .flags = 0U,
      .reserved0 = 0U,
      .serror = test_serror,
  };
  return protocol;
}

int main(void) {
  struct nxhal_fatal_protocol_v1 protocol = valid_protocol();

  if (nxhal_fatal_bind(NULL) != NXHAL_FATAL_BIND_E_NULL)
    return 1;
  protocol.abi_version = 0U;
  if (nxhal_fatal_bind(&protocol) != NXHAL_FATAL_BIND_E_VERSION)
    return 2;
  protocol = valid_protocol();
  protocol.struct_size -= 1U;
  if (nxhal_fatal_bind(&protocol) != NXHAL_FATAL_BIND_E_SIZE)
    return 3;
  protocol = valid_protocol();
  protocol.flags = 1U;
  if (nxhal_fatal_bind(&protocol) != NXHAL_FATAL_BIND_E_RESERVED)
    return 4;
  protocol = valid_protocol();
  protocol.serror = NULL;
  if (nxhal_fatal_bind(&protocol) != NXHAL_FATAL_BIND_E_MISSING)
    return 5;

  protocol = valid_protocol();
  if (nxhal_fatal_bind(&protocol) != NXHAL_FATAL_BIND_OK)
    return 6;
  if (!nxhal_fatal_is_bound())
    return 7;
  if (nxhal_fatal_bind(&protocol) != NXHAL_FATAL_BIND_E_STATE)
    return 8;

  expected_frame = (hal_frame_t *)(uintptr_t)UINT64_C(0x4000);
  if (nxhal_fatal_serror(expected_frame) != expected_frame)
    return 9;
  if (serror_calls != 1U)
    return 10;
  return 0;
}
