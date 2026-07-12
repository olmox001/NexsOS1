/* OS1 software implementation of the OpenGL presentation seam. */
#include "os1_gl_platform.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "../os1_video_platform.h"

struct os1_gl_surface {
  uint32_t backend;
  int window;
  int width;
  int height;
  uint32_t *color_buffer;
};

int os1_gl_surface_create(const struct os1_gl_surface_params *params,
                          struct os1_gl_surface **out) {
  if (!params || !out || params->width <= 0 || params->height <= 0 ||
      !params->title)
    return -EINVAL;

  struct os1_video_info info;
  if (os1_video_query(&info) != 0)
    return -ENOSYS;
  if (info.backend != OS1_VIDEO_BACKEND_SOFTWARE ||
      !(info.capabilities & OS1_VIDEO_CAP_ARGB8888))
    return -ENOSYS; /* GPU-context backend reserved for the ASTRA object */

  struct os1_gl_surface *surface =
      (struct os1_gl_surface *)malloc(sizeof(*surface));
  if (!surface)
    return -ENOMEM;
  memset(surface, 0, sizeof(*surface));
  surface->backend = info.backend;
  surface->window = -1;

  size_t count = (size_t)params->width * (size_t)params->height;
  surface->color_buffer = (uint32_t *)malloc(count * sizeof(uint32_t));
  if (!surface->color_buffer) {
    free(surface);
    return -ENOMEM;
  }
  memset(surface->color_buffer, 0, count * sizeof(uint32_t));

  surface->window = os1_video_window_create(params->x, params->y,
                                            params->width, params->height,
                                            params->title);
  if (surface->window < 0) {
    int rc = surface->window;
    free(surface->color_buffer);
    free(surface);
    return rc;
  }
  surface->width = params->width;
  surface->height = params->height;
  *out = surface;
  return 0;
}

void os1_gl_surface_destroy(struct os1_gl_surface *surface) {
  if (!surface)
    return;
  if (surface->window >= 0)
    os1_video_window_destroy(surface->window);
  free(surface->color_buffer);
  free(surface);
}

uint32_t *os1_gl_surface_buffer(struct os1_gl_surface *surface, int *width,
                                int *height, size_t *pitch_bytes) {
  if (!surface)
    return NULL;
  if (width)
    *width = surface->width;
  if (height)
    *height = surface->height;
  if (pitch_bytes)
    *pitch_bytes = (size_t)surface->width * sizeof(uint32_t);
  return surface->color_buffer;
}

int os1_gl_surface_swap(struct os1_gl_surface *surface) {
  if (!surface)
    return -EINVAL;
  int rc = os1_video_present_argb8888(
      surface->window, 0, 0, surface->width, surface->height,
      surface->color_buffer, (size_t)surface->width * surface->height);
  if (rc < 0)
    return rc;
  os1_video_render();
  return 0;
}

int os1_gl_surface_resize(struct os1_gl_surface *surface, int width,
                          int height) {
  if (!surface || width <= 0 || height <= 0)
    return -EINVAL;

  size_t count = (size_t)width * (size_t)height;
  uint32_t *fresh = (uint32_t *)malloc(count * sizeof(uint32_t));
  if (!fresh)
    return -ENOMEM;
  memset(fresh, 0, count * sizeof(uint32_t));

  int rc = os1_video_window_resize(surface->window, width, height);
  if (rc < 0) {
    free(fresh);
    return rc;
  }
  free(surface->color_buffer);
  surface->color_buffer = fresh;
  surface->width = width;
  surface->height = height;
  return 0;
}

int os1_gl_surface_backend(const struct os1_gl_surface *surface) {
  if (!surface)
    return -EINVAL;
  return (int)surface->backend;
}
