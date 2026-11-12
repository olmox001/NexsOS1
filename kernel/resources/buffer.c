/* Shared page catalogs: no pixel formats, windows, z-order or raster access.
 * Authority comes from the existing object handle; addresses name mappings in
 * the caller only. A mapping retains storage independently of its handle.
 *
 * Limits: object/mapping slots follow the process x handle security budget;
 * backing-page budget is one eighth of discovered usable RAM. Work advances in
 * one hardware-page-sized catalog quantum. No runtime general-heap allocation.
 */
#include <kernel/resource.h>
#include <kernel/object.h>
#include <kernel/hal_uaccess.h>
#include <kernel/pmm.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/vmm.h>
#include <surface.h>

#define BUFFER_SLOTS (MAX_PROCESSES * NPROC_HANDLES)
#define BUFFER_RAM_BUDGET_DIVISOR 8U
#define BUFFER_QUANTUM (PAGE_SIZE / sizeof(uint64_t))
/* Private PGD entry zero covers these addresses on both supported ISAs.
 * This arena is disjoint from ELF, the sbrk heap and the user stack. */
#define BUFFER_VA_FIRST UINT64_C(0x1000000000)
#define BUFFER_VA_END UINT64_C(0x2000000000)
#define CATALOG_RESERVED UINT64_MAX

struct buffer_storage {
  int references; /* -1 while final release is reclaiming pages */
  unsigned busy;
  size_t first, pages, allocated;
};
struct buffer_mapping {
  struct process *owner;
  struct buffer_storage *storage;
  uint64_t address;
  size_t mapped;
  unsigned writable;
};
static struct buffer_storage buffers[BUFFER_SLOTS];
static struct buffer_mapping mappings[BUFFER_SLOTS];
static uint64_t *catalog;
static size_t catalog_pages;
static DEFINE_SPINLOCK(catalog_lock);

void nx_buffer_init(void) {
  uint64_t budget = pmm_get_usable_pages() / BUFFER_RAM_BUDGET_DIVISOR;
  if (!budget || budget > SIZE_MAX / sizeof(*catalog) ||
      budget > UINT64_MAX / PAGE_SIZE)
    panic("%s", "BUFFER: invalid RAM budget");
  size_t bytes = (size_t)budget * sizeof(*catalog);
  size_t metadata_pages = (bytes + PAGE_SIZE - 1U) / PAGE_SIZE;
  catalog = pmm_alloc_pages(metadata_pages);
  if (!catalog) panic("%s", "BUFFER: catalog bootstrap allocation failed");
  memset(catalog, 0, metadata_pages * PAGE_SIZE);
  catalog_pages = (size_t)budget;
}

static int buffer_reserve(struct buffer_storage *buffer, uint64_t bytes,
                           uint64_t *work) {
  if (!bytes || bytes > (uint64_t)catalog_pages * PAGE_SIZE) return -EINVAL;
  size_t count = (size_t)((bytes + PAGE_SIZE - 1U) / PAGE_SIZE);
  if (buffer->pages) return buffer->pages == count ? 0 : -EINVAL;
  uint64_t flags;
  size_t run = 0;
  spin_lock_irqsave(&catalog_lock, &flags);
  for (size_t i = 0; i < catalog_pages; ++i) {
    ++*work;
    run = __atomic_load_n(&catalog[i], __ATOMIC_RELAXED) == 0 ? run + 1U : 0;
    if (run != count) continue;
    buffer->first = i + 1U - count;
    buffer->pages = count;
    for (size_t j = 0; j < count; ++j)
      catalog[buffer->first + j] = CATALOG_RESERVED;
    *work += count;
    spin_unlock_irqrestore(&catalog_lock, flags);
    return 0;
  }
  spin_unlock_irqrestore(&catalog_lock, flags);
  return -ENOSPC;
}

static int buffer_allocate(struct buffer_storage *buffer,
                            struct os1_buffer_request_v1 *request) {
  int status = buffer_reserve(buffer, request->byte_length, &request->work_units);
  if (status) return status;
  for (size_t step = 0; step < BUFFER_QUANTUM; ++step) {
    if (buffer->allocated == buffer->pages) break;
    void *page = pmm_alloc_page();
    ++request->work_units;
    if (!page) return -ENOMEM;
    __atomic_store_n(&catalog[buffer->first + buffer->allocated],
                     virt_to_phys(page), __ATOMIC_RELEASE);
    ++buffer->allocated;
  }
  request->completed = buffer->allocated;
  return 0;
}

static void buffer_put(struct buffer_storage *buffer) {
  uint64_t flags;
  spin_lock_irqsave(&catalog_lock, &flags);
  if (buffer->references <= 0)
    panic("%s", "BUFFER: release without a live reference");
  --buffer->references;
  int last = buffer->references == 0;
  if (last) buffer->references = -1;
  spin_unlock_irqrestore(&catalog_lock, flags);
  if (!last) return;
  if (buffer->allocated > buffer->pages || buffer->first > catalog_pages ||
      buffer->pages > catalog_pages - buffer->first)
    panic("%s", "BUFFER: corrupt page catalog extent");
  for (size_t i = 0; i < buffer->allocated; ++i)
    pmm_free_page(phys_to_virt(catalog[buffer->first + i]));
  spin_lock_irqsave(&catalog_lock, &flags);
  for (size_t i = 0; i < buffer->pages; ++i) catalog[buffer->first + i] = 0;
  memset(buffer, 0, sizeof(*buffer));
  spin_unlock_irqrestore(&catalog_lock, flags);
}

static void buffer_release(void *state) {
  buffer_put(state);
}

static struct buffer_mapping *mapping_find(struct buffer_storage *buffer,
                                            uint64_t address, uint64_t *work) {
  uint64_t flags;
  struct buffer_mapping *found = NULL;
  spin_lock_irqsave(&catalog_lock, &flags);
  for (size_t i = 0; i < BUFFER_SLOTS; ++i) {
    ++*work;
    if (mappings[i].owner == current_process &&
        mappings[i].storage == buffer && mappings[i].address == address)
      found = &mappings[i];
    if (found) break;
  }
  spin_unlock_irqrestore(&catalog_lock, flags);
  return found;
}

static int mapping_reserve_address(struct buffer_mapping *mapping) {
  struct process *owner = mapping->owner;
  uint64_t flags;
  spin_lock_irqsave(&owner->mm_lock, &flags);
  uint64_t address = owner->shared_next_va;
  if (!address) address = BUFFER_VA_FIRST;
  uint64_t bytes = (uint64_t)mapping->storage->pages * PAGE_SIZE;
  int valid = address >= BUFFER_VA_FIRST && address <= BUFFER_VA_END &&
              bytes <= BUFFER_VA_END - address;
  if (valid) {
    mapping->address = address;
    owner->shared_next_va = address + bytes;
  }
  spin_unlock_irqrestore(&owner->mm_lock, flags);
  return valid ? 0 : -ENOSPC;
}

static struct buffer_mapping *mapping_create(struct buffer_storage *buffer,
                                              unsigned writable,
                                              uint64_t *work) {
  uint64_t flags;
  struct buffer_mapping *mapping = NULL;
  unsigned owned = 0;
  spin_lock_irqsave(&catalog_lock, &flags);
  for (size_t i = 0; i < BUFFER_SLOTS; ++i) {
    ++*work;
    if (mappings[i].owner == current_process) ++owned;
    if (!mappings[i].owner && !mapping) mapping = &mappings[i];
  }
  if (owned >= NPROC_HANDLES) mapping = NULL;
  if (mapping) {
    mapping->owner = current_process;
    mapping->storage = buffer;
    mapping->mapped = 0;
    mapping->address = 0;
    mapping->writable = writable;
    ++buffer->references;
  }
  spin_unlock_irqrestore(&catalog_lock, flags);
  return mapping;
}

static void mapping_retire(struct buffer_mapping *mapping) {
  struct buffer_storage *buffer = mapping->storage;
  uint64_t flags;
  spin_lock_irqsave(&catalog_lock, &flags);
  memset(mapping, 0, sizeof(*mapping));
  spin_unlock_irqrestore(&catalog_lock, flags);
  buffer_put(buffer);
}

static void mapping_unmap_chunk(struct buffer_mapping *mapping,
                                 uint64_t *work) {
  for (size_t step = 0; step < BUFFER_QUANTUM && mapping->mapped; ++step) {
    --mapping->mapped;
    uint64_t address = mapping->address + mapping->mapped * PAGE_SIZE;
    vmm_unmap_page_locked(mapping->owner, address);
    ++*work;
  }
}

static int mapping_map_chunk(struct buffer_mapping *mapping, uint64_t *work) {
  struct buffer_storage *buffer = mapping->storage;
  struct process *owner = mapping->owner;
  uint64_t attributes = mapping->writable ? PAGE_USER_DATA : PAGE_USER_RO;
  for (size_t step = 0; step < BUFFER_QUANTUM; ++step) {
    if (mapping->mapped == buffer->pages) break;
    uint64_t address = mapping->address + mapping->mapped * PAGE_SIZE;
    uint64_t physical = catalog[buffer->first + mapping->mapped];
    uint64_t flags;
    spin_lock_irqsave(&owner->mm_lock, &flags);
    int status = vmm_get_phys(owner->page_table, address) ? -EEXIST :
        vmm_map_page(owner->page_table, address, physical, attributes);
    spin_unlock_irqrestore(&owner->mm_lock, flags);
    ++*work;
    if (status) return status;
    ++mapping->mapped;
  }
  return 0;
}

static int buffer_map(struct buffer_storage *buffer,
                       struct os1_buffer_request_v1 *request, int write) {
  unsigned writable = request->flags & OS1_BUFFER_WRITABLE;
  if (writable && !write) return -EPERM;
  if (!request->address && request->page_offset) return -EINVAL;
  if (!buffer->pages || buffer->allocated != buffer->pages) return -EAGAIN;
  struct buffer_mapping *mapping = request->address ?
      mapping_find(buffer, request->address, &request->work_units) :
      mapping_create(buffer, writable, &request->work_units);
  if (!mapping) return request->address ? -EINVAL : -ENOSPC;
  if (!mapping->address) {
    int status = mapping_reserve_address(mapping);
    if (status) { mapping_retire(mapping); return status; }
  }
  if (mapping->writable != writable || request->page_offset != mapping->mapped)
    return -EINVAL;
  request->address = mapping->address;
  int status = mapping_map_chunk(mapping, &request->work_units);
  request->completed = mapping->mapped;
  return status;
}

static int buffer_unmap(struct buffer_storage *buffer,
                         struct os1_buffer_request_v1 *request) {
  struct buffer_mapping *mapping =
      mapping_find(buffer, request->address, &request->work_units);
  if (!mapping) return -EINVAL;
  mapping_unmap_chunk(mapping, &request->work_units);
  request->completed = mapping->mapped;
  if (!mapping->mapped) mapping_retire(mapping);
  return 0;
}

static int buffer_dispatch(struct buffer_storage *buffer,
                            struct os1_buffer_request_v1 *request, int write) {
  if (request->operation == OS1_BUFFER_INFO) {
    request->completed = buffer->allocated;
    return 0;
  }
  if (request->operation == OS1_BUFFER_ALLOCATE)
    return write ? buffer_allocate(buffer, request) : -EPERM;
  if (request->operation == OS1_BUFFER_MAP)
    return buffer_map(buffer, request, write);
  if (request->operation == OS1_BUFFER_UNMAP)
    return buffer_unmap(buffer, request);
  return -EINVAL;
}

static long buffer_request(void *state, void *user, size_t size, int write) {
  struct buffer_storage *buffer = state;
  struct os1_buffer_request_v1 request;
  if (size != sizeof(request) || !current_process) return -EINVAL;
  if (hal_copy_from_user(&request, user, size)) return -EFAULT;
  if (request.abi_version != OS1_SURFACE_ABI_VERSION ||
      request.struct_size != size || (request.flags & ~OS1_BUFFER_WRITABLE))
    return -EINVAL;
  if (__atomic_exchange_n(&buffer->busy, 1, __ATOMIC_ACQUIRE)) return -EBUSY;
  request.work_units = 0;
  request.hardware_cycles = OS1_BUFFER_ACCOUNT_UNKNOWN;
  int status = buffer_dispatch(buffer, &request, write);
  request.total_pages = buffer->pages;
  request.byte_length = buffer->pages * PAGE_SIZE;
  if (hal_copy_to_user(user, &request, size)) {
    if (request.operation == OS1_BUFFER_MAP && request.address) {
      struct buffer_mapping *mapping = mapping_find(buffer, request.address,
                                                     &request.work_units);
      if (mapping && mapping->mapped <= BUFFER_QUANTUM) {
        mapping_unmap_chunk(mapping, &request.work_units);
        mapping_retire(mapping);
      }
    }
    status = -EFAULT;
  }
  __atomic_store_n(&buffer->busy, 0, __ATOMIC_RELEASE);
  return status;
}

static long buffer_read(void *state, void *user, size_t size) {
  return buffer_request(state, user, size, 0);
}

static long buffer_write(void *state, const void *user, size_t size) {
  /* Object write's descriptor is an in/out control record, never pixel data. */
  return buffer_request(state, (void *)user, size, 1);
}

static const struct nx_resource_ops buffer_ops = {
    .read = buffer_read, .write = buffer_write, .release = buffer_release};

int nx_buffer_resource_acquire(uint32_t rights, struct kobject *object) {
  const uint32_t allowed = OS1_RIGHT_READ | OS1_RIGHT_WRITE |
                          OS1_RIGHT_DUPLICATE | OS1_RIGHT_TRANSFER;
  if (!catalog) return -EAGAIN;
  if (!(rights & (OS1_RIGHT_READ | OS1_RIGHT_WRITE)) || (rights & ~allowed))
    return -EPERM;
  uint64_t flags;
  spin_lock_irqsave(&catalog_lock, &flags);
  for (size_t i = 0; i < BUFFER_SLOTS; ++i) {
    if (buffers[i].references) continue;
    buffers[i].references = 1;
    object->resource_ops = &buffer_ops;
    object->resource_state = &buffers[i];
    spin_unlock_irqrestore(&catalog_lock, flags);
    return 0;
  }
  spin_unlock_irqrestore(&catalog_lock, flags);
  return -ENOSPC;
}

void nx_buffer_process_detach(struct process *process) {
  /* Reaper calls this after all execution on this address space has stopped.
   * Remove shared PTEs before vmm_destroy_pgd frees private user pages. */
  for (size_t i = 0; i < BUFFER_SLOTS; ++i) {
    uint64_t flags;
    spin_lock_irqsave(&catalog_lock, &flags);
    int owned = mappings[i].owner == process;
    spin_unlock_irqrestore(&catalog_lock, flags);
    if (!owned) continue;
    uint64_t work = 0;
    size_t chunks = (mappings[i].mapped + BUFFER_QUANTUM - 1U) / BUFFER_QUANTUM;
    for (size_t step = 0; step < chunks; ++step)
      mapping_unmap_chunk(&mappings[i], &work);
    mapping_retire(&mappings[i]);
  }
}
