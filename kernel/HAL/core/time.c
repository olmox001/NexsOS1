#include <hal/provider.h>
#include <hal/time.h>

#include <stddef.h>
#include <stdint.h>

#define NXHAL_CLOCKSOURCE_FLAGS   (NXHAL_CLOCKSOURCE_CONTINUOUS | NXHAL_CLOCKSOURCE_CALIBRATED |    NXHAL_CLOCKSOURCE_INVARIANT | NXHAL_CLOCKSOURCE_MUST_VERIFY |    NXHAL_CLOCKSOURCE_GLOBAL)

#define NXHAL_CLOCKEVENT_FLAGS   (NXHAL_CLOCKEVENT_PER_CPU | NXHAL_CLOCKEVENT_PERIODIC |    NXHAL_CLOCKEVENT_ONESHOT | NXHAL_CLOCKEVENT_ABSOLUTE)

static int nxhal_clocksource_valid(const struct nxhal_clocksource_v1 *info) {
  uint32_t stability;

  if (info->abi_version != NXHAL_TIME_ABI_VERSION ||
      info->struct_size != sizeof(*info) || info->source_id == 0U ||
      info->reserved0 != 0U || info->frequency_hz == 0U ||
      info->counter_bits == 0U || info->counter_bits > 64U ||
      (info->flags & ~NXHAL_CLOCKSOURCE_FLAGS) != 0U ||
      (info->flags & NXHAL_CLOCKSOURCE_CONTINUOUS) == 0U ||
      (info->flags & NXHAL_CLOCKSOURCE_CALIBRATED) == 0U)
    return NXHAL_TIME_E_INVALID;

  stability = info->flags &
              (NXHAL_CLOCKSOURCE_INVARIANT | NXHAL_CLOCKSOURCE_MUST_VERIFY);
  if (stability == 0U ||
      stability ==
          (NXHAL_CLOCKSOURCE_INVARIANT | NXHAL_CLOCKSOURCE_MUST_VERIFY))
    return NXHAL_TIME_E_INVALID;
  if (info->counter_bits == 64U && info->mask != UINT64_MAX)
    return NXHAL_TIME_E_INVALID;
  if (info->max_delta_counts == 0U ||
      info->max_delta_counts > info->mask)
    return NXHAL_TIME_E_INVALID;
  return NXHAL_TIME_OK;
}

static int nxhal_clockevent_valid(const struct nxhal_clockevent_v1 *info) {
  uint32_t mode;

  if (info->abi_version != NXHAL_TIME_ABI_VERSION ||
      info->struct_size != sizeof(*info) || info->event_id == 0U ||
      info->reserved0 != 0U || info->frequency_hz == 0U ||
      (info->flags & ~NXHAL_CLOCKEVENT_FLAGS) != 0U ||
      info->min_delta_counts == 0U ||
      info->max_delta_counts < info->min_delta_counts)
    return NXHAL_TIME_E_INVALID;

  mode = info->flags & (NXHAL_CLOCKEVENT_PERIODIC | NXHAL_CLOCKEVENT_ONESHOT);
  if (mode == 0U)
    return NXHAL_TIME_E_INVALID;
  if ((info->flags & NXHAL_CLOCKEVENT_ABSOLUTE) != 0U &&
      (info->flags & NXHAL_CLOCKEVENT_ONESHOT) == 0U)
    return NXHAL_TIME_E_INVALID;
  return NXHAL_TIME_OK;
}

int nxhal_time_clocksource_info(struct nxhal_clocksource_v1 *out) {
  int status;

  if (out == NULL)
    return NXHAL_TIME_E_NULL;
  status = nxhal_provider_clocksource_info(out);
  if (status != NXHAL_TIME_OK)
    return status;
  return nxhal_clocksource_valid(out);
}

int nxhal_time_clockevent_info(struct nxhal_clockevent_v1 *out) {
  int status;

  if (out == NULL)
    return NXHAL_TIME_E_NULL;
  status = nxhal_provider_clockevent_info(out);
  if (status != NXHAL_TIME_OK)
    return status;
  return nxhal_clockevent_valid(out);
}

int nxhal_time_counter_read(uint64_t *out_count) {
  if (out_count == NULL)
    return NXHAL_TIME_E_NULL;
  return nxhal_provider_clocksource_read(out_count);
}

int nxhal_time_event_percpu_init(uint64_t frequency_hz) {
  if (frequency_hz == 0U)
    return NXHAL_TIME_E_INVALID;
  return nxhal_provider_clockevent_percpu_init(frequency_hz);
}

int nxhal_time_event_program_absolute(uint64_t count) {
  if (count == 0U)
    return NXHAL_TIME_E_INVALID;
  return nxhal_provider_clockevent_program_absolute(count);
}

int nxhal_time_event_program_periodic(uint64_t frequency_hz) {
  if (frequency_hz == 0U)
    return NXHAL_TIME_E_INVALID;
  return nxhal_provider_clockevent_program_periodic(frequency_hz);
}

int nxhal_time_event_stop(void) {
  return nxhal_provider_clockevent_stop();
}
