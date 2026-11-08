/* Versioned bridge between the legacy window objects and ring-3 compositord.
 * This is a transitional copy/present ABI: it keeps legacy window creation
 * intact while scene policy and pixel composition run in userland. */
#ifndef OS1_COMPOSITOR_H
#define OS1_COMPOSITOR_H

#include <stddef.h>
#include <stdint.h>

#define OS1_COMPOSITOR_ABI_V1 1u
#define OS1_COMPOSITOR_TITLE_MAX 64u

struct os1_window_snapshot_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  int32_t id;
  int32_t pid;
  int32_t x;
  int32_t y;
  int32_t width;
  int32_t height;
  int32_t draw_width;
  int32_t draw_height;
  int32_t z_order;
  uint32_t flags;
  uint32_t background_argb;
  uint32_t reserved;
  size_t pixel_count;
  uint32_t *pixels; /* user pointer, optional for metadata-only query */
  size_t pixel_capacity;
  char title[OS1_COMPOSITOR_TITLE_MAX];
};

struct os1_compositor_present_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  int32_t width;
  int32_t height;
  int32_t stride; /* pixels per row; v1 requires stride == width */
  uint32_t reserved;
  const uint32_t *pixels;
  size_t pixel_capacity;
};

#endif
