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
    if (result <= 0 || (size_t)result > sizeof(*query) - done)
      return -1;
    done += (size_t)result;
  }
  return done == sizeof(*query) ? 0 : -1;
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
  /* PORT ownership, not console output, constitutes service readiness. */
  /* Resident broker: each individual receive/request path is bounded.
   * Blocking on the capability port does not busy-spin a CPU. */
  for (uint64_t epoch = 0U; epoch < UINT64_MAX; ++epoch) {
    struct ipc_message message = {0};
    long received = OS1_port_recv(port, &message);
    if (received == (long)sizeof(message))
      nxpax_serve_request(&message, source);
    else {
      OS1_report_error("nxpaxd.port_receive",
                       (int)(received < 0 ? received : -EIO));
      break;
    }
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
