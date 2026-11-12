/* Hardware/surface resource provider. No executable names or userland roles. */
#include <kernel/resource.h>
#include <kernel/object.h>
#include <kernel/graphics.h>
#include <kernel/hal_uaccess.h>
#include <kernel/kmalloc.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <surface.h>
#include <nxpax.h>
#include <os1_compositor.h>

#define SURFACE_MAX_PIXELS (16U * 1024U * 1024U)
static uint64_t surface_generation = 1;
static struct wait_queue_head surface_waiters = {
    .task_list = LIST_HEAD_INIT(surface_waiters.task_list),
    .lock = SPINLOCK_INIT};

struct display_staging {
  uint32_t *pixels;
  size_t capacity;
  unsigned busy;
};

void nx_surface_changed(void) {
  __atomic_add_fetch(&surface_generation, 1, __ATOMIC_RELEASE);
  wake_up_all(&surface_waiters);
}

static int surface_unchanged(void *argument) {
  const uint64_t *generation = argument;
  return __atomic_load_n(&surface_generation, __ATOMIC_ACQUIRE) == *generation;
}

static long surface_wait(void *state, long argument) {
  uint64_t generation = (uint64_t)argument;
  (void)state;
  kthread_block(&surface_waiters, surface_unchanged, &generation);
  return 0;
}

static long surface_read(void *state, void *user, size_t size) {
  struct os1_surface_state_v1 info = {0};
  int width, height;
  (void)state;
  if (size != sizeof(info)) return -EINVAL;
  compositor_get_size(&width, &height);
  info.abi_version = OS1_SURFACE_ABI_VERSION;
  info.struct_size = sizeof(info);
  info.generation = __atomic_load_n(&surface_generation, __ATOMIC_ACQUIRE);
  info.monotonic_ns = OS1libkernel_time_monotonic_ns();
  info.width = (uint32_t)width;
  info.height = (uint32_t)height;
  /* No invented refresh period. This backend acknowledges transfer completion;
   * vblank pacing requires a provider exposing an actual display clock. */
  info.timing = OS1_SURFACE_TIMING_COMPLETION;
  return hal_copy_to_user(user, &info, sizeof(info)) == 0 ? (long)sizeof(info) :
                                                         -EFAULT;
}

static long surface_signal(void *state, const void *user, size_t size) {
  (void)state;
  (void)user;
  if (size != 0) return -EINVAL;
  nx_surface_changed();
  return 0;
}

static int present_validate(const struct os1_compositor_present_v1 *p) {
  if (p->abi_version != OS1_COMPOSITOR_ABI_V1 || p->struct_size < sizeof(*p) ||
      p->width <= 0 || p->height <= 0 || p->stride != p->width || !p->pixels)
    return -EINVAL;
  if ((size_t)p->width > SURFACE_MAX_PIXELS / (size_t)p->height)
    return -EINVAL;
  size_t count = (size_t)p->width * (size_t)p->height;
  return p->pixel_capacity >= count ? 0 : -EINVAL;
}

static long display_copy_present(struct display_staging *staging,
                                  const struct os1_compositor_present_v1 *p) {
  size_t count = (size_t)p->width * (size_t)p->height;
  if (staging->capacity < count) {
    uint32_t *replacement = kmalloc(count * sizeof(*replacement));
    if (!replacement) return -ENOMEM;
    kfree(staging->pixels);
    staging->pixels = replacement;
    staging->capacity = count;
  }
  if (hal_copy_from_user(staging->pixels, p->pixels, count * sizeof(uint32_t)))
    return -EFAULT;
  return graphics_present_argb8888(staging->pixels, p->width, p->height,
                                   p->stride, count);
}

static long display_write(void *state, const void *user, size_t size) {
  struct display_staging *staging = state;
  struct os1_compositor_present_v1 present;
  if (size != sizeof(present)) return -EINVAL;
  if (hal_copy_from_user(&present, user, sizeof(present))) return -EFAULT;
  int valid = present_validate(&present);
  if (valid) return valid;
  /* A delegated/duplicated capability can be used by several CPUs. Reject
   * overlapping submissions instead of holding a spinlock across user copies,
   * allocations or GPU completion. Object pinning protects lifetime. */
  if (__atomic_exchange_n(&staging->busy, 1, __ATOMIC_ACQUIRE)) return -EBUSY;
  long result = display_copy_present(staging, &present);
  __atomic_store_n(&staging->busy, 0, __ATOMIC_RELEASE);
  return result;
}

static void display_release(void *state) {
  struct display_staging *staging = state;
  kfree(staging->pixels);
  kfree(staging);
}

static const struct nx_resource_ops surface_ops = {
    .read = surface_read, .write = surface_signal, .wait = surface_wait};
static const struct nx_resource_ops display_ops = {
    .read = surface_read, .write = display_write, .release = display_release};

int nx_resource_acquire(struct process *caller, const char *path,
                        uint32_t rights, struct kobject *object) {
  if (strcmp(path, OS1_NXPAX_RESOURCE) == 0)
    return nx_nxpax_resource_acquire(caller, rights, object);
  if (caller && strcmp(path, OS1_RESOURCE_BUFFER) == 0)
    return nx_buffer_resource_acquire(rights, object);
  const uint32_t allowed = OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_WAIT |
                          OS1_RIGHT_TRANSFER | OS1_RIGHT_DUPLICATE;
  if (!caller || !proc_is_privileged(caller) ||
      !proc_has_cap(caller, CAP_WINDOW) || (rights & ~allowed)) return -EPERM;
  if (strcmp(path, OS1_RESOURCE_SURFACE_EVENTS) == 0) {
    object->resource_ops = &surface_ops;
    return 0;
  }
  if (strcmp(path, OS1_RESOURCE_DISPLAY) != 0) {
    int result = nx_input_resource_acquire(path, rights, object);
    if (result != -ENOENT) return result;
    return nx_window_resource_acquire(path, rights, object);
  }
  if (rights & OS1_RIGHT_WAIT) return -EINVAL;
  struct display_staging *staging = kmalloc(sizeof(*staging));
  if (!staging) return -ENOMEM;
  memset(staging, 0, sizeof(*staging));
  object->resource_state = staging;
  object->resource_ops = &display_ops;
  return 0;
}
