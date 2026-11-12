#include <hal/boot_memory.h>

#include <kernel/boot_memory.h>
#include <kernel/platform.h>
#include <kernel/printk.h>

#define BOOT_MEMORY_MODULE_OVERLAYS 1U
#define BOOT_MEMORY_CAPACITY \
  (NXHAL_BOOT_MEMORY_MAX_RECORDS + BOOT_MEMORY_MODULE_OVERLAYS)

static struct mem_region boot_regions[BOOT_MEMORY_CAPACITY];
static size_t boot_region_count;
static int boot_memory_ready;

static uint32_t boot_memory_type(uint32_t type) {
  if (type == (uint32_t)NXHAL_BOOT_MEMORY_USABLE)
    return MEM_REGION_USABLE;
  if (type == (uint32_t)NXHAL_BOOT_MEMORY_ACPI_RECLAIMABLE ||
      type == (uint32_t)NXHAL_BOOT_MEMORY_ACPI_NVS)
    return MEM_REGION_ACPI;
  if (type == (uint32_t)NXHAL_BOOT_MEMORY_MMIO)
    return MEM_REGION_MMIO;
  return MEM_REGION_RESERVED;
}

static void boot_memory_store(size_t index, uint64_t base, uint64_t size,
                              uint32_t type) {
  if (size == 0U || base > UINT64_MAX - size)
    panic("BOOT-MEM: invalid region[%zu] base=0x%lx size=0x%lx",
          index, base, size);
  boot_regions[index].base = base;
  boot_regions[index].size = size;
  boot_regions[index].type = type;
}

static int boot_memory_load_hal(void) {
  uint32_t count = nxhal_boot_memory_count();

  if (count == 0U)
    return 0;
  if (count > NXHAL_BOOT_MEMORY_MAX_RECORDS)
    panic("BOOT-MEM: HAL record count %u exceeds ABI v1 capacity", count);
  for (uint32_t index = 0U; index < count; ++index) {
    struct nxhal_boot_memory_region region;
    enum nxhal_boot_memory_status status =
        nxhal_boot_memory_get(index, &region);

    if (status != NXHAL_BOOT_MEMORY_OK)
      panic("BOOT-MEM: HAL record %u rejected status=%d", index, status);
    boot_memory_store(index, region.base, region.length,
                      boot_memory_type(region.type));
  }
  boot_region_count = (size_t)count;
  pr_info("BOOT-MEM: selected NX-HAL firmware map (%u records)\n", count);
  return 1;
}

static void boot_memory_load_legacy(void) {
  size_t count = 0U;
  struct mem_region *regions = arch_platform_get_mem_regions(&count);

  if (regions == NULL || count == 0U)
    panic("%s", "BOOT-MEM: platform returned no validated memory regions");
  if (count > NXHAL_BOOT_MEMORY_MAX_RECORDS)
    panic("BOOT-MEM: compatibility map has %zu records; HCD v1 snapshot limit",
          count);
  for (size_t index = 0U; index < count; ++index)
    boot_memory_store(index, regions[index].base, regions[index].size,
                      regions[index].type);
  boot_region_count = count;
  pr_info("BOOT-MEM: selected compatibility platform map (%zu records)\n",
          count);
}

static void boot_memory_add_module(void) {
  uint64_t base = 0U;
  uint64_t size = 0U;

  if (!arch_platform_get_boot_module(&base, &size))
    return;
  if (boot_region_count >= BOOT_MEMORY_CAPACITY)
    panic("%s", "BOOT-MEM: no representation slot for boot module");
  boot_memory_store(boot_region_count, base, size, MEM_REGION_RESERVED);
  ++boot_region_count;
}

void boot_memory_init(void) {
  if (boot_memory_ready)
    panic("%s", "BOOT-MEM: snapshot initialized twice");
  if (!boot_memory_load_hal())
    boot_memory_load_legacy();
  boot_memory_add_module();
  boot_memory_ready = 1;
}

struct mem_region *boot_memory_regions(size_t *count) {
  if (count == NULL)
    return NULL;
  if (!boot_memory_ready) {
    *count = 0U;
    return NULL;
  }
  *count = boot_region_count;
  return boot_regions;
}
