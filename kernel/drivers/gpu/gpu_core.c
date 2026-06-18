#include <drivers/gpu/gpu.h>
#include <kernel/printk.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

static struct gpu_device *gpu_list = NULL;
static struct gpu_device *primary_gpu = NULL;
static DEFINE_SPINLOCK(gpu_list_lock);

int gpu_register(struct gpu_device *dev) {
  if (!dev)
    return -1;

  uint64_t flags;
  spin_lock_irqsave(&gpu_list_lock, &flags);

  dev->next = gpu_list;
  gpu_list = dev;

  if (!primary_gpu) {
    primary_gpu = dev;
    pr_info("GPU: Primary device set to %s\n", dev->name);
  }

  spin_unlock_irqrestore(&gpu_list_lock, flags);
  pr_info("GPU: Registered %s\n", dev->name);
  return 0;
}

void gpu_unregister(struct gpu_device *dev) {
  if (!dev)
    return;

  uint64_t flags;
  spin_lock_irqsave(&gpu_list_lock, &flags);

  struct gpu_device **pp = &gpu_list;
  while (*pp) {
    if (*pp == dev) {
      *pp = dev->next;
      break;
    }
    pp = &(*pp)->next;
  }

  if (primary_gpu == dev) {
    primary_gpu = gpu_list; /* Failover to head of list */
    pr_info("GPU: Primary device removed. Switched to %s\n",
            primary_gpu ? primary_gpu->name : "None");
  }

  spin_unlock_irqrestore(&gpu_list_lock, flags);
}

struct gpu_device *gpu_get_primary(void) { return primary_gpu; }

/* Contract wrappers over the primary GPU (GFX-DYN-01).  Each is a no-op
 * returning -1 if there is no primary device or the driver does not implement
 * the op, so the compositor can call them unconditionally. */
int gpu_set_mode(int width, int height) {
  struct gpu_device *dev = primary_gpu;
  if (!dev || !dev->ops || !dev->ops->set_mode)
    return -1;
  return dev->ops->set_mode(dev, width, height);
}

int gpu_get_display_info(int *width, int *height) {
  struct gpu_device *dev = primary_gpu;
  if (!dev || !dev->ops || !dev->ops->get_display_info)
    return -1;
  return dev->ops->get_display_info(dev, width, height);
}

int gpu_poll_events(int *new_w, int *new_h) {
  struct gpu_device *dev = primary_gpu;
  if (!dev || !dev->ops || !dev->ops->poll_events)
    return -1;
  return dev->ops->poll_events(dev, new_w, new_h);
}
