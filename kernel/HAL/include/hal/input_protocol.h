#ifndef NXHAL_INPUT_PROTOCOL_H
#define NXHAL_INPUT_PROTOCOL_H

#include <stdint.h>

#define NXHAL_INPUT_PROTOCOL_ABI_VERSION UINT32_C(1)

enum nxhal_input_bind_status {
  NXHAL_INPUT_BIND_OK = 0,
  NXHAL_INPUT_BIND_E_NULL = -1,
  NXHAL_INPUT_BIND_E_VERSION = -2,
  NXHAL_INPUT_BIND_E_SIZE = -3,
  NXHAL_INPUT_BIND_E_RESERVED = -4,
  NXHAL_INPUT_BIND_E_MISSING = -5,
  NXHAL_INPUT_BIND_E_STATE = -6
};

struct nxhal_input_protocol_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t reserved0;

  void (*rx_ready)(void);
};

int nxhal_input_bind(const struct nxhal_input_protocol_v1 *protocol);
int nxhal_input_is_bound(void);
void nxhal_input_rx_ready(void);

#endif /* NXHAL_INPUT_PROTOCOL_H */
