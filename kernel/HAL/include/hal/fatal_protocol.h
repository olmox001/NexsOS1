#ifndef NXHAL_FATAL_PROTOCOL_H
#define NXHAL_FATAL_PROTOCOL_H

#include <hal/frame.h>
#include <stdint.h>

#define NXHAL_FATAL_PROTOCOL_ABI_VERSION UINT32_C(1)

enum nxhal_fatal_bind_status {
  NXHAL_FATAL_BIND_OK = 0,
  NXHAL_FATAL_BIND_E_NULL = -1,
  NXHAL_FATAL_BIND_E_VERSION = -2,
  NXHAL_FATAL_BIND_E_SIZE = -3,
  NXHAL_FATAL_BIND_E_RESERVED = -4,
  NXHAL_FATAL_BIND_E_MISSING = -5,
  NXHAL_FATAL_BIND_E_STATE = -6
};

struct nxhal_fatal_protocol_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t reserved0;

  hal_frame_t *(*serror)(hal_frame_t *frame);
};

int nxhal_fatal_bind(const struct nxhal_fatal_protocol_v1 *protocol);
int nxhal_fatal_is_bound(void);
hal_frame_t *nxhal_fatal_serror(hal_frame_t *frame);

#endif /* NXHAL_FATAL_PROTOCOL_H */
