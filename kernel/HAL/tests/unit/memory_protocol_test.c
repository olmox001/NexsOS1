#include <hal/memory_protocol.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static unsigned alloc_page_calls;
static unsigned alloc_pages_calls;
static unsigned free_page_calls;
static unsigned free_pages_calls;
static size_t last_alloc_count;
static size_t last_free_count;
static void *last_free_page;

void hal_irq_disable_all(void) {
  abort();
}

void hal_cpu_halt(void) {
  abort();
}

static void *test_alloc_page(void) {
  ++alloc_page_calls;
  return (void *)(uintptr_t)(UINT64_C(0x1000) +
                             ((uint64_t)alloc_page_calls * UINT64_C(0x1000)));
}

static void *test_alloc_pages(size_t count) {
  ++alloc_pages_calls;
  last_alloc_count = count;
  return (void *)(uintptr_t)(UINT64_C(0x100000) +
                             ((uint64_t)count * UINT64_C(0x1000)));
}

static void test_free_page(void *page) {
  ++free_page_calls;
  last_free_page = page;
}

static void test_free_pages(void *page, size_t count) {
  ++free_pages_calls;
  last_free_page = page;
  last_free_count = count;
}

static struct nxhal_memory_protocol_v1 valid_protocol(void) {
  struct nxhal_memory_protocol_v1 protocol = {
      .abi_version = NXHAL_MEMORY_PROTOCOL_ABI_VERSION,
      .struct_size = sizeof(struct nxhal_memory_protocol_v1),
      .flags = 0U,
      .reserved0 = 0U,
      .alloc_page = test_alloc_page,
      .alloc_pages = test_alloc_pages,
      .free_page = test_free_page,
      .free_pages = test_free_pages,
  };
  return protocol;
}

int main(void) {
  struct nxhal_memory_protocol_v1 protocol = valid_protocol();
  void *page1;
  void *page2;
  void *pages1;
  void *pages2;

  if (nxhal_memory_bind(NULL) != NXHAL_MEMORY_BIND_E_NULL)
    return 1;

  protocol.abi_version = 0U;
  if (nxhal_memory_bind(&protocol) != NXHAL_MEMORY_BIND_E_VERSION)
    return 2;

  protocol = valid_protocol();
  protocol.struct_size -= 1U;
  if (nxhal_memory_bind(&protocol) != NXHAL_MEMORY_BIND_E_SIZE)
    return 3;

  protocol = valid_protocol();
  protocol.flags = 1U;
  if (nxhal_memory_bind(&protocol) != NXHAL_MEMORY_BIND_E_RESERVED)
    return 4;

  protocol = valid_protocol();
  protocol.alloc_page = NULL;
  if (nxhal_memory_bind(&protocol) != NXHAL_MEMORY_BIND_E_MISSING)
    return 5;

  protocol = valid_protocol();
  protocol.alloc_pages = NULL;
  if (nxhal_memory_bind(&protocol) != NXHAL_MEMORY_BIND_E_MISSING)
    return 6;

  protocol = valid_protocol();
  protocol.free_page = NULL;
  if (nxhal_memory_bind(&protocol) != NXHAL_MEMORY_BIND_E_MISSING)
    return 7;

  protocol = valid_protocol();
  protocol.free_pages = NULL;
  if (nxhal_memory_bind(&protocol) != NXHAL_MEMORY_BIND_E_MISSING)
    return 8;

  if (nxhal_memory_is_bound())
    return 9;

  protocol = valid_protocol();
  if (nxhal_memory_bind(&protocol) != NXHAL_MEMORY_BIND_OK)
    return 10;
  if (!nxhal_memory_is_bound())
    return 11;
  if (nxhal_memory_bind(&protocol) != NXHAL_MEMORY_BIND_E_STATE)
    return 12;

  page1 = nxhal_memory_alloc_page();
  page2 = nxhal_memory_alloc_page();
  if (page1 == NULL || page2 == NULL || page1 == page2 ||
      alloc_page_calls != 2U)
    return 13;

  pages1 = nxhal_memory_alloc_pages(3U);
  if (pages1 == NULL || alloc_pages_calls != 1U || last_alloc_count != 3U)
    return 14;
  pages2 = nxhal_memory_alloc_pages(17U);
  if (pages2 == NULL || pages1 == pages2 || alloc_pages_calls != 2U ||
      last_alloc_count != 17U)
    return 15;

  nxhal_memory_free_page(page1);
  if (free_page_calls != 1U || last_free_page != page1)
    return 16;
  nxhal_memory_free_page(page2);
  if (free_page_calls != 2U || last_free_page != page2)
    return 17;

  nxhal_memory_free_pages(pages1, 3U);
  if (free_pages_calls != 1U || last_free_page != pages1 ||
      last_free_count != 3U)
    return 18;
  nxhal_memory_free_pages(pages2, 17U);
  if (free_pages_calls != 2U || last_free_page != pages2 ||
      last_free_count != 17U)
    return 19;

  return 0;
}
