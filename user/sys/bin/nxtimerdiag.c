/*
 * nxtimerdiag - OS1nx_time diagnostics client.
 *
 * The privileged service lives in /sys/services/nxtimerd.  This tool only
 * validates service snapshots, timer-queue telemetry and public clock/sleep
 * APIs; it never owns the TIME hardware-facing capabilities.
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
#define TIMERDIAG_STRESS_CHILDREN 24U
#define TIMERDIAG_STRESS_MIN_ACTIVE 16U
#define TIMERDIAG_STRESS_CHILD_SLEEP_MS 1500
#define TIMERDIAG_STRESS_ARM_POLLS 40U
#define TIMERDIAG_STRESS_REAP_POLLS 400U
#define TIMERDIAG_STRESS_POLL_MS 10

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

static uint64_t interval_gap(uint64_t value_low, uint64_t value_high,
                             uint64_t lower, uint64_t upper) {
  if (value_high < lower)
    return lower - value_high;
  if (value_low > upper)
    return value_low - upper;
  return 0U;
}

static uint64_t point_gap(uint64_t value, uint64_t lower, uint64_t upper) {
  return interval_gap(value, value, lower, upper);
}

static uint64_t millis_gap(long milliseconds, uint64_t lower,
                           uint64_t upper) {
  uint64_t value_low;
  uint64_t value_high;

  if (milliseconds < 0)
    return UINT64_MAX;
  if ((uint64_t)milliseconds > UINT64_MAX / UINT64_C(1000000))
    return UINT64_MAX;
  value_low = (uint64_t)milliseconds * UINT64_C(1000000);
  if (value_low > UINT64_MAX - UINT64_C(999999))
    return UINT64_MAX;
  value_high = value_low + UINT64_C(999999);
  return interval_gap(value_low, value_high, lower, upper);
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

static uint64_t timer_counter_delta(uint64_t before, uint64_t after) {
  return after >= before ? after - before : 0U;
}

static int timer_stats_fetch_print(
    const char *phase, struct os1_timer_stats_snapshot_v3 *snapshot) {
  if (OS1_time_service_timer_stats(snapshot) != 0) {
    printf("[TIMERDIAG] TIMERQ phase=%s status=UNAVAILABLE\n", phase);
    return -1;
  }
  printf("[TIMERDIAG] TIMERQ phase=%s cpus=%u mono_ns=%llu\n",
         phase, snapshot->cpu_count,
         (unsigned long long)snapshot->monotonic_ns);
  for (uint32_t index = 0U;
       index < snapshot->cpu_count && index < OS1_TIME_MAX_CPUS; ++index) {
    const struct os1_timer_cpu_stats_v3 *cpu = &snapshot->cpu[index];
    uint64_t avg_late_ns =
        cpu->late_fire_count != 0U
            ? cpu->fire_lateness_ns_total / cpu->late_fire_count
            : 0U;
    uint64_t avg_event_late_ns =
        cpu->clockevent_late_count != 0U
            ? cpu->clockevent_lateness_ns_total / cpu->clockevent_late_count
            : 0U;
    printf("[TIMERDIAG] TIMERQ phase=%s cpu=%u depth=%u high=%u "
           "arm=%llu cancel=%llu fire=%llu scan=%llu passes=%llu "
           "late_fire=%llu avg_late_ns=%llu max_late_ns=%llu "
           "event_late=%llu event_avg_ns=%llu event_max_ns=%llu "
           "abs_prog=%llu max_scan=%u max_fire=%u\n",
           phase, cpu->cpu_id, cpu->queue_depth, cpu->queue_high_water,
           (unsigned long long)cpu->arm_count,
           (unsigned long long)cpu->cancel_count,
           (unsigned long long)cpu->fire_count,
           (unsigned long long)cpu->scan_count,
           (unsigned long long)cpu->scan_pass_count,
           (unsigned long long)cpu->late_fire_count,
           (unsigned long long)avg_late_ns,
           (unsigned long long)cpu->max_fire_lateness_ns,
           (unsigned long long)cpu->clockevent_late_count,
           (unsigned long long)avg_event_late_ns,
           (unsigned long long)cpu->max_clockevent_lateness_ns,
           (unsigned long long)cpu->absolute_program_count,
           cpu->max_scan_per_pass, cpu->max_fire_per_pass);
  }
  return snapshot->cpu_count != 0U ? 0 : -1;
}

static void timer_stats_print_delta(
    const struct os1_timer_stats_snapshot_v3 *before,
    const struct os1_timer_stats_snapshot_v3 *after) {
  uint32_t count = before->cpu_count < after->cpu_count
      ? before->cpu_count : after->cpu_count;
  for (uint32_t index = 0U; index < count; ++index) {
    const struct os1_timer_cpu_stats_v3 *left = &before->cpu[index];
    const struct os1_timer_cpu_stats_v3 *right = &after->cpu[index];
    if (left->cpu_id != right->cpu_id)
      continue;
    uint64_t scans = timer_counter_delta(left->scan_count, right->scan_count);
    uint64_t passes =
        timer_counter_delta(left->scan_pass_count, right->scan_pass_count);
    uint64_t late =
        timer_counter_delta(left->late_fire_count, right->late_fire_count);
    uint64_t late_ns = timer_counter_delta(left->fire_lateness_ns_total,
                                           right->fire_lateness_ns_total);
    uint64_t event_late = timer_counter_delta(left->clockevent_late_count,
                                              right->clockevent_late_count);
    uint64_t event_late_ns =
        timer_counter_delta(left->clockevent_lateness_ns_total,
                            right->clockevent_lateness_ns_total);
    uint64_t programs = timer_counter_delta(left->absolute_program_count,
                                            right->absolute_program_count);
    uint64_t avg_scan_x1000 = passes != 0U ? (scans * 1000U) / passes : 0U;
    uint64_t avg_late_ns = late != 0U ? late_ns / late : 0U;
    uint64_t event_avg_ns =
        event_late != 0U ? event_late_ns / event_late : 0U;
    printf("[TIMERDIAG] TIMERQ_DELTA cpu=%u arm=%llu cancel=%llu fire=%llu "
           "scan=%llu passes=%llu avg_scan_x1000=%llu late_fire=%llu "
           "avg_late_ns=%llu max_late_ns=%llu event_late=%llu "
           "event_avg_ns=%llu event_max_ns=%llu abs_prog=%llu "
           "depth=%u high=%u\n",
           right->cpu_id,
           (unsigned long long)timer_counter_delta(left->arm_count,
                                                   right->arm_count),
           (unsigned long long)timer_counter_delta(left->cancel_count,
                                                   right->cancel_count),
           (unsigned long long)timer_counter_delta(left->fire_count,
                                                   right->fire_count),
           (unsigned long long)scans, (unsigned long long)passes,
           (unsigned long long)avg_scan_x1000, (unsigned long long)late,
           (unsigned long long)avg_late_ns,
           (unsigned long long)right->max_fire_lateness_ns,
           (unsigned long long)event_late,
           (unsigned long long)event_avg_ns,
           (unsigned long long)right->max_clockevent_lateness_ns,
           (unsigned long long)programs,
           right->queue_depth, right->queue_high_water);
  }
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

struct diag_post_clocks {
  long get_time_ms;
  uint64_t wake_mono;
  uint64_t wake_cpu;
  uint64_t after_mono;
  uint64_t after_cpu;
  uint64_t get_gap;
  uint64_t mono_gap;
  uint64_t cpu_gap;
  int mono_rc;
  int cpu_rc;
};

static void diag_read_post_clocks(struct diag_post_clocks *out) {
  struct timespec posix_mono = {0, 0};
  struct timespec posix_cpu = {0, 0};
  uint64_t lower;
  uint64_t upper;

  out->wake_mono = os1_mono_ns();
  out->wake_cpu = os1_cpu_ns();

  lower = os1_mono_ns();
  out->get_time_ms = get_time();
  upper = os1_mono_ns();
  out->get_gap = millis_gap(out->get_time_ms, lower, upper);

  lower = os1_mono_ns();
  out->mono_rc = clock_gettime(CLOCK_MONOTONIC, &posix_mono);
  upper = os1_mono_ns();
  out->mono_gap = point_gap(timespec_ns(&posix_mono), lower, upper);

  lower = os1_cpu_ns();
  out->cpu_rc = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &posix_cpu);
  upper = os1_cpu_ns();
  out->cpu_gap = point_gap(timespec_ns(&posix_cpu), lower, upper);
  out->after_mono = os1_mono_ns();
  out->after_cpu = os1_cpu_ns();
}

static int diag_do_sleep(int is_posix) {
  if (is_posix != 0) {
    struct timespec request = {0, (long)TIMERDIAG_SLEEP_NS};
    struct timespec remaining = {0, 0};
    return nanosleep(&request, &remaining);
  }
  OS1_sleep((int)TIMERDIAG_SLEEP_MS);
  return 0;
}

static void diag_sample(struct diag_state *state, unsigned int index) {
  struct diag_post_clocks post;
  const int is_posix = (int)(index & 1U);
  const char *api = is_posix != 0 ? "nanosleep" : "OS1_sleep";
  const int emit = index < 2U || (index & 31U) == 0U;
  long before_ms = get_time();
  uint64_t before_mono = os1_mono_ns();
  uint64_t before_cpu = os1_cpu_ns();
  int sleep_rc;

  if (emit != 0)
    printf("[TIMERDIAG] PRE i=%u api=%s\n", index, api);
  sleep_rc = diag_do_sleep(is_posix);
  diag_read_post_clocks(&post);

  uint64_t elapsed = post.wake_mono >= before_mono
                         ? post.wake_mono - before_mono : 0U;
  uint64_t cpu_delta = post.wake_cpu >= before_cpu
                           ? post.wake_cpu - before_cpu : 0U;
  const char *status = diag_status(sleep_rc, post.mono_rc, post.cpu_rc,
                                   elapsed, post.get_gap, post.mono_gap,
                                   post.cpu_gap);
  if (post.get_time_ms < before_ms)
    status = "CLOCK_REGRESSION";
  diag_account(state, is_posix, elapsed, post.get_time_ms, post.after_mono,
               post.after_cpu, post.get_gap, status);
  if (emit != 0 || status[0] != 'O')
    printf("[TIMERDIAG] POST i=%u api=%s status=%s get_time_ms=%ld "
           "elapsed_ns=%llu cpu_delta_ns=%llu get_time_gap_ns=%llu "
           "libc_mono_gap_ns=%llu libc_cpu_gap_ns=%llu\n",
           index, api, status, post.get_time_ms, (unsigned long long)elapsed,
           (unsigned long long)cpu_delta, (unsigned long long)post.get_gap,
           (unsigned long long)post.mono_gap,
           (unsigned long long)post.cpu_gap);
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

static uint32_t timer_stats_total_depth(
    const struct os1_timer_stats_snapshot_v3 *snapshot) {
  uint32_t total = 0U;
  uint32_t count = snapshot->cpu_count < OS1_TIME_MAX_CPUS
                       ? snapshot->cpu_count : OS1_TIME_MAX_CPUS;
  for (uint32_t index = 0U; index < count; ++index)
    total += snapshot->cpu[index].queue_depth;
  return total;
}

static unsigned int timer_stress_spawn(int *pids) {
  char *child_argv[2] = {(char *)"nxtimerdiag", (char *)"--stress-child"};
  unsigned int spawned = 0U;
  for (unsigned int index = 0U; index < TIMERDIAG_STRESS_CHILDREN; ++index) {
    int pid = spawn_args("/sys/bin/nxtimerdiag", 2, child_argv);
    if (pid <= 0)
      break;
    pids[spawned] = pid;
    ++spawned;
  }
  return spawned;
}

static uint32_t timer_stress_wait_armed(
    struct os1_timer_stats_snapshot_v3 *snapshot) {
  uint32_t depth = 0U;
  for (unsigned int poll = 0U; poll < TIMERDIAG_STRESS_ARM_POLLS; ++poll) {
    if (OS1_time_service_timer_stats(snapshot) != 0)
      return 0U;
    depth = timer_stats_total_depth(snapshot);
    if (depth >= TIMERDIAG_STRESS_MIN_ACTIVE)
      return depth;
    OS1_sleep(TIMERDIAG_STRESS_POLL_MS);
  }
  return depth;
}

static unsigned int timer_stress_reap(int *pids, unsigned int count) {
  unsigned int remaining = count;
  for (unsigned int poll = 0U;
       poll < TIMERDIAG_STRESS_REAP_POLLS && remaining != 0U; ++poll) {
    for (unsigned int index = 0U; index < count; ++index) {
      if (pids[index] <= 0)
        continue;
      int status = wait(pids[index]);
      if (status == pids[index] || status == -2) {
        pids[index] = 0;
        --remaining;
      }
    }
    if (remaining != 0U)
      OS1_sleep(TIMERDIAG_STRESS_POLL_MS);
  }
  return remaining;
}

static void timer_stress_cleanup(int *pids, unsigned int count) {
  for (unsigned int index = 0U; index < count; ++index) {
    if (pids[index] <= 0)
      continue;
    int status = kill_process(pids[index]);
    if (status == 0 || status == -ESRCH)
      pids[index] = 0;
  }
}

static int timer_queue_stress(void) {
  static struct os1_timer_stats_snapshot_v3 before;
  static struct os1_timer_stats_snapshot_v3 during;
  static struct os1_timer_stats_snapshot_v3 after;
  int pids[TIMERDIAG_STRESS_CHILDREN] = {0};
  if (timer_stats_fetch_print("stress-before", &before) != 0)
    return 1;
  unsigned int spawned = timer_stress_spawn(pids);
  uint32_t depth = timer_stress_wait_armed(&during);
  printf("[TIMERDIAG] STRESS spawned=%u active_depth=%u target=%u\n",
         spawned, depth, TIMERDIAG_STRESS_MIN_ACTIVE);
  if (timer_stats_fetch_print("stress-during", &during) != 0) {
    timer_stress_cleanup(pids, spawned);
    return 1;
  }
  unsigned int remaining = timer_stress_reap(pids, spawned);
  if (remaining != 0U)
    timer_stress_cleanup(pids, spawned);
  if (timer_stats_fetch_print("stress-after", &after) != 0)
    return 1;
  timer_stats_print_delta(&before, &after);
  int passed = spawned == TIMERDIAG_STRESS_CHILDREN &&
               depth >= TIMERDIAG_STRESS_MIN_ACTIVE && remaining == 0U;
  printf("[TIMERDIAG] STRESS DONE spawned=%u depth=%u remaining=%u status=%s\n",
         spawned, depth, remaining, passed ? "PASS" : "FAIL");
  return passed ? 0 : 1;
}

static int timer_stress_child(void) {
  OS1_sleep(TIMERDIAG_STRESS_CHILD_SLEEP_MS);
  return 0;
}

static int timer_probe(void) {
  static struct os1_timer_stats_snapshot_v3 stats_before;
  static struct os1_timer_stats_snapshot_v3 stats_after;
  struct diag_state state;
  int service_status = print_service_snapshot();
  int stats_before_status = timer_stats_fetch_print("before", &stats_before);

  diag_init(&state);
  if (service_status != 0 || stats_before_status != 0)
    ++state.failures;
  printf("[TIMERDIAG] START samples=%u sleep_ms=%u get_time_ms=%ld "
         "mono_ns=%llu cpu_ns=%llu\n",
         TIMERDIAG_SAMPLES, TIMERDIAG_SLEEP_MS, state.first_get_time,
         (unsigned long long)state.first_mono,
         (unsigned long long)state.first_cpu);
  for (unsigned int index = 0U; index < TIMERDIAG_SAMPLES; ++index)
    diag_sample(&state, index);
  int stats_after_status = timer_stats_fetch_print("after", &stats_after);
  if (stats_after_status != 0)
    ++state.failures;
  if (stats_before_status == 0 && stats_after_status == 0)
    timer_stats_print_delta(&stats_before, &stats_after);
  return diag_finish(&state);
}

int main(int argc, char *argv[]) {
  if (argc < 2 || strcmp(argv[1], "--probe") == 0)
    return timer_probe();
  if (strcmp(argv[1], "--queue-stress") == 0)
    return timer_queue_stress();
  if (strcmp(argv[1], "--stress-child") == 0)
    return timer_stress_child();
  printf("usage: nxtimerdiag [--probe|--queue-stress]\n");
  return 2;
}
