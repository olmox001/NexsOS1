#ifndef OS1_GFX_BUFFER_H
#define OS1_GFX_BUFFER_H
#include <os1_buffer.h>
#include <stdint.h>

/* Process-local mapped views of capability-owned memory. These pointers are
 * never wire authority. A peer receives an attenuated handle, then maps it.
 * Growth creates a fresh scratch surface; contents are not copied. Callers
 * redraw after growth. Equal/smaller reservations preserve contents.
 * Initialize with OS1_gfx_buffer_init before use; do not copy a live owner. */
struct os1_gfx_backing {
  int handle;
  struct os1_buffer_view view;
};
struct os1_gfx_buffer {
  struct os1_gfx_backing active;
  struct os1_gfx_backing retired;
};
void OS1_gfx_buffer_init(struct os1_gfx_buffer *buffer);
int OS1_gfx_buffer_reserve(struct os1_gfx_buffer *buffer, size_t pixels);
int OS1_gfx_buffer_release(struct os1_gfx_buffer *buffer);
#endif
