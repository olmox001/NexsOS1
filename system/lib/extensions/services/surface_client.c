#include <os1.h>
#include <os1_surface.h>

static int resource_open_cached(int *handle, const char *name, unsigned rights) {
  if (*handle >= 0) return 0;
  long result = OS1low_handle_create(OS1_NS_RESOURCE, name, rights,
                                     OBJ_TYPE_RESOURCE);
  if (result < 0) return (int)result;
  *handle = (int)result;
  return 0;
}

long OS1_surface_snapshot(struct os1_window_snapshot_v1 *snapshot) {
  static int handle = -1;
  int result = resource_open_cached(&handle, OS1_RESOURCE_WINDOW_STORAGE, OS1_RIGHT_READ);
  if (result) return result;
  return OS1_object_read(handle, snapshot, sizeof(*snapshot));
}

long OS1_surface_move(int id, int x, int y) {
  static int handle = -1;
  int result = resource_open_cached(&handle, OS1_RESOURCE_WINDOW_STORAGE, OS1_RIGHT_WRITE);
  if (result) return result;
  struct os1_surface_move_v1 request = {OS1_SURFACE_ABI_VERSION,
      sizeof(request), id, x, y};
  return OS1_object_write(handle, &request, sizeof(request));
}

long OS1_input_resource_read(struct os1_raw_input_event_v1 *event,
                             unsigned role, unsigned mode) {
  static int keyboard = -1, pointer = -1;
  if (!event || (role != OS1_INPUT_ROLE_KEYBOARD && role != OS1_INPUT_ROLE_MOUSE) ||
      mode > OS1_INPUT_READ_WAIT) return -EINVAL;
  int *handle = role == OS1_INPUT_ROLE_KEYBOARD ? &keyboard : &pointer;
  const char *name = role == OS1_INPUT_ROLE_KEYBOARD ? OS1_RESOURCE_KEYBOARD :
                                                       OS1_RESOURCE_POINTER;
  int status = resource_open_cached(handle, name, OS1_RIGHT_READ);
  if (status) return status;
  struct os1_input_request_v1 request = {0};
  request.abi_version = OS1_SURFACE_ABI_VERSION;
  request.struct_size = sizeof(request);
  request.mode = mode;
  long result = OS1_object_read(*handle, &request, sizeof(request));
  if (result > 0) *event = request.event;
  return result;
}

long OS1_surface_events_open(unsigned rights) {
  return OS1low_handle_create(OS1_NS_RESOURCE, OS1_RESOURCE_SURFACE_EVENTS,
                              rights, OBJ_TYPE_RESOURCE);
}

long OS1_surface_state(int handle, struct os1_surface_state_v1 *state) {
  if (!state) return -EINVAL;
  long result = OS1_object_read(handle, state, sizeof(*state));
  if (result < 0) return result;
  if (result != (long)sizeof(*state) ||
      state->abi_version != OS1_SURFACE_ABI_VERSION ||
      state->struct_size != sizeof(*state)) return -EINVAL;
  return 0;
}

long OS1_surface_wait(int handle, uint64_t generation) {
  return OS1_object_wait(handle, (long)generation);
}

long OS1_surface_invalidate(void) {
  long handle = OS1_surface_events_open(OS1_RIGHT_WRITE);
  if (handle < 0) return handle;
  long result = OS1_object_write((int)handle, 0, 0);
  OS1low_handle_close((int)handle);
  return result;
}

long OS1_surface_present(struct os1_compositor_present_v1 *present) {
  /* The native user runtime currently has one thread per process. The resource
   * itself serializes submissions from delegated handles on different CPUs. */
  static int display = -1;
  if (!present) return -EINVAL;
  if (display < 0) {
    long handle = OS1low_handle_create(OS1_NS_RESOURCE, OS1_RESOURCE_DISPLAY,
                                       OS1_RIGHT_WRITE, OBJ_TYPE_RESOURCE);
    if (handle < 0) return handle;
    display = (int)handle;
  }
  return OS1_object_write(display, present, sizeof(*present));
}
