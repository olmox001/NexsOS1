#ifndef OS1_BUFFER_H
#define OS1_BUFFER_H
#include <surface.h>
#include <stddef.h>
/* Each process maps the same storage at its own virtual address. Pointers are
 * local views; only handles transfer authority between processes. */
struct os1_buffer_view {
  void *address;
  size_t bytes;
  uint64_t pages;
};
long OS1_buffer_create(size_t bytes);
int OS1_buffer_map(int handle, int writable, struct os1_buffer_view *view);
int OS1_buffer_unmap(int handle, struct os1_buffer_view *view);
#endif
