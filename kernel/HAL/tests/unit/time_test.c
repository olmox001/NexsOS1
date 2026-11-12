#include <hal/time.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int source_status;
static int event_status;
static int read_status;
static int init_status;
static int absolute_status;
static int periodic_status;
static int stop_status;
static uint64_t mock_count;
static uint64_t last_value;

static struct nxhal_clocksource_v1 source_invariant(void) {
  struct nxhal_clocksource_v1 source = {
      .abi_version = NXHAL_TIME_ABI_VERSION,
      .struct_size = sizeof(source),
      .source_id = NXHAL_CLOCKSOURCE_ID_ARM64_VIRTUAL,
      .flags = NXHAL_CLOCKSOURCE_CONTINUOUS |
               NXHAL_CLOCKSOURCE_CALIBRATED |
               NXHAL_CLOCKSOURCE_INVARIANT |
               NXHAL_CLOCKSOURCE_GLOBAL,
      .counter_bits = 64U,
      .reserved0 = 0U,
      .frequency_hz = UINT64_C(62500000),
      .mask = UINT64_MAX,
      .max_delta_counts = INT64_MAX,
  };
  return source;
}

static struct nxhal_clockevent_v1 event_periodic(void) {
  struct nxhal_clockevent_v1 event = {
      .abi_version = NXHAL_TIME_ABI_VERSION,
      .struct_size = sizeof(event),
      .event_id = NXHAL_CLOCKEVENT_ID_AMD64_LAPIC,
      .flags = NXHAL_CLOCKEVENT_PER_CPU | NXHAL_CLOCKEVENT_PERIODIC,
      .irq = 32U,
      .reserved0 = 0U,
      .frequency_hz = UINT64_C(62500000),
      .min_delta_counts = 1U,
      .max_delta_counts = UINT32_MAX,
  };
  return event;
}

static struct nxhal_clocksource_v1 mock_source;
static struct nxhal_clockevent_v1 mock_event;

int nxhal_provider_clocksource_info(struct nxhal_clocksource_v1 *out) {
  if (source_status != NXHAL_TIME_OK)
    return source_status;
  *out = mock_source;
  return NXHAL_TIME_OK;
}

int nxhal_provider_clocksource_read(uint64_t *out_count) {
  if (read_status != NXHAL_TIME_OK)
    return read_status;
  *out_count = mock_count;
  return NXHAL_TIME_OK;
}

int nxhal_provider_clockevent_info(struct nxhal_clockevent_v1 *out) {
  if (event_status != NXHAL_TIME_OK)
    return event_status;
  *out = mock_event;
  return NXHAL_TIME_OK;
}

int nxhal_provider_clockevent_percpu_init(uint64_t frequency_hz) {
  last_value = frequency_hz;
  return init_status;
}

int nxhal_provider_clockevent_program_absolute(uint64_t count) {
  last_value = count;
  return absolute_status;
}

int nxhal_provider_clockevent_program_periodic(uint64_t frequency_hz) {
  last_value = frequency_hz;
  return periodic_status;
}

int nxhal_provider_clockevent_stop(void) {
  return stop_status;
}

static void reset_mocks(void) {
  source_status = NXHAL_TIME_OK;
  event_status = NXHAL_TIME_OK;
  read_status = NXHAL_TIME_OK;
  init_status = NXHAL_TIME_OK;
  absolute_status = NXHAL_TIME_OK;
  periodic_status = NXHAL_TIME_OK;
  stop_status = NXHAL_TIME_OK;
  mock_count = UINT64_C(0x12345678);
  last_value = 0U;
  mock_source = source_invariant();
  mock_event = event_periodic();
}

static int test_contract_layout(void) {
  if (sizeof(struct nxhal_clocksource_v1) != 48U)
    return 1;
  if (offsetof(struct nxhal_clocksource_v1, frequency_hz) != 24U)
    return 2;
  if (offsetof(struct nxhal_clocksource_v1, max_delta_counts) != 40U)
    return 3;
  if (sizeof(struct nxhal_clockevent_v1) != 48U)
    return 4;
  if (offsetof(struct nxhal_clockevent_v1, frequency_hz) != 24U)
    return 5;
  if (offsetof(struct nxhal_clockevent_v1, max_delta_counts) != 40U)
    return 6;
  return 0;
}

static int test_clocksource_contract(void) {
  struct nxhal_clocksource_v1 out;

  reset_mocks();
  if (nxhal_time_clocksource_info(&out) != NXHAL_TIME_OK)
    return 1;
  mock_source.flags &= ~NXHAL_CLOCKSOURCE_INVARIANT;
  mock_source.flags |= NXHAL_CLOCKSOURCE_MUST_VERIFY;
  if (nxhal_time_clocksource_info(&out) != NXHAL_TIME_OK)
    return 2;
  mock_source.flags |= NXHAL_CLOCKSOURCE_INVARIANT;
  if (nxhal_time_clocksource_info(&out) != NXHAL_TIME_E_INVALID)
    return 3;
  mock_source = source_invariant();
  mock_source.frequency_hz = 0U;
  if (nxhal_time_clocksource_info(&out) != NXHAL_TIME_E_INVALID)
    return 4;
  return nxhal_time_clocksource_info(NULL) == NXHAL_TIME_E_NULL ? 0 : 5;
}

static int test_clockevent_contract(void) {
  struct nxhal_clockevent_v1 out;

  reset_mocks();
  if (nxhal_time_clockevent_info(&out) != NXHAL_TIME_OK)
    return 1;
  mock_event.flags = NXHAL_CLOCKEVENT_PER_CPU |
                     NXHAL_CLOCKEVENT_ONESHOT |
                     NXHAL_CLOCKEVENT_ABSOLUTE;
  if (nxhal_time_clockevent_info(&out) != NXHAL_TIME_OK)
    return 2;
  mock_event.flags = NXHAL_CLOCKEVENT_ABSOLUTE;
  if (nxhal_time_clockevent_info(&out) != NXHAL_TIME_E_INVALID)
    return 3;
  mock_event = event_periodic();
  mock_event.min_delta_counts = mock_event.max_delta_counts + 1U;
  if (nxhal_time_clockevent_info(&out) != NXHAL_TIME_E_INVALID)
    return 4;
  return nxhal_time_clockevent_info(NULL) == NXHAL_TIME_E_NULL ? 0 : 5;
}

static int test_counter_and_errors(void) {
  uint64_t count = 0U;

  reset_mocks();
  if (nxhal_time_counter_read(&count) != NXHAL_TIME_OK ||
      count != mock_count)
    return 1;
  read_status = NXHAL_TIME_E_NOT_READY;
  if (nxhal_time_counter_read(&count) != NXHAL_TIME_E_NOT_READY)
    return 2;
  source_status = NXHAL_TIME_E_UNSTABLE;
  if (nxhal_time_clocksource_info(&mock_source) != NXHAL_TIME_E_UNSTABLE)
    return 3;
  return nxhal_time_counter_read(NULL) == NXHAL_TIME_E_NULL ? 0 : 4;
}

static int test_event_operations(void) {
  reset_mocks();
  if (nxhal_time_event_percpu_init(1000U) != NXHAL_TIME_OK ||
      last_value != 1000U)
    return 1;
  if (nxhal_time_event_percpu_init(0U) != NXHAL_TIME_E_INVALID)
    return 2;
  if (nxhal_time_event_program_absolute(UINT64_C(42)) != NXHAL_TIME_OK ||
      last_value != UINT64_C(42))
    return 3;
  if (nxhal_time_event_program_absolute(0U) != NXHAL_TIME_E_INVALID)
    return 4;
  if (nxhal_time_event_program_periodic(250U) != NXHAL_TIME_OK ||
      last_value != 250U)
    return 5;
  if (nxhal_time_event_program_periodic(0U) != NXHAL_TIME_E_INVALID)
    return 6;
  if (nxhal_time_event_stop() != NXHAL_TIME_OK)
    return 7;
  stop_status = NXHAL_TIME_E_UNSUPPORTED;
  return nxhal_time_event_stop() == NXHAL_TIME_E_UNSUPPORTED ? 0 : 8;
}

int main(void) {
  if (test_contract_layout() != 0)
    return 1;
  if (test_clocksource_contract() != 0)
    return 2;
  if (test_clockevent_contract() != 0)
    return 3;
  if (test_counter_and_errors() != 0)
    return 4;
  if (test_event_operations() != 0)
    return 5;
  return 0;
}
