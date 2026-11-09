/*
 * NX-CORE time engine.
 *
 * HAL owns raw counter/frequency/compare primitives. NX-CORE owns the clock
 * epoch, scheduler cadence, per-CPU time state and software deadlines.
 */
#include <hal/core.h>
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
static uint64_t time_boot_counter;
static uint64_t time_frequency_hz;
static uint64_t time_snapshot_sequence;
static uint32_t time_ready;

static uint64_t time_scale_counts(uint64_t counts, uint64_t frequency) {
  if (frequency == 0U)
    panic("%s", "TIME: zero hardware counter frequency");
  return (uint64_t)(((__uint128_t)counts * OS1LIBKERNEL_NSEC_PER_SEC) /
                    frequency);
}

static uint64_t time_counter_to_ns(uint64_t counter) {
  if (counter < time_boot_counter)
    return 0U;
  return time_scale_counts(counter - time_boot_counter, time_frequency_hz);
}

void OS1libkernel_time_init(void) {
  uint64_t frequency = hal_timer_get_freq();
  uint64_t counter = hal_timer_get_count();

  if (frequency == 0U)
    panic("%s", "TIME: HAL published zero counter frequency");
  time_boot_counter = counter;
  time_frequency_hz = frequency;
  __atomic_store_n(&jiffies, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&time_ready, 1U, __ATOMIC_RELEASE);
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
  uint64_t counter;

  if (__atomic_load_n(&time_ready, __ATOMIC_ACQUIRE) == 0U)
    panic("%s", "TIME: monotonic clock requested before initialization");
  counter = hal_timer_get_count();
  return time_counter_to_ns(counter);
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

uint64_t OS1libkernel_timer_percpu_arm(void) {
  struct cpu_info *cpu = get_cpu_info();
  uint64_t now;
  uint64_t interval;

  if (cpu == NULL)
    panic("%s", "TIME: per-CPU arm without CPU state");
  if (__atomic_load_n(&time_ready, __ATOMIC_ACQUIRE) == 0U)
    panic("%s", "TIME: per-CPU arm before global clock initialization");
  cpu->tick_error_acc = 0U;
  interval = timer_interval_counts(cpu);
  now = hal_timer_get_count();
  cpu->next_tick_target = now + interval;
  hal_timer_set_compare(cpu->next_tick_target);
  timer_publish_cpu(cpu, now, cpu->next_tick_target, cpu->tick_count);
  return cpu->next_tick_target;
}

uint64_t OS1libkernel_timer_percpu_tick(void) {
  struct cpu_info *cpu = get_cpu_info();
  uint64_t interval;
  uint64_t now;

  if (cpu == NULL)
    panic("%s", "TIME: timer IRQ without CPU state");
  interval = timer_interval_counts(cpu);
  now = hal_timer_get_count();
  cpu->next_tick_target += interval;
  if (cpu->next_tick_target <= now) {
    ++timer_cpu_late[cpu->cpu_id];
    cpu->next_tick_target = now + interval;
  }
  hal_timer_set_compare(cpu->next_tick_target);
  timer_publish_cpu(cpu, now, cpu->next_tick_target, cpu->tick_count + 1U);
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

static void timer_fire_due(struct cpu_info *cpu, uint64_t now_ns) {
  struct OS1libkernel_timer *timer;
  struct OS1libkernel_timer *next;
  uint64_t flags;

  spin_lock_irqsave(&cpu->timer_lock, &flags);
  list_for_each_entry_safe(timer, next, &cpu->timer_list, list) {
    if (now_ns < timer->deadline_ns)
      continue;
    list_del_init(&timer->list);
    __atomic_store_n(&timer->pending, false, __ATOMIC_RELEASE);
    if (timer->callback != NULL)
      timer->callback(timer->data);
    __atomic_store_n(&timer->cpu, -1, __ATOMIC_RELEASE);
  }
  spin_unlock_irqrestore(&cpu->timer_lock, flags);
}

hal_frame_t *OS1libkernel_timer_tick(hal_frame_t *frame) {
  struct cpu_info *cpu;
  uint64_t now_ns;

  if (panic_flag != 0) {
    hal_timer_control(0U);
    hal_cpu_halt();
  }
  cpu = get_cpu_info();
  if (cpu == NULL)
    panic("%s", "TIME: scheduler tick without CPU state");
  ++cpu->tick_count;
  now_ns = OS1libkernel_time_monotonic_ns();
  if (cpu->cpu_id == 0U) {
    uint64_t expected = now_ns / OS1LIBKERNEL_NSEC_PER_TICK;
    __atomic_store_n(&jiffies, expected, __ATOMIC_RELEASE);
  }
  timer_fire_due(cpu, now_ns);
  return schedule(frame);
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
      list_del_init(&timer->list);
      __atomic_store_n(&timer->pending, false, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&timer->cpu, -1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&cpu_data[cpu].timer_lock, flags);
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
  __atomic_store_n(&timer->pending, true, __ATOMIC_RELEASE);
  spin_unlock_irqrestore(&cpu->timer_lock, flags);
}

bool OS1libkernel_timer_pending(const struct OS1libkernel_timer *timer) {
  if (timer == NULL)
    return false;
  return __atomic_load_n(&timer->pending, __ATOMIC_ACQUIRE);
}
