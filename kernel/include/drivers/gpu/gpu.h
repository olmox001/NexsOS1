#ifndef _DRIVERS_GPU_H
#define _DRIVERS_GPU_H

#include <stddef.h>
#include <stdint.h>

struct gpu_device;

struct gpu_ops {
  int (*init)(struct gpu_device *dev);
  int (*set_mode)(struct gpu_device *dev, int width, int height);
  void *(*get_framebuffer)(struct gpu_device *dev, size_t *size);
  int (*flush)(struct gpu_device *dev, int x, int y, int w, int h);
  void (*destroy)(struct gpu_device *dev);
};

struct gpu_device {
  char name[32];
  int width;
  int height;
  int bpp;
  void *framebuffer_virt; /* Kernel Virtual Address of FB */
  size_t framebuffer_size;
  struct gpu_ops *ops;
  void *priv;              /* Driver private data */
  struct gpu_device *next; /* Linked list for multi-gpu */
};

/* Core GPU Management */
int gpu_register(struct gpu_device *dev);
void gpu_unregister(struct gpu_device *dev);
struct gpu_device *gpu_get_primary(void);

#endif
