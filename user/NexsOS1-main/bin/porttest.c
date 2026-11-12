/* Real PORT transport metadata and malformed-service-request regressions. */
#include <os1.h>
#include <os1_buffer.h>
#include <os1_time_service.h>
#include <execsvc.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>

static int packet_roundtrip(int port, int sender, int buffer) {
  struct ipc_message message = {0};
  message.type = 7;
  /* Payload slot zero looks authoritative, but no capability was sent. */
  if (OS1_port_send(sender, &message) != (long)sizeof(message)) return 1;
  struct os1_port_packet packet;
  if (OS1_port_packet_receive(port, &packet, 0) != 0 ||
      packet.cap_count != 0U || packet.message.from != get_pid()) return 2;
  long delegated = OS1low_handle_duplicate(buffer,
      OS1_RIGHT_READ | OS1_RIGHT_TRANSFER);
  if (delegated < 0) return 3;
  int give = (int)delegated;
  long sent = OS1_port_send_caps(sender, &message, &give, 1);
  long closed = OS1low_handle_close(give);
  if (sent != (long)sizeof(message) || closed != 0) return 4;
  if (OS1_port_packet_receive(port, &packet, 0) != 0 ||
      packet.cap_count != 1U || packet.message.from != get_pid()) return 5;
  int received = -1;
  unsigned char *bytes = (unsigned char *)&received;
  for (unsigned i = 0U; i < sizeof(received); ++i)
    bytes[i] = (unsigned char)packet.message.payload[i];
  long rights = OS1low_cap_query(received);
  int released = OS1_port_packet_close_caps(&packet);
  if (rights < 0 || OS1_CAPQ_TYPE(rights) != OBJ_TYPE_RESOURCE ||
      OS1_CAPQ_RIGHTS(rights) != (OS1_RIGHT_READ | OS1_RIGHT_TRANSFER) ||
      released != 0 || packet.cap_count != 0U) return 6;
  return OS1_port_packet_receive(port, &packet, 1) == -EAGAIN ? 0 : 7;
}

static int packet_test(void) {
  char name[32];
  int n = snprintf(name, sizeof(name), "OS1nx_pkt_%d", get_pid());
  if (n <= 0 || (size_t)n >= sizeof(name)) return 1;
  int port = OS1_port_create(name);
  int sender = OS1_port_open(name);
  long buffer = OS1_buffer_create(4096U);
  int result = port < 0 || sender < 0 || buffer < 0 ? 1 :
               packet_roundtrip(port, sender, (int)buffer);
  if (buffer >= 0 && OS1low_handle_close((int)buffer) != 0) result = 8;
  if (sender >= 0 && OS1low_handle_close(sender) != 0) result = 9;
  if (port >= 0 && OS1low_handle_close(port) != 0) result = 10;
  return result;
}

static int forged_service_request(const char *name, int type, uint64_t version) {
  int sender = OS1_port_open(name);
  if (sender < 0) return sender;
  struct ipc_message message = {.type = type, .data1 = 1U, .data2 = version};
  long sent = OS1_port_send(sender, &message);
  long closed = OS1low_handle_close(sender);
  return sent == (long)sizeof(message) && closed == 0 ? 0 : -EIO;
}

static int owned_spawn_denials(void) {
  char key[16];
  int n = snprintf(key, sizeof(key), "%d", get_pid());
  if (n <= 0 || (size_t)n >= sizeof(key)) return 1;
  long control = OS1low_handle_create(OS1_NS_PROC, key, OS1_RIGHT_DESTROY,
                                      OBJ_TYPE_PROCESS);
  long read_only = OS1low_handle_create(OS1_NS_PROC, key, OS1_RIGHT_READ,
                                        OBJ_TYPE_PROCESS);
  int result = control < 0 || read_only < 0 ? 2 : 0;
  if (result == 0 && (OS1_object_ctl((int)control, OBJ_CTL_SPAWN_OWNED, 0) != -EPERM ||
      OS1_object_ctl((int)read_only, OBJ_CTL_SPAWN_OWNED, 0) != -EPERM)) result = 3;
  if (control >= 0 && OS1low_handle_close((int)control) != 0) result = 4;
  if (read_only >= 0 && OS1low_handle_close((int)read_only) != 0) result = 5;
  return result;
}

static int services_survive_forged_slots(void) {
  if (owned_spawn_denials() != 0) return 6;
  if (forged_service_request(OS1NX_PORT_TIME, OS1_TIME_SERVICE_IPC_TYPE,
                              OS1_TIME_SERVICE_VERSION) != 0 ||
      forged_service_request(OS1NX_PORT_EXEC, EXECSVC_REQ_SPAWN,
                              EXECSVC_VERSION) != 0) return 1;
  struct os1_time_snapshot_v1 snapshot;
  if (OS1_time_service_snapshot(&snapshot) != 0) return 2;
  char *args[1] = {(char *)"/bin/true"};
  int pid = execsvc_spawn(1, args, 0U);
  if (pid <= 0 && printf("[porttest] exec-result=%d\n", pid) < 0) return 4;
  if (pid <= 0) return 3;
  int status = 0, joined = 0;
  for (unsigned step = 0U; step < 400U && joined == 0; ++step) {
    joined = waitpid(pid, &status, WNOHANG);
    if (joined == 0) OS1_sleep(10);
  }
  return joined == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 5;
}

struct porttest_args { char *values[16]; };

static int wake_worker(const char *parent_name) {
  char name[32] = {0};
  int n = snprintf(name, sizeof(name), "OS1nx_wqr_%d", get_pid());
  if (n <= 0 || (size_t)n >= sizeof(name)) return 1;
  int reply = OS1_port_create(name), parent = OS1_port_open(parent_name);
  int result = reply < 0 || parent < 0 ? 1 : 0;
  for (unsigned i = 0U; i < 1024U && result == 0; ++i) {
    struct ipc_message message = {.type = 9, .data1 = i};
    if (memcpy(message.payload, name, (size_t)n + 1U) != message.payload)
      result = 2;
    if (result == 0 && OS1_port_send(parent, &message) != (long)sizeof(message))
      result = 3;
    if (result == 0) {
      struct os1_port_packet packet;
      if (OS1_port_packet_receive(reply, &packet, 0) != 0 ||
          packet.cap_count != 0U || packet.message.data1 != i) result = 4;
    }
  }
  if (parent >= 0 && OS1low_handle_close(parent) != 0) result = 5;
  if (reply >= 0 && OS1low_handle_close(reply) != 0) result = 6;
  return result;
}

struct wake_stats {
  uint64_t max_round_ns;
  unsigned max_round_index, rounds;
};

static int wake_parent(int port, int child, struct wake_stats *stats) {
  int reply = -1, result = 0;
  for (unsigned i = 0U; i < 1024U && result == 0; ++i) {
    uint64_t started = os1_mono_ns();
    struct os1_port_packet packet;
    if (OS1_port_packet_receive(port, &packet, 0) != 0 ||
        packet.cap_count != 0U || packet.message.from != child ||
        packet.message.data1 != i) result = 1;
    if (result == 0 && reply < 0) {
      packet.message.payload[sizeof(packet.message.payload) - 1U] = '\0';
      reply = OS1_port_open(packet.message.payload);
      if (reply < 0) result = 2;
    }
    struct ipc_message ack = {.type = 10, .data1 = i};
    if (result == 0 && OS1_port_send(reply, &ack) != (long)sizeof(ack)) result = 3;
    uint64_t finished = os1_mono_ns();
    if (finished < started) result = 5;
    uint64_t elapsed = finished >= started ? finished - started : 0U;
    if (elapsed > stats->max_round_ns) {
      stats->max_round_ns = elapsed;
      stats->max_round_index = i;
    }
    ++stats->rounds;
  }
  if (reply >= 0 && OS1low_handle_close(reply) != 0) result = 4;
  return result;
}

static int wake_race_test(struct wake_stats *stats) {
  char name[32];
  int n = snprintf(name, sizeof(name), "OS1nx_wq_%d", get_pid());
  if (n <= 0 || (size_t)n >= sizeof(name)) return 1;
  int port = OS1_port_create(name);
  if (port < 0) return 2;
  char *argv[3] = {(char *)"/bin/porttest", (char *)"--worker", name};
  int child = spawn_args("/bin/porttest", 3, argv);
  int result = child <= 0 ? 3 : wake_parent(port, child, stats);
  int status = 0, joined = 0;
  if (child > 0 && result != 0 && kill_process(child) < 0) result = 5;
  for (unsigned i = 0U; i < 500U && child > 0 && joined == 0; ++i) {
    joined = waitpid(child, &status, WNOHANG);
    if (joined == 0) OS1_sleep(10);
  }
  if (child > 0 && (joined != child || !WIFEXITED(status) ||
                    WEXITSTATUS(status) != 0)) result = 6;
  if (OS1low_handle_close(port) != 0) result = 7;
  return result;
}

int main(int argc, const struct porttest_args *args) {
  if (argc == 3 && args != NULL && args->values[1] != NULL &&
      strcmp(args->values[1], "--worker") == 0)
    return args->values[2] != NULL ? wake_worker(args->values[2]) : 1;
  uint64_t started = os1_mono_ns();
  int packet = packet_test();
  uint64_t packet_end = os1_mono_ns();
  int services = services_survive_forged_slots();
  uint64_t services_end = os1_mono_ns();
  struct wake_stats stats = {0};
  int wake = wake_race_test(&stats);
  uint64_t wake_end = os1_mono_ns();
  int failures = (packet != 0) + (services != 0) + (wake != 0);
  if (packet_end < started || services_end < packet_end || wake_end < services_end)
    return 1;
  if (printf("[porttest] rounds=%u max_round_ns=%llu max_round_index=%u\n",
             stats.rounds, (unsigned long long)stats.max_round_ns,
             stats.max_round_index) < 0) return 1;
  if (printf("[porttest] phase_ns packet=%llu services=%llu wake=%llu\n",
             (unsigned long long)(packet_end - started),
             (unsigned long long)(services_end - packet_end),
             (unsigned long long)(wake_end - services_end)) < 0) return 1;
  if (printf("[porttest] packet=%d forged-services=%d wake-race=%d\n", packet, services, wake) < 0)
    return 1;
  if (printf("[porttest] done: %d/3 passed, %d failure(s)\n",
             3 - failures, failures) < 0) return 1;
  return failures;
}
