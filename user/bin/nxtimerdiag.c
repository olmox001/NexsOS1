/*
 * nxtimerdiag — userland clock, get_time(), and blocking-sleep probe.
 *
 * Exercises the exact millisecond get_time() path used by nxbar, the
 * OS1_sleep() primitive used by nxbar/nxui/nxinit, and POSIX nanosleep().
 * Compares get_time() with the independent nanosecond monotonic syscall and
 * reports per-API latency distributions without flooding the compositor.
 */
#include <os1.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>

#define TIMERDIAG_SAMPLES 160U
#define TIMERDIAG_SLEEP_MS 33U
#define TIMERDIAG_SLEEP_NS 33000000ULL
#define TIMERDIAG_LATE_NS 250000000ULL
#define TIMERDIAG_CLOCK_TOLERANCE_NS 2000000ULL

struct sleep_stats {
  unsigned int count;
  unsigned int late_10ms;
  unsigned int late_50ms;
  unsigned int late_250ms;
  unsigned long long min_ns;
  unsigned long long max_ns;
  unsigned long long total_ns;
};

static unsigned long long timespec_ns(const struct timespec *ts) {
  return (unsigned long long)ts->tv_sec * 1000000000ULL +
         (unsigned long long)ts->tv_nsec;
}

static unsigned long long abs_diff_u64(unsigned long long a,
                                       unsigned long long b) {
  return a >= b ? a - b : b - a;
}

static void stats_init(struct sleep_stats *s) {
  s->count = 0;
  s->late_10ms = 0;
  s->late_50ms = 0;
  s->late_250ms = 0;
  s->min_ns = ~0ULL;
  s->max_ns = 0;
  s->total_ns = 0;
}

static void stats_add(struct sleep_stats *s, unsigned long long elapsed) {
  ++s->count;
  if (elapsed < s->min_ns)
    s->min_ns = elapsed;
  if (elapsed > s->max_ns)
    s->max_ns = elapsed;
  s->total_ns += elapsed;
  if (elapsed > TIMERDIAG_SLEEP_NS + 10000000ULL)
    ++s->late_10ms;
  if (elapsed > TIMERDIAG_SLEEP_NS + 50000000ULL)
    ++s->late_50ms;
  if (elapsed > TIMERDIAG_LATE_NS)
    ++s->late_250ms;
}

static void stats_print(const char *name, const struct sleep_stats *s) {
  unsigned long long avg =
      s->count ? s->total_ns / (unsigned long long)s->count : 0ULL;
  printf("[TIMERDIAG] API=%s n=%u min_ns=%llu avg_ns=%llu max_ns=%llu "
         "late_10ms=%u late_50ms=%u late_250ms=%u\n",
         name, s->count, s->count ? s->min_ns : 0ULL, avg, s->max_ns,
         s->late_10ms, s->late_50ms, s->late_250ms);
}

int main(void) {
  unsigned int failures = 0;
  unsigned int get_time_regressions = 0;
  unsigned int get_time_mismatches = 0;
  unsigned long long get_time_max_gap_ns = 0;
  struct sleep_stats os1_stats, posix_stats;
  stats_init(&os1_stats);
  stats_init(&posix_stats);

  unsigned long long first_mono = os1_mono_ns();
  unsigned long long first_cpu = os1_cpu_ns();
  unsigned long long previous_mono = first_mono;
  unsigned long long previous_cpu = first_cpu;
  long first_get_time = get_time();
  long previous_get_time = first_get_time;

  printf("[TIMERDIAG] START samples=%u sleep_ms=%u get_time_ms=%ld "
         "mono_ns=%llu cpu_ns=%llu\n",
         TIMERDIAG_SAMPLES, TIMERDIAG_SLEEP_MS, first_get_time,
         first_mono, first_cpu);

  for (unsigned int i = 0; i < TIMERDIAG_SAMPLES; ++i) {
    const int is_posix = (int)(i & 1U);
    const char *api = is_posix ? "nanosleep" : "OS1_sleep";
    /* Include both APIs at startup; thereafter keep serial traffic bounded. */
    int emit_checkpoint = (i < 2U) || ((i & 31U) == 0U);

    if (emit_checkpoint)
      printf("[TIMERDIAG] PRE i=%u api=%s\n", i, api);

    long before_get_time = get_time();
    unsigned long long before_mono = os1_mono_ns();
    unsigned long long before_cpu = os1_cpu_ns();
    int sleep_rc = 0;

    if (is_posix) {
      struct timespec req = {0, (long)TIMERDIAG_SLEEP_NS};
      struct timespec rem = {0, 0};
      sleep_rc = nanosleep(&req, &rem);
    } else {
      OS1_sleep((int)TIMERDIAG_SLEEP_MS);
    }

    unsigned long long after_mono = os1_mono_ns();
    unsigned long long after_cpu = os1_cpu_ns();
    long after_get_time = get_time();
    unsigned long long elapsed =
        after_mono >= before_mono ? after_mono - before_mono : 0ULL;
    unsigned long long cpu_delta =
        after_cpu >= before_cpu ? after_cpu - before_cpu : 0ULL;
    unsigned long long get_time_gap_ns =
        abs_diff_u64((unsigned long long)after_get_time * 1000000ULL,
                     after_mono);

    struct timespec libc_mono = {0, 0};
    struct timespec libc_cpu = {0, 0};
    int mono_rc = clock_gettime(CLOCK_MONOTONIC, &libc_mono);
    int cpu_rc = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &libc_cpu);
    unsigned long long libc_mono_ns =
        mono_rc == 0 ? timespec_ns(&libc_mono) : 0ULL;
    unsigned long long libc_cpu_ns =
        cpu_rc == 0 ? timespec_ns(&libc_cpu) : 0ULL;
    unsigned long long mono_gap =
        abs_diff_u64(libc_mono_ns, after_mono);
    unsigned long long cpu_gap =
        abs_diff_u64(libc_cpu_ns, after_cpu);

    if (is_posix)
      stats_add(&posix_stats, elapsed);
    else
      stats_add(&os1_stats, elapsed);

    const char *status = "OK";
    if (sleep_rc != 0 || mono_rc != 0 || cpu_rc != 0) {
      status = "SYSCALL_ERROR";
      ++failures;
    } else if (after_mono < before_mono || after_cpu < before_cpu ||
               after_get_time < before_get_time) {
      status = "CLOCK_REGRESSION";
      ++failures;
    } else if (elapsed < TIMERDIAG_SLEEP_NS) {
      status = "EARLY_WAKE";
      ++failures;
    } else if (elapsed > TIMERDIAG_LATE_NS) {
      status = "LATE_WAKE";
      ++failures;
    } else if (get_time_gap_ns > TIMERDIAG_CLOCK_TOLERANCE_NS ||
               mono_gap > TIMERDIAG_CLOCK_TOLERANCE_NS ||
               cpu_gap > TIMERDIAG_CLOCK_TOLERANCE_NS) {
      status = "CLOCK_MISMATCH";
      ++failures;
    }

    if (after_get_time < previous_get_time) {
      ++get_time_regressions;
      ++failures;
    }
    if (get_time_gap_ns > TIMERDIAG_CLOCK_TOLERANCE_NS) {
      ++get_time_mismatches;
    }
    if (get_time_gap_ns > get_time_max_gap_ns)
      get_time_max_gap_ns = get_time_gap_ns;

    if (emit_checkpoint || status[0] != 'O') {
      printf("[TIMERDIAG] POST i=%u api=%s status=%s get_time_ms=%ld "
             "elapsed_ns=%llu cpu_delta_ns=%llu get_time_gap_ns=%llu "
             "libc_mono_gap_ns=%llu libc_cpu_gap_ns=%llu\n",
             i, api, status, after_get_time, elapsed, cpu_delta,
             get_time_gap_ns, mono_gap, cpu_gap);
    }

    if (after_mono < previous_mono || after_cpu < previous_cpu) {
      printf("[TIMERDIAG] GLOBAL_REGRESSION i=%u\n", i);
      ++failures;
    }
    previous_mono = after_mono;
    previous_cpu = after_cpu;
    previous_get_time = after_get_time;
  }

  long final_get_time = get_time();
  unsigned long long final_mono = os1_mono_ns();
  unsigned long long mono_elapsed_ms =
      (final_mono - first_mono) / 1000000ULL;
  unsigned long long get_time_elapsed_ms =
      final_get_time >= first_get_time
          ? (unsigned long long)(final_get_time - first_get_time)
          : 0ULL;
  unsigned long long elapsed_gap_ms =
      abs_diff_u64(get_time_elapsed_ms, mono_elapsed_ms);
  if (elapsed_gap_ms > 2ULL || get_time_regressions != 0U)
    ++failures;

  stats_print("OS1_sleep", &os1_stats);
  stats_print("nanosleep", &posix_stats);
  printf("[TIMERDIAG] GET_TIME start_ms=%ld end_ms=%ld delta_ms=%llu "
         "mono_delta_ms=%llu max_sample_gap_ns=%llu regressions=%u "
         "mismatches=%u elapsed_gap_ms=%llu\n",
         first_get_time, final_get_time, get_time_elapsed_ms,
         mono_elapsed_ms, get_time_max_gap_ns, get_time_regressions,
         get_time_mismatches, elapsed_gap_ms);
  printf("[TIMERDIAG] DONE elapsed_ns=%llu cpu_ns=%llu failures=%u\n",
         final_mono - first_mono, previous_cpu - first_cpu, failures);
  return failures ? 1 : 0;
}
