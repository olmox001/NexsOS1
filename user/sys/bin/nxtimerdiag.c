/*
 * nxtimerdiag - supervised OS1nx_time service and timer diagnostics client.
 *
 * --service: owns the privileged OS1_NS_TIME capability and serves immutable
 *            per-CPU snapshots through a named capability port.
 * --probe:   validates service snapshots plus the public sleep/clock APIs.
 */
#include <os1_time_service.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TIMERDIAG_SAMPLES 160U
#define TIMERDIAG_SLEEP_MS 33U
#define TIMERDIAG_SLEEP_NS UINT64_C(33000000)
#define TIMERDIAG_LATE_NS UINT64_C(250000000)
#define TIMERDIAG_CLOCK_TOLERANCE_NS UINT64_C(2000000)
#define TIMER_SERVICE_CLAIM_TRIES 20U
#define TIMER_SERVICE_CLAIM_WAIT_MS 100

struct sleep_stats {
  unsigned int count;
  unsigned int late_10ms;
  unsigned int late_50ms;
  unsigned int late_250ms;
  uint64_t min_ns;
  uint64_t max_ns;
  uint64_t total_ns;
};

struct diag_state {
  unsigned int failures;
  unsigned int get_time_regressions;
  unsigned int get_time_mismatches;
  uint64_t get_time_max_gap_ns;
  uint64_t first_mono;
  uint64_t first_cpu;
  uint64_t previous_mono;
  uint64_t previous_cpu;
  long first_get_time;
  long previous_get_time;
  struct sleep_stats os1_stats;
  struct sleep_stats posix_stats;
};

static uint64_t timespec_ns(const struct timespec *value) {
  return (uint64_t)value->tv_sec * UINT64_C(1000000000) +
         (uint64_t)value->tv_nsec;
}

static uint64_t abs_diff_u64(uint64_t left, uint64_t right) {
  return left >= right ? left - right : right - left;
}

static void stats_init(struct sleep_stats *stats) {
  memset(stats, 0, sizeof(*stats));
  stats->min_ns = UINT64_MAX;
}

static void stats_add(struct sleep_stats *stats, uint64_t elapsed) {
  ++stats->count;
  if (elapsed < stats->min_ns)
    stats->min_ns = elapsed;
  if (elapsed > stats->max_ns)
    stats->max_ns = elapsed;
  stats->total_ns += elapsed;
  if (elapsed > TIMERDIAG_SLEEP_NS + UINT64_C(10000000))
    ++stats->late_10ms;
  if (elapsed > TIMERDIAG_SLEEP_NS + UINT64_C(50000000))
    ++stats->late_50ms;
  if (elapsed > TIMERDIAG_LATE_NS)
    ++stats->late_250ms;
}

static void stats_print(const char *name, const struct sleep_stats *stats) {
  uint64_t average = stats->count != 0U
      ? stats->total_ns / (uint64_t)stats->count : 0U;
  printf("[TIMERDIAG] API=%s n=%u min_ns=%llu avg_ns=%llu max_ns=%llu "
         "late_10ms=%u late_50ms=%u late_250ms=%u\n",
         name, stats->count,
         (unsigned long long)(stats->count != 0U ? stats->min_ns : 0U),
         (unsigned long long)average, (unsigned long long)stats->max_ns,
         stats->late_10ms, stats->late_50ms, stats->late_250ms);
}

static int service_write_full(int fd, const void *buffer, unsigned long size) {
  const unsigned char *bytes = (const unsigned char *)buffer;
  unsigned long done = 0U;

  for (unsigned long step = 0U; step < size && done < size; ++step) {
    long written = write(fd, (const char *)bytes + done, size - done);
    if (written <= 0)
      return -1;
    done += (unsigned long)written;
  }
  return done == size ? 0 : -1;
}

static void service_handle_snapshot(const struct ipc_message *message,
                                    int time_handle) {
  static struct os1_time_snapshot_v1 snapshot;
  int reply = -1;

  memcpy(&reply, message->payload, sizeof(reply));
  if (reply < 0)
    return;
  if (message->data1 != OS1_TIME_SERVICE_REQUEST_SNAPSHOT ||
      message->data2 != OS1_TIME_SERVICE_VERSION) {
    close(reply);
    return;
  }
  long status = OS1low_time_snapshot(time_handle, &snapshot);
  if (status == (long)sizeof(snapshot))
    (void)service_write_full(reply, &snapshot, sizeof(snapshot));
  close(reply);
}

static int time_service_claim(void) {
  for (unsigned int attempt = 0U; attempt < TIMER_SERVICE_CLAIM_TRIES;
       ++attempt) {
    int port = OS1_port_create(OS1NX_PORT_TIME);
    if (port >= 0)
      return port;
    OS1_sleep(TIMER_SERVICE_CLAIM_WAIT_MS);
  }
  return -1;
}

static int time_service(void) {
  int time_handle = (int)OS1low_time_open();
  int port;

  if (time_handle < 0) {
    printf("[TIMERDIAG] service cannot acquire TIME capability: %d\n",
           time_handle);
    return 1;
  }
  port = time_service_claim();
  if (port < 0) {
    printf("[TIMERDIAG] service cannot publish %s\n", OS1NX_PORT_TIME);
    OS1low_handle_close(time_handle);
    return 1;
  }
  printf("[TIMERDIAG] service listening on %s\n", OS1NX_PORT_TIME);
  /* Event-loop exception to Power-of-Ten Rule 2: the service is intentionally
   * resident. Every individual request path and retry inside it is bounded. */
  for (;;) {
    struct ipc_message message;
    long received = OS1_port_recv(port, &message);
    if (received < 0)
      continue;
    if (received == 0)
      continue;
    if (message.type == OS1_TIME_SERVICE_IPC_TYPE)
      service_handle_snapshot(&message, time_handle);
  }
}

static int print_service_snapshot(void) {
  struct os1_time_snapshot_v1 snapshot;

  if (OS1_time_service_snapshot(&snapshot) != 0) {
    printf("[TIMERDIAG] SERVICE status=UNAVAILABLE\n");
    return -1;
  }
  printf("[TIMERDIAG] SERVICE cpus=%u mono_ns=%llu freq_hz=%llu tick_hz=%llu\n",
         snapshot.cpu_count, (unsigned long long)snapshot.monotonic_ns,
         (unsigned long long)snapshot.counter_frequency_hz,
         (unsigned long long)snapshot.scheduler_tick_hz);
  for (uint32_t index = 0U;
       index < snapshot.cpu_count && index < OS1_TIME_MAX_CPUS; ++index) {
    const struct os1_time_cpu_v1 *cpu = &snapshot.cpu[index];
    printf("[TIMERDIAG] CPU id=%u flags=0x%x seq=%llu tick=%llu late=%llu "
           "mono_ns=%llu next_ns=%llu\n",
           cpu->cpu_id, cpu->flags, (unsigned long long)cpu->sequence,
           (unsigned long long)cpu->tick_count,
           (unsigned long long)cpu->late_tick_count,
           (unsigned long long)cpu->monotonic_ns,
           (unsigned long long)cpu->next_deadline_ns);
  }
  return snapshot.cpu_count != 0U ? 0 : -1;
}

static void diag_init(struct diag_state *state) {
  memset(state, 0, sizeof(*state));
  stats_init(&state->os1_stats);
  stats_init(&state->posix_stats);
  state->first_mono = os1_mono_ns();
  state->first_cpu = os1_cpu_ns();
  state->previous_mono = state->first_mono;
  state->previous_cpu = state->first_cpu;
  state->first_get_time = get_time();
  state->previous_get_time = state->first_get_time;
}

static const char *diag_status(int sleep_rc, int mono_rc, int cpu_rc,
                               uint64_t elapsed, uint64_t get_gap,
                               uint64_t mono_gap, uint64_t cpu_gap) {
  if (sleep_rc != 0 || mono_rc != 0 || cpu_rc != 0)
    return "SYSCALL_ERROR";
  if (elapsed < TIMERDIAG_SLEEP_NS)
    return "EARLY_WAKE";
  if (elapsed > TIMERDIAG_LATE_NS)
    return "LATE_WAKE";
  if (get_gap > TIMERDIAG_CLOCK_TOLERANCE_NS ||
      mono_gap > TIMERDIAG_CLOCK_TOLERANCE_NS ||
      cpu_gap > TIMERDIAG_CLOCK_TOLERANCE_NS)
    return "CLOCK_MISMATCH";
  return "OK";
}

static void diag_account(struct diag_state *state, int is_posix,
                         uint64_t elapsed, long after_get_time,
                         uint64_t after_mono, uint64_t after_cpu,
                         uint64_t get_gap, const char *status) {
  if (is_posix != 0)
    stats_add(&state->posix_stats, elapsed);
  else
    stats_add(&state->os1_stats, elapsed);
  if (status[0] != 'O')
    ++state->failures;
  if (after_get_time < state->previous_get_time) {
    ++state->get_time_regressions;
    ++state->failures;
  }
  if (get_gap > TIMERDIAG_CLOCK_TOLERANCE_NS)
    ++state->get_time_mismatches;
  if (get_gap > state->get_time_max_gap_ns)
    state->get_time_max_gap_ns = get_gap;
  if (after_mono < state->previous_mono || after_cpu < state->previous_cpu)
    ++state->failures;
  state->previous_mono = after_mono;
  state->previous_cpu = after_cpu;
  state->previous_get_time = after_get_time;
}

static void diag_sample(struct diag_state *state, unsigned int index) {
  const int is_posix = (int)(index & 1U);
  const char *api = is_posix != 0 ? "nanosleep" : "OS1_sleep";
  const int emit = index < 2U || (index & 31U) == 0U;
  long before_ms = get_time();
  uint64_t before_mono = os1_mono_ns();
  uint64_t before_cpu = os1_cpu_ns();
  int sleep_rc = 0;

  if (emit != 0)
    printf("[TIMERDIAG] PRE i=%u api=%s\n", index, api);
  if (is_posix != 0) {
    struct timespec request = {0, (long)TIMERDIAG_SLEEP_NS};
    struct timespec remaining = {0, 0};
    sleep_rc = nanosleep(&request, &remaining);
  } else {
    OS1_sleep((int)TIMERDIAG_SLEEP_MS);
  }

  uint64_t after_mono = os1_mono_ns();
  uint64_t after_cpu = os1_cpu_ns();
  long after_ms = get_time();
  struct timespec posix_mono = {0, 0};
  struct timespec posix_cpu = {0, 0};
  int mono_rc = clock_gettime(CLOCK_MONOTONIC, &posix_mono);
  int cpu_rc = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &posix_cpu);
  uint64_t elapsed = after_mono >= before_mono ? after_mono - before_mono : 0U;
  uint64_t cpu_delta = after_cpu >= before_cpu ? after_cpu - before_cpu : 0U;
  uint64_t get_gap = abs_diff_u64((uint64_t)after_ms * UINT64_C(1000000),
                                  after_mono);
  uint64_t mono_gap = abs_diff_u64(timespec_ns(&posix_mono), after_mono);
  uint64_t cpu_gap = abs_diff_u64(timespec_ns(&posix_cpu), after_cpu);
  const char *status = diag_status(sleep_rc, mono_rc, cpu_rc, elapsed,
                                   get_gap, mono_gap, cpu_gap);

  if (after_ms < before_ms)
    status = "CLOCK_REGRESSION";
  diag_account(state, is_posix, elapsed, after_ms, after_mono, after_cpu,
               get_gap, status);
  if (emit != 0 || status[0] != 'O')
    printf("[TIMERDIAG] POST i=%u api=%s status=%s get_time_ms=%ld "
           "elapsed_ns=%llu cpu_delta_ns=%llu get_time_gap_ns=%llu "
           "libc_mono_gap_ns=%llu libc_cpu_gap_ns=%llu\n",
           index, api, status, after_ms, (unsigned long long)elapsed,
           (unsigned long long)cpu_delta, (unsigned long long)get_gap,
           (unsigned long long)mono_gap, (unsigned long long)cpu_gap);
}

static int diag_finish(struct diag_state *state) {
  long final_ms = get_time();
  uint64_t final_mono = os1_mono_ns();
  uint64_t mono_elapsed_ms =
      (final_mono - state->first_mono) / UINT64_C(1000000);
  uint64_t get_elapsed_ms = final_ms >= state->first_get_time
      ? (uint64_t)(final_ms - state->first_get_time) : 0U;
  uint64_t elapsed_gap = abs_diff_u64(get_elapsed_ms, mono_elapsed_ms);

  if (elapsed_gap > 2U || state->get_time_regressions != 0U)
    ++state->failures;
  stats_print("OS1_sleep", &state->os1_stats);
  stats_print("nanosleep", &state->posix_stats);
  printf("[TIMERDIAG] GET_TIME start_ms=%ld end_ms=%ld delta_ms=%llu "
         "mono_delta_ms=%llu max_sample_gap_ns=%llu regressions=%u "
         "mismatches=%u elapsed_gap_ms=%llu\n",
         state->first_get_time, final_ms, (unsigned long long)get_elapsed_ms,
         (unsigned long long)mono_elapsed_ms,
         (unsigned long long)state->get_time_max_gap_ns,
         state->get_time_regressions, state->get_time_mismatches,
         (unsigned long long)elapsed_gap);
  printf("[TIMERDIAG] DONE elapsed_ns=%llu cpu_ns=%llu failures=%u\n",
         (unsigned long long)(final_mono - state->first_mono),
         (unsigned long long)(state->previous_cpu - state->first_cpu),
         state->failures);
  return state->failures != 0U ? 1 : 0;
}

static int timer_probe(void) {
  struct diag_state state;
  int service_status = print_service_snapshot();

  diag_init(&state);
  if (service_status != 0)
    ++state.failures;
  printf("[TIMERDIAG] START samples=%u sleep_ms=%u get_time_ms=%ld "
         "mono_ns=%llu cpu_ns=%llu\n",
         TIMERDIAG_SAMPLES, TIMERDIAG_SLEEP_MS, state.first_get_time,
         (unsigned long long)state.first_mono,
         (unsigned long long)state.first_cpu);
  for (unsigned int index = 0U; index < TIMERDIAG_SAMPLES; ++index)
    diag_sample(&state, index);
  return diag_finish(&state);
}

int main(int argc, char *argv[]) {
  if (argc >= 2 && strcmp(argv[1], "--service") == 0)
    return time_service();
  if (argc >= 2 && strcmp(argv[1], "--probe") != 0) {
    printf("usage: nxtimerdiag [--service|--probe]\n");
    return 2;
  }
  return timer_probe();
}
