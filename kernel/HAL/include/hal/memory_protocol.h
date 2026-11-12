#ifndef NXHAL_MEMORY_PROTOCOL_H
#define NXHAL_MEMORY_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define NXHAL_MEMORY_PROTOCOL_ABI_VERSION UINT32_C(1)

enum nxhal_memory_bind_status {
  NXHAL_MEMORY_BIND_OK = 0,
  NXHAL_MEMORY_BIND_E_NULL = -1,
  NXHAL_MEMORY_BIND_E_VERSION = -2,
  NXHAL_MEMORY_BIND_E_SIZE = -3,
  NXHAL_MEMORY_BIND_E_RESERVED = -4,
  NXHAL_MEMORY_BIND_E_MISSING = -5,
  NXHAL_MEMORY_BIND_E_STATE = -6
};

struct nxhal_memory_protocol_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint32_t flags;
  uint32_t reserved0;

  void *(*alloc_page)(void);
  void *(*alloc_pages)(size_t count);
  void (*free_page)(void *page);
  void (*free_pages)(void *page, size_t count);
};

int nxhal_memory_bind(const struct nxhal_memory_protocol_v1 *protocol);
int nxhal_memory_is_bound(void);
void *nxhal_memory_alloc_page(void);
void *nxhal_memory_alloc_pages(size_t count);
void nxhal_memory_free_page(void *page);
void nxhal_memory_free_pages(void *page, size_t count);

#endif /* NXHAL_MEMORY_PROTOCOL_H */
