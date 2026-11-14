/*
 * NXPAX: diagnostic command client.
 * Queries the dedicated /sys/services/nxpaxd broker through a capability
 * port and transferred response pipe. It never opens the privileged resource
 * or instantiates the server.
 */
#include <os1.h>
#include <nxpax.h>
#include <nxpax_service.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static int nxpax_read_full(int handle, struct os1_nxpax_query *query) {
  uint8_t *bytes = (uint8_t *)query;
  size_t done = 0U;
  if (query == NULL)
    return -1;
  for (size_t step = 0U; step < sizeof(*query) && done < sizeof(*query);
       ++step) {
    long result = OS1_object_read(handle, bytes + done,
                                  sizeof(*query) - done);
    if (result <= 0 || (size_t)result > sizeof(*query) - done)
      return -1;
    done += (size_t)result;
  }
  return done == sizeof(*query) ? 0 : -1;
}

static int nxpax_client_read(uint64_t cursor, struct os1_nxpax_query *output) {
  int pipefd[2] = {-1, -1};
  int ok = -1;
  if (output == NULL)
    return -1;
  int port = OS1_port_open(NXPAX_PORT);
  if (port < 0)
    return -1;
  if (pipe(pipefd) != 0) {
    OS1low_handle_close(port);
    return -1;
  }
  struct ipc_message message = {0};
  message.type = NXPAX_IPC_READ;
  message.data1 = cursor;
  message.data2 = OS1_NXPAX_ABI_VERSION;
  if (OS1_port_send_caps(port, &message, &pipefd[1], 1) ==
      (long)sizeof(message)) {
    OS1low_handle_close(pipefd[1]);
    pipefd[1] = -1;
    if (nxpax_read_full(pipefd[0], output) == 0 &&
        output->abi_version == OS1_NXPAX_ABI_VERSION &&
        output->struct_size == sizeof(*output) &&
        output->operation_id == OS1_NXPAX_OP_READ_RECORD &&
        output->work_units <= 1U)
      ok = 0;
  }
  if (pipefd[1] >= 0)
    OS1low_handle_close(pipefd[1]);
  OS1low_handle_close(pipefd[0]);
  OS1low_handle_close(port);
  return ok;
}

static int nxpax_probe(void) {
  struct os1_nxpax_query query = {0};
  uint64_t cursor = 0U;
  for (uint32_t i = 0U; i < NXPAX_MAX_PROBE; ++i) {
    if (nxpax_client_read(cursor, &query) != 0) {
      printf("[NXPAX] service unavailable or protocol failed\n");
      return 1;
    }
    if (query.status == OS1_NXPAX_STATUS_EMPTY) {
      printf("[NXPAX] end newest=%llu dropped=%llu\n",
             (unsigned long long)query.newest,
             (unsigned long long)query.dropped);
      return 0;
    }
    if (query.status != OS1_NXPAX_STATUS_EVENT &&
        query.status != OS1_NXPAX_STATUS_OVERRUN)
      return 1;
    const char *severity =
        query.kind == OS1_NXPAX_KIND_DIAGNOSTIC_WARNING ||
                query.kind == OS1_NXPAX_KIND_USER_TERMINATED
            ? "WARN"
            : query.kind == OS1_NXPAX_KIND_DIAGNOSTIC_ERROR ||
                      query.kind == OS1_NXPAX_KIND_TERMINATION_FAILED
                  ? "ERROR"
                  : query.kind == OS1_NXPAX_KIND_KERNEL_FATAL ? "FATAL" : "FAULT";
    printf("[NXPAX][%s] seq=%llu cpu=%d pid=%d kind=%u flags=%u overrun=%u units=%llu\n",
           severity,
           (unsigned long long)query.cursor, query.cpu_id, query.pid,
           query.kind, query.flags,
           query.status == OS1_NXPAX_STATUS_OVERRUN ? 1U : 0U,
           (unsigned long long)query.work_units);
    if (query.kind == OS1_NXPAX_KIND_DIAGNOSTIC_WARNING ||
        query.kind == OS1_NXPAX_KIND_DIAGNOSTIC_ERROR)
      printf("[NXPAX] diagnostic fingerprint=0x%016llx\n",
             (unsigned long long)query.syndrome);
    if (query.cursor <= cursor)
      return 1;
    cursor = query.cursor;
  }
  return 0;
}

int main(int argc, char **argv) {
  /* Diagnostics cannot escalate a command to the server personality. */
  if (argc == 2 && strcmp(argv[1], "--service") == 0)
    return 1;
  if (argc == 1 || (argc == 2 && strcmp(argv[1], "--probe") == 0))
    return nxpax_probe();
  if (printf("usage: nxpax [--probe]\n") < 0)
    return 1;
  return 1;
}
