/*
 * NX-CORE time engine.
 *
 * HAL owns raw counter/frequency/compare primitives. NX-CORE owns the clock
 * epoch, scheduler cadence, per-CPU time state and software deadlines.
 */
#include <hal/core.h>
#include <hal/irq_mask_stats.h>
#include <hal/time.h>
#include <kernel/cpu.h>
#include <kernel/list.h>
#include <kernel/printk.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/timer.h>

#define TIMER_SNAPSHOT_RETRIES 4U
#define TIMER_CANCEL_RETRIES 8U

extern volatile int panic_flag;

/* Compatibility telemetry only. No scheduler/sleep decision uses jiffies. */
volatile uint64_t jiffies;

struct timer_cpu_buffer {
  uint64_t publish_sequence;
  struct os1_time_cpu_v1 slots[2];
};

static struct timer_cpu_buffer timer_cpu_buffers[MAX_CPUS];
static uint64_t timer_cpu_late[MAX_CPUS];
static struct OS1libkernel_timer_stats timer_cpu_stats[MAX_CPUS];
static uint64_t time_boot_counter;
static uint64_t time_frequency_hz;
static uint64_t time_snapshot_sequence;
static uint64_t time_event_frequency_hz;
static uint64_t time_event_min_delta_counts;
static uint64_t time_event_max_delta_counts;
static uint32_t time_event_flags;
static uint32_t time_event_deadline_mode;
static uint32_t time_ready;

static uint64_t time_scale_counts(uint64_t counts, uint64_t frequency) {
  __uint128_t scaled;

  if (frequency == 0U)
    panic("%s", "TIME: zero hardware counter frequency");
  scaled = ((__uint128_t)counts * OS1LIBKERNEL_NSEC_PER_SEC) / frequency;
  return scaled > UINT64_MAX ? UINT64_MAX : (uint64_t)scaled;
}

static uint64_t time_counter_to_ns(uint64_t counter) {
  if (counter < time_boot_counter)
    return 0U;
  return time_scale_counts(counter - time_boot_counter, time_frequency_hz);
}

uint64_t OS1libkernel_time_counter_read(void) {
  uint64_t counter = 0U;

  if (nxhal_time_counter_read(&counter) != NXHAL_TIME_OK)
    panic("%s", "TIME: clocksource read failed");
  return counter;
}

void OS1libkernel_time_init(void) {
  struct nxhal_clocksource_v1 source;
  struct nxhal_clockevent_v1 event;

  if (nxhal_time_clocksource_info(&source) != NXHAL_TIME_OK)
    panic("%s", "TIME: no validated HAL clocksource");
  if (nxhal_time_clockevent_info(&event) != NXHAL_TIME_OK)
    panic("%s", "TIME: no validated HAL clockevent");
  if ((source.flags & NXHAL_CLOCKSOURCE_MUST_VERIFY) != 0U)
    pr_warn("%s", "TIME: clocksource requires runtime verification\n");

  time_boot_counter = OS1libkernel_time_counter_read();
  time_frequency_hz = source.frequency_hz;
  time_event_frequency_hz = event.frequency_hz;
  time_event_min_delta_counts = event.min_delta_counts;
  time_event_max_delta_counts = event.max_delta_counts;
  time_event_flags = event.flags;
  time_event_deadline_mode =
      ((event.flags & (NXHAL_CLOCKEVENT_ONESHOT | NXHAL_CLOCKEVENT_ABSOLUTE)) ==
       (NXHAL_CLOCKEVENT_ONESHOT | NXHAL_CLOCKEVENT_ABSOLUTE)) &&
      event.frequency_hz == source.frequency_hz;
  __atomic_store_n(&jiffies, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&time_ready, 1U, __ATOMIC_RELEASE);
}

void OS1libkernel_timer_core_init(void) {
  memset(timer_cpu_stats, 0, sizeof(timer_cpu_stats));
  for (uint32_t cpu_id = 0U; cpu_id < MAX_CPUS; ++cpu_id) {
    INIT_LIST_HEAD(&cpu_data[cpu_id].timer_list);
    spin_lock_init(&cpu_data[cpu_id].timer_lock);
    cpu_data[cpu_id].timer_next_deadline_ns = 0U;
  }
}

uint64_t OS1libkernel_time_counter_frequency(void) {
  if (__atomic_load_n(&time_ready, __ATOMIC_ACQUIRE) == 0U)
    panic("%s", "TIME: frequency requested before initialization");
  return time_frequency_hz;
}

uint64_t OS1libkernel_time_counts_to_ns(uint64_t counts) {
  return time_scale_counts(counts, OS1libkernel_time_counter_frequency());
}

uint64_t OS1libkernel_time_monotonic_ns(void) {
  if (__atomic_load_n(&time_ready, __ATOMIC_ACQUIRE) == 0U)
    panic("%s", "TIME: monotonic clock requested before initialization");
  return time_counter_to_ns(OS1libkernel_time_counter_read());
}

int OS1libkernel_time_deadline_after_ns(uint64_t delta_ns,
                                        uint64_t *deadline_ns) {
  uint64_t now;

  if (deadline_ns == NULL || delta_ns == 0U)
    return -1;
  now = OS1libkernel_time_monotonic_ns();
  if (delta_ns > UINT64_MAX - now)
    return -1;
  *deadline_ns = now + delta_ns;
  return 0;
}

bool OS1libkernel_time_deadline_reached(uint64_t deadline_ns) {
  if (deadline_ns == 0U)
    return true;
  return OS1libkernel_time_monotonic_ns() >= deadline_ns;
}

static uint64_t timer_interval_counts(struct cpu_info *cpu) {
  uint64_t interval = time_frequency_hz / OS1LIBKERNEL_TIMER_HZ;
  uint64_t remainder = time_frequency_hz % OS1LIBKERNEL_TIMER_HZ;

  if (interval == 0U)
    interval = 1U;
  cpu->tick_error_acc += remainder;
  if (cpu->tick_error_acc >= OS1LIBKERNEL_TIMER_HZ) {
    ++interval;
    cpu->tick_error_acc -= OS1LIBKERNEL_TIMER_HZ;
  }
  return interval;
}

static uint64_t timer_checked_add(uint64_t value, uint64_t delta) {
  if (delta > UINT64_MAX - value)
    panic("%s", "TIME: hardware counter deadline overflow");
  return value + delta;
}

static int timer_uses_deadline_event(void) {
  return time_event_deadline_mode != 0U &&
         time_event_frequency_hz == time_frequency_hz;
}

static uint64_t timer_ns_to_counter_ceil(uint64_t deadline_ns) {
  __uint128_t scaled = (__uint128_t)deadline_ns * time_frequency_hz;
  __uint128_t count = scaled / OS1LIBKERNEL_NSEC_PER_SEC;
  uint64_t available = UINT64_MAX - time_boot_counter;

  /* Check the wide quotient before narrowing. A far-future deadline must
   * saturate, never wrap to an earlier physical clockevent. */
  if (count > available)
    return UINT64_MAX;
  uint64_t delta = (uint64_t)count;
  if ((scaled % OS1LIBKERNEL_NSEC_PER_SEC) != 0U) {
    if (delta == available)
      return UINT64_MAX;
    ++delta;
  }
  return time_boot_counter + delta;
}

static uint64_t timer_clamp_target(uint64_t now, uint64_t target) {
  uint64_t min_target = timer_checked_add(now, time_event_min_delta_counts);
  uint64_t max_delta = time_event_max_delta_counts;
  uint64_t max_target =
      max_delta > UINT64_MAX - now ? UINT64_MAX : now + max_delta;
  if (target < min_target)
    return min_target;
  if (target > max_target)
    return max_target;
  return target;
}

static uint64_t timer_idle_delta_counts(void) {
  /* With no logical deadline there is nothing to convert from nanoseconds.
   * Sleep for the provider's longest representable absolute-event delta. */
  return time_event_max_delta_counts;
}

static uint64_t timer_target_for_deadline(uint64_t now, uint64_t deadline_ns) {
  if (deadline_ns == 0U) {
    uint64_t delta = timer_idle_delta_counts();
    return delta > UINT64_MAX - now ? UINT64_MAX : now + delta;
  }
  return timer_clamp_target(now, timer_ns_to_counter_ceil(deadline_ns));
}

static uint64_t timer_min_deadline(uint64_t first, uint64_t second) {
  if (first == 0U)
    return second;
  if (second == 0U)
    return first;
  return first < second ? first : second;
}

static uint64_t timer_current_deadline_ns(struct cpu_info *cpu) {
  uint64_t software =
      __atomic_load_n(&cpu->timer_next_deadline_ns, __ATOMIC_ACQUIRE);
  struct process *current = cpu->current_task;
  uint64_t quantum = current == NULL
                         ? 0U
                         : __atomic_load_n(&current->quantum_deadline_ns,
                                           __ATOMIC_ACQUIRE);
  return timer_min_deadline(software, quantum);
}

static void timer_program_target(struct cpu_info *cpu, uint64_t target) {
  int status;

  if ((time_event_flags & NXHAL_CLOCKEVENT_ABSOLUTE) == 0U) {
    if ((time_event_flags & NXHAL_CLOCKEVENT_PERIODIC) != 0U)
      return;
    panic("%s", "TIME: clockevent has no scheduling mode");
  }
  status = nxhal_time_event_program_absolute(target);
  if (status != NXHAL_TIME_OK)
    panic("TIME: absolute clockevent programming failed: %d", status);
  if (cpu == NULL || cpu->cpu_id >= MAX_CPUS)
    panic("%s", "TIME: absolute clockevent programmed without CPU state");
  if (timer_cpu_stats[cpu->cpu_id].absolute_program_count != UINT64_MAX)
    ++timer_cpu_stats[cpu->cpu_id].absolute_program_count;
}

static void timer_publish_cpu(struct cpu_info *cpu, uint64_t counter,
                              uint64_t next_counter, uint64_t tick_count) {
  uint32_t id = cpu->cpu_id;
  struct timer_cpu_buffer *buffer;
  struct os1_time_cpu_v1 *record;
  uint64_t sequence;

  if (id >= MAX_CPUS)
    panic("TIME: invalid CPU id %u", id);
  buffer = &timer_cpu_buffers[id];
  sequence = __atomic_load_n(&buffer->publish_sequence, __ATOMIC_RELAXED) + 1U;
  record = &buffer->slots[sequence & 1U];
  record->cpu_id = id;
  record->flags = OS1_TIME_CPU_ARMED | OS1_TIME_CPU_SYNCHRONIZED;
  if (__atomic_load_n(&cpu->online, __ATOMIC_ACQUIRE) != 0U)
    record->flags |= OS1_TIME_CPU_ONLINE;
  record->sequence = sequence;
  record->counter = counter;
  record->monotonic_ns = time_counter_to_ns(counter);
  record->next_deadline_ns = time_counter_to_ns(next_counter);
  record->tick_count = tick_count;
  record->late_tick_count = timer_cpu_late[id];
  __atomic_store_n(&buffer->publish_sequence, sequence, __ATOMIC_RELEASE);
}

void OS1libkernel_timer_reschedule_local(void) {
  if (!timer_uses_deadline_event())
    return;
  struct cpu_info *cpu = get_cpu_info();
  if (cpu == NULL)
    return;
  uint64_t now = OS1libkernel_time_counter_read();
  uint64_t deadline_ns = timer_current_deadline_ns(cpu);
  uint64_t target = timer_target_for_deadline(now, deadline_ns);
  if (target == cpu->next_tick_target)
    return;
  cpu->next_tick_target = target;
  timer_program_target(cpu, target);
}

uint64_t OS1libkernel_timer_percpu_arm(void) {
  struct cpu_info *cpu = get_cpu_info();
  uint64_t interval;
  uint64_t now;

  if (cpu == NULL)
    panic("%s", "TIME: per-CPU arm without CPU state");
  if (__atomic_load_n(&time_ready, __ATOMIC_ACQUIRE) == 0U)
    panic("%s", "TIME: per-CPU arm before global clock initialization");

  cpu->tick_error_acc = 0U;
  interval = timer_interval_counts(cpu);
  now = OS1libkernel_time_counter_read();
  cpu->next_tick_target = timer_checked_add(now, interval);
  timer_program_target(cpu, cpu->next_tick_target);
  timer_publish_cpu(cpu, now, cpu->next_tick_target, cpu->tick_count);
  return cpu->next_tick_target;
}

static void timer_stats_note_clockevent_lateness(struct cpu_info *cpu,
                                                 uint64_t now) {
  uint64_t target = cpu->next_tick_target;
  if (target == 0U || now <= target)
    return;
  uint64_t lateness_ns = time_scale_counts(now - target, time_frequency_hz);
  struct OS1libkernel_timer_stats *stats = &timer_cpu_stats[cpu->cpu_id];
  if (stats->clockevent_late_count != UINT64_MAX)
    ++stats->clockevent_late_count;
  if (UINT64_MAX - stats->clockevent_lateness_ns_total < lateness_ns)
    stats->clockevent_lateness_ns_total = UINT64_MAX;
  else
    stats->clockevent_lateness_ns_total += lateness_ns;
  if (lateness_ns > stats->max_clockevent_lateness_ns)
    stats->max_clockevent_lateness_ns = lateness_ns;
}

uint64_t OS1libkernel_timer_percpu_tick(void) {
  struct cpu_info *cpu = get_cpu_info();
  uint64_t now;

  if (cpu == NULL)
    panic("%s", "TIME: timer IRQ without CPU state");
  now = OS1libkernel_time_counter_read();
  if (timer_uses_deadline_event()) {
    uint64_t deadline_ns = timer_current_deadline_ns(cpu);
    cpu->next_tick_target = timer_target_for_deadline(now, deadline_ns);
  } else {
    uint64_t interval = timer_interval_counts(cpu);
    cpu->next_tick_target =
        timer_checked_add(cpu->next_tick_target, interval);
    if (cpu->next_tick_target <= now)
      cpu->next_tick_target = timer_checked_add(now, interval);
  }
  timer_program_target(cpu, cpu->next_tick_target);
  timer_publish_cpu(cpu, now, cpu->next_tick_target, cpu->tick_count);
  return cpu->next_tick_target;
}

static int timer_read_cpu(uint32_t cpu_id, struct os1_time_cpu_v1 *out) {
  struct timer_cpu_buffer *buffer = &timer_cpu_buffers[cpu_id];

  for (uint32_t attempt = 0U; attempt < TIMER_SNAPSHOT_RETRIES; ++attempt) {
    uint64_t before =
        __atomic_load_n(&buffer->publish_sequence, __ATOMIC_ACQUIRE);
    if (before == 0U)
      return 0;
    *out = buffer->slots[before & 1U];
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (before ==
        __atomic_load_n(&buffer->publish_sequence, __ATOMIC_ACQUIRE))
      return 1;
  }
  return -1;
}

int OS1libkernel_time_snapshot(struct os1_time_snapshot_v1 *out) {
  uint32_t count = 0U;

  if (out == NULL)
    return -1;
  memset(out, 0, sizeof(*out));
  out->abi_version = OS1_TIME_ABI_VERSION;
  out->struct_size = (uint32_t)sizeof(*out);
  out->cpu_count = 0U;
  out->cpu_record_size = (uint32_t)sizeof(struct os1_time_cpu_v1);
  out->sequence =
      __atomic_add_fetch(&time_snapshot_sequence, 1U, __ATOMIC_RELAXED);
  out->monotonic_ns = OS1libkernel_time_monotonic_ns();
  out->counter_frequency_hz = OS1libkernel_time_counter_frequency();
  out->scheduler_tick_hz = OS1LIBKERNEL_TIMER_HZ;

  for (uint32_t cpu_id = 0U; cpu_id < MAX_CPUS; ++cpu_id) {
    struct os1_time_cpu_v1 record;
    int status = timer_read_cpu(cpu_id, &record);
    if (status < 0)
      return -1;
    if (status == 0)
      continue;
    out->cpu[count] = record;
    ++count;
  }
  out->cpu_count = count;
  return 0;
}

static uint64_t timer_find_next_locked(struct cpu_info *cpu) {
  struct list_head *node;
  uint64_t earliest = 0U;
  list_for_each(node, &cpu->timer_list) {
    struct OS1libkernel_timer *timer =
        list_entry(node, struct OS1libkernel_timer, list);
    if (earliest == 0U || timer->deadline_ns < earliest)
      earliest = timer->deadline_ns;
  }
  return earliest;
}

static void timer_stats_note_lateness(struct OS1libkernel_timer_stats *stats,
                                      uint64_t deadline_ns,
                                      uint64_t now_ns) {
  if (now_ns <= deadline_ns)
    return;
  uint64_t lateness = now_ns - deadline_ns;
  if (stats->late_fire_count != UINT64_MAX)
    ++stats->late_fire_count;
  if (UINT64_MAX - stats->fire_lateness_ns_total < lateness)
    stats->fire_lateness_ns_total = UINT64_MAX;
  else
    stats->fire_lateness_ns_total += lateness;
  if (lateness > stats->max_fire_lateness_ns)
    stats->max_fire_lateness_ns = lateness;
}

static void timer_fire_due(struct cpu_info *cpu, uint64_t now_ns) {
  struct OS1libkernel_timer *timer;
  struct OS1libkernel_timer *next;
  struct OS1libkernel_timer_stats *stats = &timer_cpu_stats[cpu->cpu_id];
  uint32_t visited = 0U;
  uint32_t fired = 0U;
  uint64_t flags;
  uint64_t earliest = 0U;

  spin_lock_irqsave(&cpu->timer_lock, &flags);
  ++stats->scan_pass_count;
  if (cpu->timer_next_deadline_ns == 0U ||
      now_ns < cpu->timer_next_deadline_ns) {
    spin_unlock_irqrestore(&cpu->timer_lock, flags);
    return;
  }
  list_for_each_entry_safe(timer, next, &cpu->timer_list, list) {
    ++visited;
    if (now_ns < timer->deadline_ns) {
      if (earliest == 0U || timer->deadline_ns < earliest)
        earliest = timer->deadline_ns;
      continue;
    }
    list_del_init(&timer->list);
    __atomic_store_n(&timer->pending, false, __ATOMIC_RELEASE);
    if (stats->queue_depth != 0U)
      --stats->queue_depth;
    ++stats->fire_count;
    timer_stats_note_lateness(stats, timer->deadline_ns, now_ns);
    ++fired;
    if (timer->callback != NULL)
      timer->callback(timer->data);
    __atomic_store_n(&timer->cpu, -1, __ATOMIC_RELEASE);
  }
  cpu->timer_next_deadline_ns = earliest;
  stats->scan_count += visited;
  if (visited > stats->max_scan_per_pass)
    stats->max_scan_per_pass = visited;
  if (fired > stats->max_fire_per_pass)
    stats->max_fire_per_pass = fired;
  spin_unlock_irqrestore(&cpu->timer_lock, flags);
}

hal_frame_t *OS1libkernel_timer_tick(hal_frame_t *frame) {
  struct cpu_info *cpu;
  uint64_t now_counter;
  uint64_t now_ns;

  if (panic_flag != 0) {
    (void)nxhal_time_event_stop();
    hal_cpu_halt();
  }
  cpu = get_cpu_info();
  if (cpu == NULL)
    panic("%s", "TIME: scheduler tick without CPU state");
  now_counter = OS1libkernel_time_counter_read();
  timer_stats_note_clockevent_lateness(cpu, now_counter);
  if (cpu->next_tick_target != 0U && now_counter > cpu->next_tick_target &&
      timer_cpu_late[cpu->cpu_id] != UINT64_MAX)
    ++timer_cpu_late[cpu->cpu_id];
  ++cpu->tick_count;
  now_ns = time_counter_to_ns(now_counter);
  if (cpu->cpu_id == 0U) {
    uint64_t expected = now_ns / OS1LIBKERNEL_NSEC_PER_TICK;
    __atomic_store_n(&jiffies, expected, __ATOMIC_RELEASE);
  }
  timer_fire_due(cpu, now_ns);
  return OS1libkernel_sched_timer_tick(frame, now_ns);
}

void OS1libkernel_timer_setup(struct OS1libkernel_timer *timer,
                              OS1libkernel_timer_callback_t callback,
                              void *data) {
  if (timer == NULL || callback == NULL)
    panic("%s", "TIME: invalid software timer setup");
  INIT_LIST_HEAD(&timer->list);
  timer->deadline_ns = 0U;
  timer->callback = callback;
  timer->data = data;
  __atomic_store_n(&timer->pending, false, __ATOMIC_RELAXED);
  __atomic_store_n(&timer->cpu, -1, __ATOMIC_RELAXED);
}

void OS1libkernel_timer_cancel(struct OS1libkernel_timer *timer) {
  if (timer == NULL)
    panic("%s", "TIME: cancel of NULL software timer");

  for (uint32_t attempt = 0U; attempt < TIMER_CANCEL_RETRIES; ++attempt) {
    int cpu = __atomic_load_n(&timer->cpu, __ATOMIC_ACQUIRE);
    uint64_t flags;
    if (cpu < 0)
      return;
    if (cpu >= MAX_CPUS)
      panic("TIME: corrupt software timer owner %d", cpu);
    spin_lock_irqsave(&cpu_data[cpu].timer_lock, &flags);
    if (__atomic_load_n(&timer->cpu, __ATOMIC_ACQUIRE) != cpu) {
      spin_unlock_irqrestore(&cpu_data[cpu].timer_lock, flags);
      continue;
    }
    if (__atomic_load_n(&timer->pending, __ATOMIC_ACQUIRE)) {
      int was_earliest =
          timer->deadline_ns == cpu_data[cpu].timer_next_deadline_ns;
      list_del_init(&timer->list);
      __atomic_store_n(&timer->pending, false, __ATOMIC_RELEASE);
      struct OS1libkernel_timer_stats *stats = &timer_cpu_stats[cpu];
      if (stats->queue_depth != 0U)
        --stats->queue_depth;
      ++stats->cancel_count;
      if (was_earliest)
        cpu_data[cpu].timer_next_deadline_ns =
            timer_find_next_locked(&cpu_data[cpu]);
    }
    __atomic_store_n(&timer->cpu, -1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&cpu_data[cpu].timer_lock, flags);
    struct cpu_info *local = get_cpu_info();
    if (local != NULL && local->cpu_id == (uint32_t)cpu)
      OS1libkernel_timer_reschedule_local();
    return;
  }
  panic("%s", "TIME: software timer owner changed during bounded cancellation");
}

void OS1libkernel_timer_arm_deadline(struct OS1libkernel_timer *timer,
                                     uint64_t deadline_ns) {
  struct cpu_info *cpu;
  uint64_t flags;

  if (timer == NULL || deadline_ns == 0U)
    panic("%s", "TIME: invalid software timer deadline");
  OS1libkernel_timer_cancel(timer);
  cpu = get_cpu_info();
  if (cpu == NULL)
    panic("%s", "TIME: software timer arm without CPU state");
  spin_lock_irqsave(&cpu->timer_lock, &flags);
  timer->deadline_ns = deadline_ns;
  __atomic_store_n(&timer->cpu, (int)cpu->cpu_id, __ATOMIC_RELAXED);
  list_add_tail(&timer->list, &cpu->timer_list);
  if (cpu->timer_next_deadline_ns == 0U ||
      deadline_ns < cpu->timer_next_deadline_ns)
    cpu->timer_next_deadline_ns = deadline_ns;
  struct OS1libkernel_timer_stats *stats = &timer_cpu_stats[cpu->cpu_id];
  ++stats->arm_count;
  ++stats->queue_depth;
  if (stats->queue_depth > stats->queue_high_water)
    stats->queue_high_water = stats->queue_depth;
  __atomic_store_n(&timer->pending, true, __ATOMIC_RELEASE);
  spin_unlock_irqrestore(&cpu->timer_lock, flags);
  OS1libkernel_timer_reschedule_local();
}

bool OS1libkernel_timer_pending(const struct OS1libkernel_timer *timer) {
  if (timer == NULL)
    return false;
  return __atomic_load_n(&timer->pending, __ATOMIC_ACQUIRE);
}

int OS1libkernel_timer_stats_snapshot(
    uint32_t cpu_id, struct OS1libkernel_timer_stats *out) {
  if (out == NULL || cpu_id >= MAX_CPUS)
    return -1;
  uint64_t flags;
  spin_lock_irqsave(&cpu_data[cpu_id].timer_lock, &flags);
  *out = timer_cpu_stats[cpu_id];
  spin_unlock_irqrestore(&cpu_data[cpu_id].timer_lock, flags);
  return 0;
}

static void timer_export_queue_stats(
    struct os1_timer_cpu_stats_v4 *record,
    const struct OS1libkernel_timer_stats *stats) {
  record->arm_count = stats->arm_count;
  record->cancel_count = stats->cancel_count;
  record->fire_count = stats->fire_count;
  record->scan_count = stats->scan_count;
  record->scan_pass_count = stats->scan_pass_count;
  record->late_fire_count = stats->late_fire_count;
  record->fire_lateness_ns_total = stats->fire_lateness_ns_total;
  record->max_fire_lateness_ns = stats->max_fire_lateness_ns;
  record->clockevent_late_count = stats->clockevent_late_count;
  record->clockevent_lateness_ns_total = stats->clockevent_lateness_ns_total;
  record->max_clockevent_lateness_ns = stats->max_clockevent_lateness_ns;
  record->absolute_program_count = stats->absolute_program_count;
  record->queue_depth = stats->queue_depth;
  record->queue_high_water = stats->queue_high_water;
  record->max_scan_per_pass = stats->max_scan_per_pass;
  record->max_fire_per_pass = stats->max_fire_per_pass;
}

static int timer_export_irq_mask_stats(
    uint32_t cpu_id, uint64_t now_counter,
    struct os1_timer_cpu_stats_v4 *record) {
  struct nxhal_irq_mask_stats_v1 stats;
  int status = nxhal_irq_mask_stats_snapshot(cpu_id, &stats);

  if (status != NXHAL_IRQ_MASK_STATS_OK)
    return -1;
  record->irq_mask_interval_count = stats.interval_count;
  record->irq_mask_total_ns =
      time_scale_counts(stats.total_counts, time_frequency_hz);
  record->irq_mask_max_ns =
      time_scale_counts(stats.max_counts, time_frequency_hz);
  record->irq_mask_invalid_sample_count = stats.invalid_sample_count;
  record->irq_mask_flags = stats.flags;
  if ((stats.flags & NXHAL_IRQ_MASK_STATS_ACTIVE) != 0U &&
      now_counter >= stats.active_start_count)
    record->irq_mask_active_ns =
        time_scale_counts(now_counter - stats.active_start_count,
                          time_frequency_hz);
  return 0;
}

int OS1libkernel_timer_stats_export(
    struct os1_timer_stats_snapshot_v4 *out) {
  uint64_t now_counter;
  uint32_t count = 0U;

  if (out == NULL)
    return -1;
  memset(out, 0, sizeof(*out));
  now_counter = OS1libkernel_time_counter_read();
  out->abi_version = OS1_TIMER_STATS_ABI_VERSION;
  out->struct_size = (uint32_t)sizeof(*out);
  out->cpu_record_size = (uint32_t)sizeof(struct os1_timer_cpu_stats_v4);
  out->sequence =
      __atomic_add_fetch(&time_snapshot_sequence, 1U, __ATOMIC_RELAXED);
  out->monotonic_ns = time_counter_to_ns(now_counter);
  for (uint32_t cpu_id = 0U; cpu_id < MAX_CPUS; ++cpu_id) {
    struct os1_time_cpu_v1 time_record;
    struct OS1libkernel_timer_stats stats;
    int status = timer_read_cpu(cpu_id, &time_record);
    if (status < 0)
      return -1;
    if (status == 0)
      continue;
    if (OS1libkernel_timer_stats_snapshot(cpu_id, &stats) != 0)
      return -1;
    struct os1_timer_cpu_stats_v4 *record = &out->cpu[count];
    record->cpu_id = cpu_id;
    timer_export_queue_stats(record, &stats);
    if (timer_export_irq_mask_stats(cpu_id, now_counter, record) != 0)
      return -1;
    ++count;
  }
  out->cpu_count = count;
  return 0;
}
