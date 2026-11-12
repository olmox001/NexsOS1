#ifndef NX_KERNEL_RESOURCE_H
#define NX_KERNEL_RESOURCE_H
#include <kernel/types.h>

struct process;
struct kobject;
/* Object core only transports authority and lifetime. Providers own their
 * resource protocol and copy/validate user memory outside object_lock. */
struct nx_resource_ops {
  long (*read)(void *state, void *user, size_t size);
  long (*write)(void *state, const void *user, size_t size);
  long (*wait)(void *state, long argument);
  void (*release)(void *state);
};
int nx_resource_acquire(struct process *caller, const char *path,
                        uint32_t rights, struct kobject *object);
int nx_nxpax_resource_acquire(struct process *caller, uint32_t rights,
                              struct kobject *object);
void nx_surface_changed(void);
int nx_input_resource_acquire(const char *path, uint32_t rights,
                              struct kobject *object);
int nx_window_resource_acquire(const char *path, uint32_t rights,
                               struct kobject *object);
void nx_buffer_init(void);
int nx_buffer_resource_acquire(uint32_t rights, struct kobject *object);
void nx_buffer_process_detach(struct process *process);
#endif
