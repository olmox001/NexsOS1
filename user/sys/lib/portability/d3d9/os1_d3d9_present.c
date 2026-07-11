/* OS1 D3D9-style presentation chain over the portability adapter. */
#include "os1_d3d9_present.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "../os1_video_platform.h"

/* Backend dispatch table: device/context-facing entry points never branch on
 * the backend themselves, they forward here.  A future ASTRA GPU-context
 * backend adds one more table, not new personality code. */
struct os1_d3d9_present_ops {
  uint32_t backend;
  int (*create)(struct os1_d3d9_swapchain *swapchain,
                const struct os1_d3d9_present_params *params);
  void (*destroy)(struct os1_d3d9_swapchain *swapchain);
  int (*lock)(struct os1_d3d9_swapchain *swapchain,
              struct os1_d3d9_locked_rect *out);
  int (*unlock)(struct os1_d3d9_swapchain *swapchain);
  int (*present)(struct os1_d3d9_swapchain *swapchain,
                 const struct os1_d3d9_dirty_rect *dirty);
  int (*reset)(struct os1_d3d9_swapchain *swapchain, int width, int height);
};

struct os1_d3d9_swapchain {
  const struct os1_d3d9_present_ops *ops;
  int window;
  int width;
  int height;
  int locked;
  uint32_t *backbuffer;
};

/* ── Software backend: CPU backbuffer presented over os1_video_platform ── */

static int sw_create(struct os1_d3d9_swapchain *sc,
                     const struct os1_d3d9_present_params *params) {
  size_t count = (size_t)params->width * (size_t)params->height;
  sc->backbuffer = (uint32_t *)malloc(count * sizeof(uint32_t));
  if (!sc->backbuffer)
    return -ENOMEM;
  memset(sc->backbuffer, 0, count * sizeof(uint32_t));

  sc->window = os1_video_window_create(params->x, params->y, params->width,
                                       params->height, params->title);
  if (sc->window < 0) {
    free(sc->backbuffer);
    sc->backbuffer = NULL;
    return sc->window;
  }
  sc->width = params->width;
  sc->height = params->height;
  return 0;
}

static void sw_destroy(struct os1_d3d9_swapchain *sc) {
  if (sc->window >= 0)
    os1_video_window_destroy(sc->window);
  free(sc->backbuffer);
  sc->backbuffer = NULL;
}

static int sw_lock(struct os1_d3d9_swapchain *sc,
                   struct os1_d3d9_locked_rect *out) {
  out->bits = sc->backbuffer;
  out->pitch = (size_t)sc->width * sizeof(uint32_t);
  return 0;
}

static int sw_unlock(struct os1_d3d9_swapchain *sc) {
  (void)sc;
  return 0;
}

static int sw_present(struct os1_d3d9_swapchain *sc,
                      const struct os1_d3d9_dirty_rect *dirty) {
  int x = 0, y = 0, w = sc->width, h = sc->height;
  if (dirty) {
    x = dirty->x;
    y = dirty->y;
    w = dirty->width;
    h = dirty->height;
    /* Clamp to the backbuffer (D3D9 rejects out-of-range source rects). */
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > sc->width ||
        y + h > sc->height)
      return -EINVAL;
  }
  const uint32_t *first_row = sc->backbuffer + (size_t)y * sc->width + x;
  size_t remaining =
      (size_t)sc->width * sc->height - ((size_t)y * sc->width + x);
  int rc = os1_video_present_argb8888_strided(sc->window, x, y, w, h,
                                              first_row, (size_t)sc->width,
                                              remaining);
  if (rc < 0)
    return rc;
  os1_video_render();
  return 0;
}

static int sw_reset(struct os1_d3d9_swapchain *sc, int width, int height) {
  size_t count = (size_t)width * (size_t)height;
  uint32_t *fresh = (uint32_t *)malloc(count * sizeof(uint32_t));
  if (!fresh)
    return -ENOMEM;
  memset(fresh, 0, count * sizeof(uint32_t));

  int rc = os1_video_window_resize(sc->window, width, height);
  if (rc < 0) {
    free(fresh);
    return rc;
  }
  free(sc->backbuffer);
  sc->backbuffer = fresh;
  sc->width = width;
  sc->height = height;
  return 0;
}

static const struct os1_d3d9_present_ops software_present_ops = {
    .backend = OS1_VIDEO_BACKEND_SOFTWARE,
    .create = sw_create,
    .destroy = sw_destroy,
    .lock = sw_lock,
    .unlock = sw_unlock,
    .present = sw_present,
    .reset = sw_reset,
};

/* ── Backend-independent dispatch ──────────────────────────────────────── */

static const struct os1_d3d9_present_ops *select_present_ops(void) {
  struct os1_video_info info;
  if (os1_video_query(&info) != 0)
    return NULL;
  if (info.backend == OS1_VIDEO_BACKEND_SOFTWARE &&
      (info.capabilities & OS1_VIDEO_CAP_ARGB8888))
    return &software_present_ops;
  /* OS1_VIDEO_BACKEND_GPU_CONTEXT is reserved for the ASTRA GPU object. */
  return NULL;
}

int os1_d3d9_swapchain_create(const struct os1_d3d9_present_params *params,
                              struct os1_d3d9_swapchain **out) {
  if (!params || !out || params->width <= 0 || params->height <= 0 ||
      !params->title)
    return -EINVAL;

  const struct os1_d3d9_present_ops *ops = select_present_ops();
  if (!ops)
    return -ENOSYS;

  struct os1_d3d9_swapchain *sc =
      (struct os1_d3d9_swapchain *)malloc(sizeof(*sc));
  if (!sc)
    return -ENOMEM;
  memset(sc, 0, sizeof(*sc));
  sc->ops = ops;
  sc->window = -1;

  int rc = ops->create(sc, params);
  if (rc < 0) {
    free(sc);
    return rc;
  }
  *out = sc;
  return 0;
}

void os1_d3d9_swapchain_destroy(struct os1_d3d9_swapchain *swapchain) {
  if (!swapchain)
    return;
  swapchain->ops->destroy(swapchain);
  free(swapchain);
}

int os1_d3d9_swapchain_lock(struct os1_d3d9_swapchain *swapchain,
                            struct os1_d3d9_locked_rect *out) {
  if (!swapchain || !out)
    return -EINVAL;
  if (swapchain->locked)
    return -EBUSY;
  int rc = swapchain->ops->lock(swapchain, out);
  if (rc == 0)
    swapchain->locked = 1;
  return rc;
}

int os1_d3d9_swapchain_unlock(struct os1_d3d9_swapchain *swapchain) {
  if (!swapchain)
    return -EINVAL;
  if (!swapchain->locked)
    return -EINVAL;
  int rc = swapchain->ops->unlock(swapchain);
  if (rc == 0)
    swapchain->locked = 0;
  return rc;
}

int os1_d3d9_swapchain_present(struct os1_d3d9_swapchain *swapchain,
                               const struct os1_d3d9_dirty_rect *dirty) {
  if (!swapchain)
    return -EINVAL;
  if (swapchain->locked)
    return -EINVAL; /* D3D9: presenting a locked surface is illegal */
  return swapchain->ops->present(swapchain, dirty);
}

int os1_d3d9_swapchain_reset(struct os1_d3d9_swapchain *swapchain, int width,
                             int height) {
  if (!swapchain || width <= 0 || height <= 0)
    return -EINVAL;
  if (swapchain->locked)
    return -EBUSY;
  return swapchain->ops->reset(swapchain, width, height);
}

int os1_d3d9_swapchain_backend(const struct os1_d3d9_swapchain *swapchain) {
  if (!swapchain)
    return -EINVAL;
  return (int)swapchain->ops->backend;
}
