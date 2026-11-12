#include <hal/core.h>
#include <hal/memory_protocol.h>
#include <stddef.h>
#include <stdint.h>

enum nxhal_memory_state {
  NXHAL_MEMORY_UNBOUND = 0,
  NXHAL_MEMORY_BINDING = 1,
  NXHAL_MEMORY_BOUND = 2
};

static struct nxhal_memory_protocol_v1 memory_protocol;
static uint32_t memory_state;

static void nxhal_memory_stop(void) __attribute__((noreturn));
static void nxhal_memory_stop(void) {
  hal_irq_disable_all();
  hal_cpu_halt();
}

static int nxhal_memory_protocol_valid(
    const struct nxhal_memory_protocol_v1 *protocol) {
  if (protocol == NULL)
    return NXHAL_MEMORY_BIND_E_NULL;
  if (protocol->abi_version != NXHAL_MEMORY_PROTOCOL_ABI_VERSION)
    return NXHAL_MEMORY_BIND_E_VERSION;
  if (protocol->struct_size != sizeof(struct nxhal_memory_protocol_v1))
    return NXHAL_MEMORY_BIND_E_SIZE;
  if (protocol->flags != 0U || protocol->reserved0 != 0U)
    return NXHAL_MEMORY_BIND_E_RESERVED;
  if (protocol->alloc_page == NULL || protocol->alloc_pages == NULL ||
      protocol->free_page == NULL || protocol->free_pages == NULL)
    return NXHAL_MEMORY_BIND_E_MISSING;
  return NXHAL_MEMORY_BIND_OK;
}

int nxhal_memory_bind(const struct nxhal_memory_protocol_v1 *protocol) {
  int valid = nxhal_memory_protocol_valid(protocol);
  uint32_t expected = NXHAL_MEMORY_UNBOUND;

  if (valid != NXHAL_MEMORY_BIND_OK)
    return valid;
  if (!__atomic_compare_exchange_n(&memory_state, &expected,
                                   NXHAL_MEMORY_BINDING, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return NXHAL_MEMORY_BIND_E_STATE;

  memory_protocol.abi_version = protocol->abi_version;
  memory_protocol.struct_size = protocol->struct_size;
  memory_protocol.flags = protocol->flags;
  memory_protocol.reserved0 = protocol->reserved0;
  memory_protocol.alloc_page = protocol->alloc_page;
  memory_protocol.alloc_pages = protocol->alloc_pages;
  memory_protocol.free_page = protocol->free_page;
  memory_protocol.free_pages = protocol->free_pages;
  __atomic_store_n(&memory_state, NXHAL_MEMORY_BOUND, __ATOMIC_RELEASE);
  return NXHAL_MEMORY_BIND_OK;
}

int nxhal_memory_is_bound(void) {
  return __atomic_load_n(&memory_state, __ATOMIC_ACQUIRE) ==
         NXHAL_MEMORY_BOUND;
}

static const struct nxhal_memory_protocol_v1 *nxhal_memory_protocol(void) {
  if (!nxhal_memory_is_bound())
    nxhal_memory_stop();
  return &memory_protocol;
}

void *nxhal_memory_alloc_page(void) {
  return nxhal_memory_protocol()->alloc_page();
}
void *nxhal_memory_alloc_pages(size_t count) {
  return nxhal_memory_protocol()->alloc_pages(count);
}
void nxhal_memory_free_page(void *page) {
  nxhal_memory_protocol()->free_page(page);
}
void nxhal_memory_free_pages(void *page, size_t count) {
  nxhal_memory_protocol()->free_pages(page, count);
}
