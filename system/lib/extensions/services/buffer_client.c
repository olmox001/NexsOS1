#include <os1.h>
#include <os1_buffer.h>

static struct os1_buffer_request_v1 buffer_command(uint32_t operation) {
  struct os1_buffer_request_v1 request = {0};
  request.abi_version = OS1_SURFACE_ABI_VERSION;
  request.struct_size = sizeof(request);
  request.operation = operation;
  return request;
}

long OS1_buffer_create(size_t bytes) {
  if (!bytes) return -EINVAL;
  long handle = OS1low_handle_create(OS1_NS_RESOURCE, OS1_RESOURCE_BUFFER,
      OS1_RIGHT_READ | OS1_RIGHT_WRITE | OS1_RIGHT_DUPLICATE | OS1_RIGHT_TRANSFER,
      OBJ_TYPE_RESOURCE);
  if (handle < 0) return handle;
  struct os1_buffer_request_v1 request = buffer_command(OS1_BUFFER_ALLOCATE);
  request.byte_length = bytes;
  long result = OS1_object_write((int)handle, &request, sizeof(request));
  /* Kernel-reported page count bounds retries; every success must progress. */
  for (uint64_t step = 0; result == 0 && step < request.total_pages; ++step) {
    if (request.completed == request.total_pages) return handle;
    uint64_t previous = request.completed;
    result = OS1_object_write((int)handle, &request, sizeof(request));
    if (!result && request.completed <= previous) result = -EIO;
  }
  long closed = OS1low_handle_close((int)handle);
  if (result) return result;
  return closed < 0 ? closed : -EIO;
}

int OS1_buffer_unmap(int handle, struct os1_buffer_view *view) {
  if (!view || !view->address) return -EINVAL;
  struct os1_buffer_request_v1 request = buffer_command(OS1_BUFFER_UNMAP);
  request.address = (uint64_t)(uintptr_t)view->address;
  for (uint64_t step = 0; step <= view->pages; ++step) {
    long result = OS1_object_read(handle, &request, sizeof(request));
    if (result) return (int)result;
    if (!request.completed) {
      *view = (struct os1_buffer_view){0};
      return 0;
    }
  }
  return -EIO;
}

int OS1_buffer_map(int handle, int writable, struct os1_buffer_view *view) {
  if (!view) return -EINVAL;
  *view = (struct os1_buffer_view){0};
  struct os1_buffer_request_v1 request = buffer_command(OS1_BUFFER_INFO);
  long result = OS1_object_read(handle, &request, sizeof(request));
  if (result) return (int)result;
  if (!request.total_pages || request.completed != request.total_pages)
    return -EAGAIN;
  uint64_t limit = request.total_pages;
  request.operation = OS1_BUFFER_MAP;
  request.flags = writable ? OS1_BUFFER_WRITABLE : 0;
  for (uint64_t step = 0; step < limit; ++step) {
    result = writable ? OS1_object_write(handle, &request, sizeof(request)) :
                        OS1_object_read(handle, &request, sizeof(request));
    view->address = (void *)(uintptr_t)request.address;
    view->bytes = (size_t)request.byte_length;
    view->pages = request.completed;
    if (result || request.completed <= request.page_offset) break;
    if (request.completed == limit) return 0;
    request.page_offset = request.completed;
  }
  if (view->address) {
    int cleanup = OS1_buffer_unmap(handle, view);
    if (cleanup) return cleanup;
  }
  return result ? (int)result : -EIO;
}
