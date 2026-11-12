/* Transitional v1 surface storage. Desktop policy is not part of object core.
 * The existing snapshot and move ABI remains available during broker migration. */
#include <kernel/resource.h>
#include <kernel/object.h>
#include <kernel/graphics.h>
#include <kernel/hal_uaccess.h>
#include <kernel/string.h>
#include <surface.h>
#include <os1_compositor.h>

static long window_read(void *state, void *user, size_t size) {
  (void)state;
  if (size != sizeof(struct os1_window_snapshot_v1)) return -EINVAL;
  return compositor_window_snapshot(user);
}

static long window_write(void *state, const void *user, size_t size) {
  struct os1_surface_move_v1 request;
  (void)state;
  if (size != sizeof(request)) return -EINVAL;
  if (hal_copy_from_user(&request, user, size)) return -EFAULT;
  if (request.abi_version != OS1_SURFACE_ABI_VERSION ||
      request.struct_size != sizeof(request) || request.id <= 0) return -EINVAL;
  compositor_move_window(request.id, request.x, request.y);
  return 0;
}

static const struct nx_resource_ops window_ops = {
    .read = window_read, .write = window_write};

int nx_window_resource_acquire(const char *path, uint32_t rights,
                               struct kobject *object) {
  if (strcmp(path, OS1_RESOURCE_WINDOW_STORAGE)) return -ENOENT;
  const uint32_t allowed = OS1_RIGHT_READ | OS1_RIGHT_WRITE |
                          OS1_RIGHT_TRANSFER | OS1_RIGHT_DUPLICATE;
  if (rights & ~allowed) return -EPERM;
  object->resource_ops = &window_ops;
  return 0;
}
