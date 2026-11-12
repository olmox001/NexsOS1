/* Real renderer storage lifecycle over the normal resource capability ABI. */
#include <os1.h>
#include <os1_gfx_buffer.h>
#include <os1_surface.h>
#include <stdio.h>

static int storage_check(struct os1_gfx_buffer *buffer) {
  if (OS1_gfx_buffer_reserve(buffer, 1024) != 0) return 1;
  uint32_t *pixels = buffer->active.view.address;
  pixels[0] = UINT32_C(0xa314975e);
  int first = buffer->active.handle;
  if (OS1_gfx_buffer_reserve(buffer, 512) != 0 ||
      buffer->active.handle != first ||
      buffer->active.view.address != pixels ||
      pixels[0] != UINT32_C(0xa314975e)) return 2;
  if (OS1_gfx_buffer_reserve(buffer, SIZE_MAX) != -EOVERFLOW ||
      buffer->active.handle != first || pixels[0] != UINT32_C(0xa314975e))
    return 3;
  /* Growth is a new scratch surface, committed before retiring the old one. */
  if (OS1_gfx_buffer_reserve(buffer, 4096) != 0 ||
      buffer->active.handle == first || buffer->retired.handle != -1 ||
      buffer->active.view.bytes < 4096 * sizeof(uint32_t)) return 4;
  pixels = buffer->active.view.address;
  pixels[4095] = UINT32_C(0x5eadc128);
  if (pixels[4095] != UINT32_C(0x5eadc128)) return 5;
  return 0;
}

static int own_window_size(int win, int width, int height) {
  char name[16];
  int n = snprintf(name, sizeof(name), "%d", win);
  if (n <= 0 || (size_t)n >= sizeof(name)) return -EINVAL;
  long handle = OS1low_handle_create(OS1_NS_WIN, name, OS1_RIGHT_READ,
                                     OBJ_TYPE_WINDOW);
  if (handle < 0) return (int)handle;
  struct window_info info;
  long received = OS1_object_read((int)handle, &info, sizeof(info));
  long closed = OS1low_handle_close((int)handle);
  if (received != (long)sizeof(info) || closed != 0) return -EIO;
  return info.w == width && info.h == height ? 0 : -EINVAL;
}

/* Logical adoption is an owner operation, not a new WM request. It must
 * not echo stale sizes into the same mailbox during a drag. */
static int resize_check(void) {
  int win = create_window(40, 80, 96, 80, "resize-contract");
  if (win < 0) return 1;
  int status = 0;
  for (unsigned step = 0U; step < 32U && status == 0; ++step) {
    int width = step % 2U ? 96 : 128;
    int height = step % 2U ? 80 : 96;
    if (OS1_window_resize(win, width, height) != 0) status = 2;
    /* The USER test queries its WINDOW capability. The scene/pixel resource
     * remains privileged; this test must not require compositor authority. */
    if (own_window_size(win, width, height) != 0) status = 3;
    struct ipc_message event = {0};
    long received = OS1low_ipc_try_recv(-1, &event);
    if (received == 0 && event.type == IPC_TYPE_RESIZE) status = 4;
    if (received != 0 && received != -EAGAIN) status = 5;
  }
  destroy_window(win);
  return status;
}

int main(void) {
  struct os1_gfx_buffer buffer;
  OS1_gfx_buffer_init(&buffer);
  int rc = storage_check(&buffer);
  int released = OS1_gfx_buffer_release(&buffer);
  int repeated = OS1_gfx_buffer_release(&buffer);
  if (released != 0 || repeated != 0 || buffer.active.handle != -1 ||
      buffer.active.view.address != NULL || buffer.retired.handle != -1)
    rc = 6;
  if (printf("[gfxbuffertest] scratch reserve/reuse/grow/overflow/release: %s (%d)\n",
             rc == 0 ? "PASS" : "FAIL", rc) < 0) return 1;
  int resized = resize_check();
  int failures = (rc != 0) + (resized != 0);
  if (printf("[gfxbuffertest] owner resize without feedback: %s (%d)\n",
             resized == 0 ? "PASS" : "FAIL", resized) < 0) return 1;
  if (printf("[gfxbuffertest] done: %d/2 passed, %d failure(s)\n",
             2 - failures, failures) < 0) return 1;
  return failures;
}
