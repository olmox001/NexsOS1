/*
 * AArch64 EL1 virtual clocksource/clockevent provider.
 *
 * CNTVCT_EL0 is the free-running clocksource. CNTV_CVAL_EL0 + CNTV_CTL_EL0
 * form a per-CPU absolute one-shot clockevent. Scheduler cadence belongs to
 * NX-CORE; this provider only exposes hardware capabilities and programs the
 * event requested through the stable NX-HAL time ABI.
 */
#include <arch/arch.h>
#include <arch/timer.h>
#include <hal/diagnostic_protocol.h>
#include <hal/irq.h>
#include <hal/provider.h>
#include <hal/time.h>
#include <hal/timer_protocol.h>

#include <stddef.h>
#include <stdint.h>

static uint64_t timer_freq;

void nxhal_arm64_timer_global_init(void) {
  timer_freq = arch_impl_timer_get_freq();
  if (timer_freq == 0U) {
    nxhal_diag_error("%s", "Timer: CNTFRQ_EL0 reported zero\n");
    return;
  }

  hal_irq_line_enable(NXHAL_ARM64_VIRTUAL_TIMER_IRQ);
  nxhal_diag_info("Timer: ARM virtual clocksource %lu Hz\n", timer_freq);
}

int nxhal_provider_clocksource_info(struct nxhal_clocksource_v1 *out) {
  if (out == NULL)
    return NXHAL_TIME_E_NULL;
  if (timer_freq == 0U)
    return NXHAL_TIME_E_NOT_READY;

  *out = (struct nxhal_clocksource_v1){
      .abi_version = NXHAL_TIME_ABI_VERSION,
      .struct_size = sizeof(*out),
      .source_id = NXHAL_CLOCKSOURCE_ID_ARM64_VIRTUAL,
      .flags = NXHAL_CLOCKSOURCE_CONTINUOUS |
               NXHAL_CLOCKSOURCE_CALIBRATED |
               NXHAL_CLOCKSOURCE_INVARIANT |
               NXHAL_CLOCKSOURCE_GLOBAL,
      .counter_bits = 64U,
      .frequency_hz = timer_freq,
      .mask = UINT64_MAX,
      .max_delta_counts = INT64_MAX,
  };
  return NXHAL_TIME_OK;
}

int nxhal_provider_clocksource_read(uint64_t *out_count) {
  if (out_count == NULL)
    return NXHAL_TIME_E_NULL;
  if (timer_freq == 0U)
    return NXHAL_TIME_E_NOT_READY;

  *out_count = arch_impl_timer_get_count();
  return NXHAL_TIME_OK;
}

int nxhal_provider_clockevent_info(struct nxhal_clockevent_v1 *out) {
  if (out == NULL)
    return NXHAL_TIME_E_NULL;
  if (timer_freq == 0U)
    return NXHAL_TIME_E_NOT_READY;

  *out = (struct nxhal_clockevent_v1){
      .abi_version = NXHAL_TIME_ABI_VERSION,
      .struct_size = sizeof(*out),
      .event_id = NXHAL_CLOCKEVENT_ID_ARM64_VIRTUAL,
      .flags = NXHAL_CLOCKEVENT_PER_CPU | NXHAL_CLOCKEVENT_ONESHOT |
               NXHAL_CLOCKEVENT_ABSOLUTE,
      .irq = NXHAL_ARM64_VIRTUAL_TIMER_IRQ,
      .frequency_hz = timer_freq,
      .min_delta_counts = 1U,
      .max_delta_counts = INT64_MAX,
  };
  return NXHAL_TIME_OK;
}

int nxhal_provider_clockevent_program_absolute(uint64_t count) {
  if (count == 0U || timer_freq == 0U)
    return NXHAL_TIME_E_INVALID;

  arch_impl_timer_set_compare(count);
  arch_impl_timer_control(UINT32_C(1));
  return NXHAL_TIME_OK;
}

int nxhal_provider_clockevent_program_periodic(uint64_t frequency_hz) {
  (void)frequency_hz;
  return NXHAL_TIME_E_UNSUPPORTED;
}

int nxhal_provider_clockevent_stop(void) {
  arch_impl_timer_control(UINT32_C(0));
  return NXHAL_TIME_OK;
}

int nxhal_provider_clockevent_percpu_init(uint64_t frequency_hz) {
  uint64_t next_tick_target;

  if (frequency_hz == 0U || timer_freq == 0U)
    return NXHAL_TIME_E_NOT_READY;

  next_tick_target = nxhal_timer_arm_current();
  arch_impl_timer_control(UINT32_C(1));
  hal_irq_line_enable(NXHAL_ARM64_VIRTUAL_TIMER_IRQ);
  nxhal_diag_info("Timer: CPU event armed at 0x%lx\n", next_tick_target);
  return NXHAL_TIME_OK;
}
