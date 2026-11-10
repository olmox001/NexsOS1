/*
 * Real Core->HAL NXPAX contract tests. Link the actual production modules.
 * Each assertion validates the captured event fields, not call occurrences.
 */
#include <kernel/nxpax.h>
#include <hal/nxpax.h>
#include <stdint.h>
#include <stdio.h>

static int32_t test_cpu_id = 3;

int32_t hal_cpu_id_fault_safe(void) {
  return test_cpu_id;
}

static int test_user_disposition(void) {
  struct nxhal_nxpax_event event = {0};
  nxpax_kernel_record(17, NXHAL_NXPAX_KIND_USER_TERMINATED,
                      UINT64_C(0x1234), UINT64_C(0x5678),
                      UINT64_C(0xaabb), NXHAL_NXPAX_FLAG_USER);
  if (nxhal_nxpax_read(1U, &event) != 1)
    return 1;
  if (event.sequence != 1U || event.pid != 17 ||
      event.cpu_id != 3 ||
      event.kind != NXHAL_NXPAX_KIND_USER_TERMINATED ||
      event.flags != NXHAL_NXPAX_FLAG_USER)
    return 2;
  if (event.fault_pc != UINT64_C(0x1234) ||
      event.fault_address != UINT64_C(0x5678) ||
      event.syndrome != UINT64_C(0xaabb))
    return 3;
  return 0;
}

static int test_fatal_disposition(void) {
  struct nxhal_nxpax_event event = {0};
  test_cpu_id = 8;
  nxpax_kernel_record(-1, NXHAL_NXPAX_KIND_KERNEL_FATAL,
                      UINT64_C(0xdead), 0U, UINT64_C(0x99), 0U);
  if (nxhal_nxpax_read(2U, &event) != 1)
    return 1;
  if (event.sequence != 2U || event.pid != -1 ||
      event.cpu_id != 8 || event.kind != NXHAL_NXPAX_KIND_KERNEL_FATAL ||
      event.flags != 0U)
    return 2;
  if (event.fault_pc != UINT64_C(0xdead) ||
      event.fault_address != 0U || event.syndrome != UINT64_C(0x99))
    return 3;
  if (nxhal_nxpax_dropped() != 0U || nxhal_nxpax_latest() != 2U)
    return 4;
  return 0;
}

int main(void) {
  int status = test_user_disposition();
  if (status != 0) {
    fprintf(stderr, "NXPAX user disposition failed: %d\n", status);
    return status;
  }
  status = test_fatal_disposition();
  if (status != 0) {
    fprintf(stderr, "NXPAX fatal disposition failed: %d\n", status);
    return status;
  }
  puts("PASS: Core NXPAX user/fatal disposition through real HAL ring");
  return 0;
}
