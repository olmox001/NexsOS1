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
