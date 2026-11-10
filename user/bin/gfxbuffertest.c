/* Real renderer storage lifecycle over the normal resource capability ABI. */
#include <os1.h>
#include <os1_gfx_buffer.h>
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
  if (printf("[gfxbuffertest] done: %d/1 passed, %d failure(s)\n",
             rc == 0 ? 1 : 0, rc == 0 ? 0 : 1) < 0) return 1;
  return rc;
}
