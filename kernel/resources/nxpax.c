#include <kernel/resource.h>
#include <kernel/object.h>
#include <kernel/hal_uaccess.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <hal/nxpax.h>
#include <nxpax.h>

_Static_assert(OS1_NXPAX_CAPTURE_CPU_LIMIT == NXHAL_CPU_LIMIT,
               "NXPAX resource budget must match HAL CPU limit");
_Static_assert(OS1_NXPAX_MAX_WINDOW_RECORDS == NXHAL_NXPAX_RING_CAPACITY,
               "NXPAX ABI window must match HAL ring");
_Static_assert(OS1_NXPAX_KIND_HARDWARE == NXHAL_NXPAX_KIND_HARDWARE,
               "hardware event kind contract mismatch");
_Static_assert(OS1_NXPAX_KIND_USER_TERMINATED == NXHAL_NXPAX_KIND_USER_TERMINATED,
               "user fault disposition kind contract mismatch");
_Static_assert(OS1_NXPAX_KIND_TERMINATION_FAILED == NXHAL_NXPAX_KIND_TERMINATION_FAILED,
               "failed disposition kind contract mismatch");
_Static_assert(OS1_NXPAX_KIND_KERNEL_FATAL == NXHAL_NXPAX_KIND_KERNEL_FATAL,
               "fatal disposition kind contract mismatch");

static long nxpax_resource_read(void *state, void *user, size_t size) {
  struct os1_nxpax_query request;
  struct os1_nxpax_query reply = {0};
  struct nxhal_nxpax_event event;
  uint64_t target;
  int status;
  (void)state;
  if (size != sizeof(request) || user == NULL)
    return -EINVAL;
  if (hal_copy_from_user(&request, user, sizeof(request)) != 0)
    return -EFAULT;
  if (request.abi_version != OS1_NXPAX_ABI_VERSION ||
      request.struct_size != sizeof(request) ||
      request.operation_id != OS1_NXPAX_OP_READ_RECORD ||
      request.cursor == UINT64_MAX)
    return -EINVAL;
  reply.abi_version = OS1_NXPAX_ABI_VERSION;
  reply.operation_id = OS1_NXPAX_OP_READ_RECORD;
  reply.struct_size = sizeof(reply);
  reply.cursor = request.cursor;
  reply.newest = nxhal_nxpax_latest();
  reply.dropped = nxhal_nxpax_dropped();
  reply.status = OS1_NXPAX_STATUS_EMPTY;
  target = request.cursor + 1U;
  if (reply.newest >= target) {
    if (reply.newest - target >= NXHAL_NXPAX_RING_CAPACITY) {
      target = reply.newest - NXHAL_NXPAX_RING_CAPACITY + 1U;
      reply.status = OS1_NXPAX_STATUS_OVERRUN;
    }
    reply.work_units = 1U;
    status = nxhal_nxpax_read(target, &event);
    if (status < 0)
      return -EAGAIN;
    if (status == 1) {
      reply.cursor = event.sequence;
      reply.pid = event.pid;
      reply.cpu_id = event.cpu_id;
      reply.kind = event.kind;
      reply.flags = event.flags;
      reply.fault_pc = event.fault_pc;
      reply.fault_address = event.fault_address;
      reply.syndrome = event.syndrome;
      if (reply.status != OS1_NXPAX_STATUS_OVERRUN)
        reply.status = OS1_NXPAX_STATUS_EVENT;
    }
  }
  if (hal_copy_to_user(user, &reply, sizeof(reply)) != 0)
    return -EFAULT;
  return (long)sizeof(reply);
}

static const struct nx_resource_ops nxpax_ops = {
    .read = nxpax_resource_read};

int nx_nxpax_resource_acquire(struct process *caller, uint32_t rights,
                               struct kobject *object) {
  /* Never grant TRANSFER/DUPLICATE: only the privileged broker may read
   * machine addresses. Clients obtain sanitized events through its port. */
  const uint32_t allowed = OS1_RIGHT_READ;
  if (caller == NULL || object == NULL || !proc_is_privileged(caller))
    return -EPERM;
  if ((rights & OS1_RIGHT_READ) == 0U || (rights & ~allowed) != 0U)
    return -EPERM;
  object->resource_ops = &nxpax_ops;
  return 0;
}
