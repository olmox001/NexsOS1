/*
 * nxtimerdd - supervised NexsOS time/telemetry service.
 *
 * Owns the privileged TIME object capabilities and exposes sanitized snapshots
 * through OS1nx_time.  The process serves a bounded request epoch and exits;
 * nxinit owns restart policy and reclaims the named receive endpoint.
 */
#include <os1_time_service.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TIMERD_CLAIM_TRIES UINT32_C(20)
#define TIMERD_CLAIM_WAIT_MS 100
#define TIMERD_MAX_REPLY                                                       \
  ((unsigned long)sizeof(struct os1_time_service_timer_stats))

static int timerd_write_full(int fd, const void *buffer, unsigned long size) {
  const unsigned char *bytes = (const unsigned char *)buffer;
  unsigned long done = 0U;
  if (size > TIMERD_MAX_REPLY)
    return -1;
  for (unsigned long step = 0U; step < TIMERD_MAX_REPLY && done < size;
       ++step) {
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

static void timerd_copy_cpu(struct os1_time_service_timer_cpu *dest,
                            const struct os1_timer_cpu_stats_v4 *src) {
  dest->cpu_id = src->cpu_id;
  dest->arm_count = src->arm_count;
  dest->cancel_count = src->cancel_count;
  dest->fire_count = src->fire_count;
  dest->scan_count = src->scan_count;
  dest->scan_pass_count = src->scan_pass_count;
  dest->queue_depth = src->queue_depth;
  dest->queue_high_water = src->queue_high_water;
  dest->max_scan_per_pass = src->max_scan_per_pass;
  dest->max_fire_per_pass = src->max_fire_per_pass;
  dest->late_fire_count = src->late_fire_count;
  dest->fire_lateness_ns_total = src->fire_lateness_ns_total;
  dest->max_fire_lateness_ns = src->max_fire_lateness_ns;
  dest->clockevent_late_count = src->clockevent_late_count;
  dest->clockevent_lateness_ns_total = src->clockevent_lateness_ns_total;
  dest->max_clockevent_lateness_ns = src->max_clockevent_lateness_ns;
  dest->absolute_program_count = src->absolute_program_count;
  dest->irq_mask_interval_count = src->irq_mask_interval_count;
  dest->irq_mask_total_ns = src->irq_mask_total_ns;
  dest->irq_mask_max_ns = src->irq_mask_max_ns;
  dest->irq_mask_invalid_sample_count = src->irq_mask_invalid_sample_count;
  dest->irq_mask_active_ns = src->irq_mask_active_ns;
  dest->irq_mask_flags = src->irq_mask_flags;
}

static int timerd_reply_stats(int reply, int stats_handle) {
  static struct os1_timer_stats_snapshot_v4 wire;
  static struct os1_time_service_timer_stats published;
  long status = OS1low_time_stats_snapshot(stats_handle, &wire, sizeof(wire));
  if (status != (long)sizeof(wire) ||
      wire.abi_version != OS1_TIMER_STATS_ABI_VERSION ||
      wire.struct_size != sizeof(wire) ||
      wire.cpu_record_size != sizeof(struct os1_timer_cpu_stats_v4) ||
      wire.cpu_count == 0U || wire.cpu_count > OS1_TIME_MAX_CPUS)
    return -1;
  published = (struct os1_time_service_timer_stats){0};
  published.cpu_count = wire.cpu_count;
  published.sequence = wire.sequence;
  published.monotonic_ns = wire.monotonic_ns;
  for (uint32_t index = 0U; index < OS1_TIME_MAX_CPUS; ++index) {
    if (index >= wire.cpu_count)
      break;
    timerd_copy_cpu(&published.cpu[index], &wire.cpu[index]);
  }
  return timerd_write_full(reply, &published, sizeof(published));
}

static int timerd_handle_request(const struct ipc_message *message,
                                 int time_handle, int stats_handle) {
  int reply = -1;
  /* The transferred handle is a native 32-bit capability descriptor.
   * Decode exactly four bytes; no unaligned aliasing or unchecked memcpy. */
  unsigned char *target = (unsigned char *)&reply;
  for (unsigned i = 0U; i < sizeof(reply); ++i)
    target[i] = (unsigned char)message->payload[i];
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
  for (uint32_t request = 0U; request < OS1_TIME_SERVICE_EPOCH_REQUEST_LIMIT;
       ++request) {
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
  if (time_handle < 0 || stats_handle < 0) {
    if (time_handle < 0)
      OS1_report_error("nxtimerdd.time_open", time_handle);
    if (stats_handle < 0)
      OS1_report_error("nxtimerdd.stats_open", stats_handle);
    if (time_handle >= 0)
      (void)OS1low_handle_close(time_handle);
    if (stats_handle >= 0)
      (void)OS1low_handle_close(stats_handle);
    return 1;
  }
  int port = timerd_claim();
  if (port < 0) {
    OS1_report_error("nxtimerdd.port_claim", port);
    (void)OS1low_handle_close(stats_handle);
    (void)OS1low_handle_close(time_handle);
    return 1;
  }
  /* Readiness is the successfully claimed authoritative service PORT. */
  int status = timerd_serve_epoch(port, time_handle, stats_handle);
  int close_port = OS1low_handle_close(port);
  int close_stats = OS1low_handle_close(stats_handle);
  int close_time = OS1low_handle_close(time_handle);
  if (status != 0)
    OS1_report_error("nxtimerdd.serve_epoch", status);
  if (close_port != 0)
    OS1_report_error("nxtimerdd.port_close", close_port);
  if (close_stats != 0)
    OS1_report_error("nxtimerdd.stats_close", close_stats);
  if (close_time != 0)
    OS1_report_error("nxtimerdd.time_close", close_time);
  if (status != 0 || close_port != 0 || close_stats != 0 || close_time != 0)
    return 1;
  return 0;
}
