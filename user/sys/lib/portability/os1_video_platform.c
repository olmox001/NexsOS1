/* OS1 software-video implementation of the portability adapter. */
#include "os1_video_platform.h"

#include <errno.h>
#include <input.h>
#include <os1.h>
#include <stdlib.h>
#include <string.h>

#define OS1_VIDEO_ABI_VERSION 1u

int os1_video_query(struct os1_video_info *out) {
  if (!out)
    return -EINVAL;

  long packed = _sys_display_info();
  if (packed < 0)
    return (int)packed;

  out->abi_version = OS1_VIDEO_ABI_VERSION;
  out->backend = OS1_VIDEO_BACKEND_SOFTWARE;
  out->capabilities = OS1_VIDEO_CAP_ARGB8888 | OS1_VIDEO_CAP_WINDOW_RESIZE |
                      OS1_VIDEO_CAP_DAMAGE_PRESENT |
                      OS1_VIDEO_CAP_SOFTWARE_3D;
  out->width = (int)((packed >> 16) & 0xffff);
  out->height = (int)(packed & 0xffff);
  return 0;
}

int os1_video_window_create(int x, int y, int width, int height,
                            const char *title) {
  if (width <= 0 || height <= 0 || !title)
    return -EINVAL;
  return _sys_create_window(x, y, width, height, title);
}

void os1_video_window_destroy(int window) { _sys_destroy_window(window); }

int os1_video_window_resize(int window, int width, int height) {
  if (width <= 0 || height <= 0)
    return -EINVAL;
  return _sys_window_resize(window, width, height);
}

int os1_video_present_argb8888(int window, int x, int y, int width,
                               int height, const uint32_t *pixels,
                               size_t pixel_count) {
  return os1_video_present_argb8888_strided(window, x, y, width, height,
                                            pixels, (size_t)width,
                                            pixel_count);
}

int os1_video_present_argb8888_strided(int window, int x, int y, int width,
                                       int height, const uint32_t *pixels,
                                       size_t source_stride,
                                       size_t source_pixel_count) {
  if (window < 0 || width <= 0 || height <= 0 || !pixels)
    return -EINVAL;

  size_t row_width = (size_t)width;
  size_t rows = (size_t)height;
  if (source_stride < row_width)
    return -EINVAL;

  size_t packed_count = row_width * rows;
  if (packed_count / row_width != rows)
    return -EINVAL;
  size_t source_count = (rows - 1) * source_stride + row_width;
  if ((rows - 1) != 0 && (source_count - row_width) / (rows - 1) != source_stride)
    return -EINVAL;
  if (source_pixel_count < source_count)
    return -EINVAL;

  if (source_stride == row_width) {
    _sys_window_blit(window, x, y, width, height, pixels);
    return 0;
  }

  if (packed_count > ((size_t)-1) / sizeof(uint32_t))
    return -EINVAL;
  uint32_t *packed = (uint32_t *)malloc(packed_count * sizeof(uint32_t));
  if (!packed)
    return -ENOMEM;
  for (size_t row = 0; row < rows; row++)
    memcpy(packed + row * row_width, pixels + row * source_stride,
           row_width * sizeof(uint32_t));

  _sys_window_blit(window, x, y, width, height, packed);
  free(packed);
  return 0;
}

int os1_video_poll_event(struct os1_video_event *out) {
  if (!out)
    return -EINVAL;

  input_event_t input;
  int result = input_poll_event(&input);
  if (result <= 0)
    return result;

  if (input.type == INPUT_TYPE_KEYBOARD) {
    out->type = OS1_VIDEO_EVENT_KEY;
    out->data.key.scancode = input.keyboard.scancode;
    out->data.key.key = input.keyboard.key;
    out->data.key.state = (uint8_t)input.keyboard.state;
    for (int i = 0; i < 8; i++)
      out->data.key.utf8[i] = input.keyboard.utf8[i];
    return 1;
  }
  if (input.type == INPUT_TYPE_MOUSE) {
    out->type = OS1_VIDEO_EVENT_MOUSE_BUTTON;
    out->data.mouse.button = input.mouse.button;
    out->data.mouse.state = input.mouse.state;
    out->data.mouse.x = input.mouse.x;
    out->data.mouse.y = input.mouse.y;
    return 1;
  }
  if (input.type == INPUT_TYPE_RESIZE) {
    out->type = OS1_VIDEO_EVENT_RESIZE;
    out->data.resize.width = input.resize.w;
    out->data.resize.height = input.resize.h;
    return 1;
  }
  return 0;
}

void os1_video_render(void) { _sys_compositor_render(); }
