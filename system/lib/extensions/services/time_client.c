#include <os1_time_service.h>
#include <string.h>
#include <unistd.h>

#define TIME_REPLY_MAX_BYTES ((unsigned long)sizeof(struct os1_time_service_timer_stats))

static int time_read_full(int fd, void *buffer, unsigned long size) {
  unsigned char *bytes = (unsigned char *)buffer;
  unsigned long done = 0U;

  if (size > TIME_REPLY_MAX_BYTES)
    return -1;
  for (unsigned long step = 0U;
       step < TIME_REPLY_MAX_BYTES && done < size; ++step) {
    long got = read(fd, (char *)bytes + done, size - done);
    if (got <= 0)
      return -1;
    done += (unsigned long)got;
  }
  return done == size ? 0 : -1;
}

static int time_service_request(uint64_t request, void *output,
                                unsigned long size) {
  struct ipc_message message;
  int reply[2] = {-1, -1};
  int service;

  if (output == NULL || size > TIME_REPLY_MAX_BYTES)
    return -1;
  service = OS1_port_open(OS1NX_PORT_TIME);
  if (service < 0)
    return -1;
  if (pipe(reply) != 0) {
    OS1low_handle_close(service);
    return -1;
  }
  memset(&message, 0, sizeof(message));
  message.type = OS1_TIME_SERVICE_IPC_TYPE;
  message.data1 = request;
  message.data2 = OS1_TIME_SERVICE_VERSION;
  if (OS1_port_send_caps(service, &message, &reply[1], 1) !=
      (long)sizeof(message)) {
    close(reply[0]);
    close(reply[1]);
    OS1low_handle_close(service);
    return -1;
  }
  close(reply[1]);
  OS1low_handle_close(service);
  if (time_read_full(reply[0], output, size) != 0) {
    close(reply[0]);
    return -1;
  }
  close(reply[0]);
  return 0;
}

int OS1_time_service_snapshot(struct os1_time_snapshot_v1 *snapshot) {
  if (snapshot == NULL)
    return -1;
  if (time_service_request(OS1_TIME_SERVICE_REQUEST_SNAPSHOT, snapshot,
                           sizeof(*snapshot)) != 0)
    return -1;
  if (snapshot->abi_version != OS1_TIME_ABI_VERSION ||
      snapshot->struct_size != sizeof(*snapshot))
    return -1;
  return 0;
}

int OS1_time_service_timer_stats(
    struct os1_time_service_timer_stats *snapshot) {
  if (snapshot == NULL)
    return -1;
  if (time_service_request(OS1_TIME_SERVICE_REQUEST_TIMER_STATS, snapshot,
                           sizeof(*snapshot)) != 0)
    return -1;
  if (snapshot->cpu_count == 0U ||
      snapshot->cpu_count > OS1_TIME_MAX_CPUS)
    return -1;
  return 0;
}
