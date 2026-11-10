/*
 * nxtimerd - supervised NexsOS time/telemetry service.
 *
 * Owns the privileged TIME object capabilities and exposes sanitized snapshots
 * through OS1nx_time.  The process serves a bounded request epoch and exits;
 * nxinit owns restart policy and reclaims the named receive endpoint.
 */
#include <os1_time_service.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define TIMERD_CLAIM_TRIES UINT32_C(20)
#define TIMERD_CLAIM_WAIT_MS 100
#define TIMERD_MAX_REPLY \
  ((unsigned long)sizeof(struct os1_timer_stats_snapshot_v3))

static int timerd_write_full(int fd, const void *buffer, unsigned long size) {
  const unsigned char *bytes = (const unsigned char *)buffer;
  unsigned long done = 0U;
  if (size > TIMERD_MAX_REPLY)
    return -1;
  for (unsigned long step = 0U;
       step < TIMERD_MAX_REPLY && done < size; ++step) {
    long written = write(fd, (const char *)bytes + done, size - done);
    if (written <= 0)
      return -1;
    done += (unsigned long)written;
  }
  return done == size ? 0 : -1;
}

static int timerd_reply_snapshot(int reply, int time_handle) {
  static struct os1_time_snapshot_v1 snapshot;
  long status = OS1low_time_snapshot(time_handle, &snapshot);
  if (status != (long)sizeof(snapshot))
    return -1;
  return timerd_write_full(reply, &snapshot, sizeof(snapshot));
}

static int timerd_reply_stats(int reply, int stats_handle) {
  static struct os1_timer_stats_snapshot_v3 snapshot;
  long status = OS1low_time_stats_snapshot(stats_handle, &snapshot);
  if (status != (long)sizeof(snapshot))
    return -1;
  return timerd_write_full(reply, &snapshot, sizeof(snapshot));
}

static int timerd_handle_request(const struct ipc_message *message,
                                 int time_handle, int stats_handle) {
  int reply = -1;
  memcpy(&reply, message->payload, sizeof(reply));
  if (reply < 0)
    return -1;
  int status = -1;
  if (message->data2 == OS1_TIME_SERVICE_VERSION) {
    if (message->data1 == OS1_TIME_SERVICE_REQUEST_SNAPSHOT)
      status = timerd_reply_snapshot(reply, time_handle);
    else if (message->data1 == OS1_TIME_SERVICE_REQUEST_TIMER_STATS)
      status = timerd_reply_stats(reply, stats_handle);
  }
  if (close(reply) != 0)
    return -1;
  return status;
}

static int timerd_claim(void) {
  for (uint32_t attempt = 0U; attempt < TIMERD_CLAIM_TRIES; ++attempt) {
    int port = OS1_port_create(OS1NX_PORT_TIME);
    if (port >= 0)
      return port;
    OS1_sleep(TIMERD_CLAIM_WAIT_MS);
  }
  return -1;
}

static int timerd_serve_epoch(int port, int time_handle, int stats_handle) {
  for (uint32_t request = 0U;
       request < OS1_TIME_SERVICE_EPOCH_REQUEST_LIMIT; ++request) {
    struct ipc_message message;
    long received = OS1_port_recv(port, &message);
    if (received != (long)sizeof(message))
      return -1;
    if (message.type == OS1_TIME_SERVICE_IPC_TYPE &&
        timerd_handle_request(&message, time_handle, stats_handle) != 0)
      return -1;
  }
  return 0;
}

int main(void) {
  int time_handle = (int)OS1low_time_open();
  int stats_handle = (int)OS1low_time_stats_open();
  if (time_handle < 0 || stats_handle < 0)
    return 1;
  int port = timerd_claim();
  if (port < 0)
    return 1;
  printf("[nxtimerd] service listening on %s\n", OS1NX_PORT_TIME);
  int status = timerd_serve_epoch(port, time_handle, stats_handle);
  int close_port = OS1low_handle_close(port);
  int close_stats = OS1low_handle_close(stats_handle);
  int close_time = OS1low_handle_close(time_handle);
  if (status != 0 || close_port != 0 || close_stats != 0 || close_time != 0)
    return 1;
  return 0;
}
