#include <kernel/resource.h>
#include <kernel/object.h>
#include <kernel/hal_uaccess.h>
#include <kernel/string.h>
#include <drivers/input.h>
#include <surface.h>

static long input_read(void *state, void *user, size_t size) {
  struct os1_input_request_v1 request;
  if (size != sizeof(request)) return -EINVAL;
  if (hal_copy_from_user(&request, user, size)) return -EFAULT;
  if (request.abi_version != OS1_SURFACE_ABI_VERSION ||
      request.struct_size != sizeof(request) || request.reserved ||
      request.mode > OS1_INPUT_READ_WAIT) return -EINVAL;
  unsigned role = (unsigned)(uintptr_t)state;
  long result = OS1libkernel_input_read_raw(&request.event, role, request.mode);
  if (result > 0 && hal_copy_to_user(user, &request, size)) return -EFAULT;
  return result;
}

static const struct nx_resource_ops input_ops = {.read = input_read};

int nx_input_resource_acquire(const char *path, uint32_t rights,
                              struct kobject *object) {
  unsigned role;
  if (!strcmp(path, OS1_RESOURCE_KEYBOARD)) role = OS1_INPUT_ROLE_KEYBOARD;
  else if (!strcmp(path, OS1_RESOURCE_POINTER)) role = OS1_INPUT_ROLE_MOUSE;
  else return -ENOENT;
  const uint32_t allowed = OS1_RIGHT_READ | OS1_RIGHT_TRANSFER | OS1_RIGHT_DUPLICATE;
  if (!(rights & OS1_RIGHT_READ) || (rights & ~allowed)) return -EPERM;
  object->resource_ops = &input_ops;
  object->resource_state = (void *)(uintptr_t)role;
  return 0;
}
