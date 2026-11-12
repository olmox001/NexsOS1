/*
 * nxpaxd: dedicated privileged-but-user-mode diagnostic broker.
 * The command-line client remains /sys/bin/nxpax; both share the same ABI.
 * Machine addresses stay sanitized on the broker's only outgoing pipe.
 */
#include <os1.h>
#include <nxpax.h>
#include <nxpax_service.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int nxpax_write_full(int handle,
                            const struct os1_nxpax_query *query) {
  const uint8_t *bytes = (const uint8_t *)query;
  size_t done = 0U;
  if (query == NULL)
    return -1;
  for (size_t step = 0U; step < sizeof(*query) && done < sizeof(*query);
       ++step) {
    long result = OS1_object_write(handle, bytes + done,
                                   sizeof(*query) - done);
    if (result <= 0)
      return result < 0 ? (int)result : -EPIPE;
    if ((size_t)result > sizeof(*query) - done)
      return -EIO;
    done += (size_t)result;
  }
  return done == sizeof(*query) ? 0 : -EIO;
}

static int nxpax_claim_port(void) {
  int last_failure = -EEXIST;
  for (uint32_t step = 0U; step < NXPAX_CLAIM_TRIES; ++step) {
    int handle = OS1_port_create(NXPAX_PORT);
    if (handle >= 0)
      return handle;
    last_failure = handle;
    OS1_sleep(100);
  }
  return last_failure;
}

static void nxpax_serve_request(const struct ipc_message *message, int source) {
  struct os1_nxpax_query query = {0};
  int reply = -1;
  uint8_t *reply_bytes = (uint8_t *)&reply;
  for (unsigned i = 0U; i < sizeof(reply); ++i)
    reply_bytes[i] = (uint8_t)message->payload[i];
  if (reply < 0)
    return;
  if (message->type == NXPAX_IPC_READ &&
      message->data2 == OS1_NXPAX_ABI_VERSION &&
      message->data1 != UINT64_MAX) {
    query.abi_version = OS1_NXPAX_ABI_VERSION;
    query.operation_id = OS1_NXPAX_OP_READ_RECORD;
    query.struct_size = sizeof(query);
    query.cursor = message->data1;
    if (OS1_object_read(source, &query, sizeof(query)) == (long)sizeof(query)) {
      /* The service deliberately does not delegate machine addresses to
       * clients with only an OS1nx_pax send capability. */
      query.fault_pc = 0U;
      query.fault_address = 0U;
      if (query.kind != OS1_NXPAX_KIND_DIAGNOSTIC_WARNING &&
          query.kind != OS1_NXPAX_KIND_DIAGNOSTIC_ERROR)
        query.syndrome = 0U;
      int delivered = nxpax_write_full(reply, &query);
      if (delivered != 0)
        OS1_report_error("nxpaxd.reply_delivery", delivered);
    }
  }
  long closed = OS1low_handle_close(reply);
  if (closed < 0)
    OS1_report_error("nxpaxd.reply_close", (int)closed);
}

/* Mirror each newly published HAL/Core event to UART from the existing
 * privileged broker; ring reads are snapshots, not destructive dequeues.
 * The output contains no raw PC/address and never depends on NXShell. */
static int nxpax_uart_drain(int source, uint64_t *cursor) {
  if (cursor == NULL)
    return -EINVAL;
  for (unsigned step = 0U; step < NXPAX_MAX_PROBE; ++step) {
    struct os1_nxpax_query query = {0};
    query.abi_version = OS1_NXPAX_ABI_VERSION;
    query.struct_size = sizeof(query);
    query.operation_id = OS1_NXPAX_OP_READ_RECORD;
    query.cursor = *cursor;
    long read = OS1_object_read(source, &query, sizeof(query));
    if (read == -EAGAIN)
      return 0; /* a reserved sequence is not yet published */
    if (read != (long)sizeof(query))
      return read < 0 ? (int)read : -EIO;
    if (query.abi_version != OS1_NXPAX_ABI_VERSION ||
        query.struct_size != sizeof(query) ||
        query.operation_id != OS1_NXPAX_OP_READ_RECORD)
      return -EINVAL;
    if (query.status == OS1_NXPAX_STATUS_EMPTY)
      return 0;
    if ((query.status != OS1_NXPAX_STATUS_EVENT &&
         query.status != OS1_NXPAX_STATUS_OVERRUN) ||
        query.cursor <= *cursor)
      return -EIO;
    const char *severity =
        query.kind == OS1_NXPAX_KIND_DIAGNOSTIC_WARNING ||
        query.kind == OS1_NXPAX_KIND_USER_TERMINATED ? "WARN" :
        query.kind == OS1_NXPAX_KIND_DIAGNOSTIC_ERROR ||
        query.kind == OS1_NXPAX_KIND_TERMINATION_FAILED ? "ERROR" :
        query.kind == OS1_NXPAX_KIND_KERNEL_FATAL ? "FATAL" : "FAULT";
    int logged = printf("[NXPAX][%s] seq=%llu cpu=%d pid=%d kind=%u flags=%u "
           "overrun=%u dropped=%llu fingerprint=0x%016llx\n",
           severity, (unsigned long long)query.cursor,
           query.cpu_id, query.pid, query.kind, query.flags,
           query.status == OS1_NXPAX_STATUS_OVERRUN ? 1U : 0U,
           (unsigned long long)query.dropped,
           (unsigned long long)(
               query.kind == OS1_NXPAX_KIND_DIAGNOSTIC_WARNING ||
               query.kind == OS1_NXPAX_KIND_DIAGNOSTIC_ERROR
                   ? query.syndrome : 0U));
    if (logged < 0)
      return -EIO;
    *cursor = query.cursor;
  }
  return 0; /* bounded per-service-turn work; continue next turn */
}

static int nxpax_service(void) {
  int source = (int)OS1low_handle_create(OS1_NS_RESOURCE, OS1_NXPAX_RESOURCE,
                                        OS1_RIGHT_READ, OBJ_TYPE_RESOURCE);
  if (source < 0) {
    OS1_report_error("nxpaxd.resource_open", source);
    return 1;
  }
  int port = nxpax_claim_port();
  if (port < 0) {
    OS1_report_error("nxpaxd.port_claim", port);
    long closed = OS1low_handle_close(source);
    if (closed < 0)
      OS1_report_error("nxpaxd.resource_close", (int)closed);
    return 1;
  }
  /* The same service receives client IPC and mirrors newly recorded
   * diagnostics. Nonblocking PORT reads prevent a quiet client queue from
   * indefinitely suppressing UART streaming. Both loops are bounded. */
  uint64_t cursor = 0U;
  int status = 0;
  for (uint64_t epoch = 0U; epoch < UINT64_MAX && status == 0; ++epoch) {
    int received = 0;
    for (unsigned request = 0U; request < 32U && received == 0 &&
                               status == 0; ++request) {
      struct os1_port_packet packet;
      received = OS1_port_packet_receive(port, &packet, 1);
      if (received == -EAGAIN) continue;
      if (received != 0) { status = received; continue; }
      if (packet.cap_count == 1U && packet.message.type == NXPAX_IPC_READ)
        nxpax_serve_request(&packet.message, source);
      else status = OS1_port_packet_close_caps(&packet);
    }
    if (status != 0) {
      OS1_report_error("nxpaxd.port_receive", status);
    }
    if (status == 0) status = nxpax_uart_drain(source, &cursor);
    if (status != 0) {
      OS1_report_error("nxpaxd.uart_drain", status);
    }
    if (status == 0) OS1_sleep(20);
  }
  long closed_port = OS1low_handle_close(port);
  long closed_source = OS1low_handle_close(source);
  if (closed_port < 0)
    OS1_report_error("nxpaxd.port_close", (int)closed_port);
  if (closed_source < 0)
    OS1_report_error("nxpaxd.resource_close", (int)closed_source);
  return 1;
}

int main(void) {
  return nxpax_service();
}
