#ifndef _DRIVERS_GPU_H
#define _DRIVERS_GPU_H

#include <stddef.h>
#include <stdint.h>

struct gpu_device;

struct gpu_ops {
  int (*init)(struct gpu_device *dev);
  /* Reconfigure the scanout to width x height at runtime.  Recreates the host
   * resource + backing and re-points the scanout; updates dev->width/height
   * and the framebuffer pointer.  0 on success.  (GFX-DYN-01 #49) */
  int (*set_mode)(struct gpu_device *dev, int width, int height);
  void *(*get_framebuffer)(struct gpu_device *dev, size_t *size);
  int (*flush)(struct gpu_device *dev, int x, int y, int w, int h);
  void (*destroy)(struct gpu_device *dev);
  /* Query the host's current preferred display size (what the device
   * advertises via GET_DISPLAY_INFO).  0 on success.  (GFX-DYN-01 #54) */
  int (*get_display_info)(struct gpu_device *dev, int *width, int *height);
  /* Poll for a host-initiated display-change event.  Returns 1 and fills
   * new_w/new_h when the host resized the output, 0 if nothing changed,
   * <0 on error/unsupported transport. */
  int (*poll_events)(struct gpu_device *dev, int *new_w, int *new_h);
};

struct gpu_device {
  char name[32];
  int width;
  int height;
  int bpp;
  /* The scanout buffer is reached ONLY through ops->get_framebuffer (S-ALIGN
   * F7) — the old raw framebuffer_virt field was a second, field-level path
   * to the same memory and is retired. */
  size_t framebuffer_size;
  struct gpu_ops *ops;
  void *priv;              /* Driver private data */
  struct gpu_device *next; /* Linked list for multi-gpu */
};

/* Core GPU Management */
int gpu_register(struct gpu_device *dev);
void gpu_unregister(struct gpu_device *dev);
struct gpu_device *gpu_get_primary(void);

/* Contract wrappers over the primary GPU (GFX-DYN-01).  The kernel core
 * (compositor) calls these instead of reaching into a driver: no driver
 * internals, no magic numbers in the core. */
int gpu_set_mode(int width, int height);
int gpu_get_display_info(int *width, int *height);
int gpu_poll_events(int *new_w, int *new_h);

#endif
