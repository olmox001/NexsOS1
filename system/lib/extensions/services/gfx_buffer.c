/* Capability storage shared by renderers, display adapters and surface clients.
 * At most one active and one retired backing. No general heap allocation.
 * A failed retirement remains owned and retryable; no handle is discarded. */
#include <os1.h>
#include <os1_gfx_buffer.h>

void OS1_gfx_buffer_init(struct os1_gfx_buffer *buffer) {
  if (buffer == NULL) return;
  *buffer = (struct os1_gfx_buffer){0};
  buffer->active.handle = -1;
  buffer->retired.handle = -1;
}

static int backing_release(struct os1_gfx_backing *backing) {
  if (backing->handle < 0) return 0;
  if (backing->view.address != NULL) {
    int result = OS1_buffer_unmap(backing->handle, &backing->view);
    if (result != 0) return result;
  }
  long closed = OS1low_handle_close(backing->handle);
  if (closed < 0) return (int)closed;
  backing->handle = -1;
  return 0;
}

int OS1_gfx_buffer_release(struct os1_gfx_buffer *buffer) {
  if (buffer == NULL) return -EINVAL;
  int active = backing_release(&buffer->active);
  int retired = backing_release(&buffer->retired);
  return active != 0 ? active : retired;
}

int OS1_gfx_buffer_reserve(struct os1_gfx_buffer *buffer, size_t pixels) {
  if (buffer == NULL || pixels == 0) return -EINVAL;
  if (pixels > SIZE_MAX / sizeof(uint32_t)) return -EOVERFLOW;
  int retired = backing_release(&buffer->retired);
  if (retired != 0) return retired;
  size_t bytes = pixels * sizeof(uint32_t);
  if (buffer->active.view.address != NULL &&
      buffer->active.view.bytes >= bytes) return 0;
  long handle = OS1_buffer_create(bytes);
  if (handle < 0) return (int)handle;
  struct os1_gfx_backing replacement = {.handle = (int)handle};
  int mapped = OS1_buffer_map((int)handle, 1, &replacement.view);
  if (mapped != 0) {
    buffer->retired = replacement;
    int cleanup = backing_release(&buffer->retired);
    return cleanup != 0 ? cleanup : mapped;
  }
  buffer->retired = buffer->active;
  buffer->active = replacement;
  return backing_release(&buffer->retired);
}
