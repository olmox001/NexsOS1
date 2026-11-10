#include <hal/nxpax.h>
#include <stdint.h>
#include <stddef.h>

int main(void) {
  struct nxhal_nxpax_event event = {0};
  if (nxhal_nxpax_latest() != 0U || nxhal_nxpax_dropped() != 0U)
    return 1;
  if (nxhal_nxpax_read(0U, &event) != -1 ||
      nxhal_nxpax_read(1U, NULL) != -1)
    return 2;
  if (nxhal_nxpax_read(1U, &event) != 0)
    return 3;
  nxhal_nxpax_fault(UINT64_C(0x1234), UINT64_C(0x5678),
                    UINT64_C(0x9abc), 1, 5);
  if (nxhal_nxpax_read(1U, &event) != 1 ||
      event.sequence != 1U ||
      event.fault_pc != UINT64_C(0x1234) ||
      event.fault_address != UINT64_C(0x5678) ||
      event.syndrome != UINT64_C(0x9abc) ||
      event.kind != NXHAL_NXPAX_KIND_HARDWARE ||
      event.flags != NXHAL_NXPAX_FLAG_USER || event.cpu_id != 5)
    return 4;
  const struct nxhal_nxpax_event decision = {
      .fault_pc = UINT64_C(0x2000), .pid = 42,
      .kind = NXHAL_NXPAX_KIND_USER_TERMINATED,
      .flags = NXHAL_NXPAX_FLAG_UACCESS, .cpu_id = 9};
  nxhal_nxpax_note(&decision);
  if (nxhal_nxpax_read(2U, &event) != 1 || event.pid != 42 ||
      event.kind != NXHAL_NXPAX_KIND_USER_TERMINATED ||
      event.flags != NXHAL_NXPAX_FLAG_UACCESS || event.cpu_id != 9)
    return 5;
  for (uint32_t i = 0U; i < NXHAL_NXPAX_RING_CAPACITY; ++i)
    nxhal_nxpax_fault(i, UINT64_C(0x8000) + i, i + 9U, 0, 7);
  if (nxhal_nxpax_read(1U, &event) != 0 ||
      nxhal_nxpax_latest() != (uint64_t)NXHAL_NXPAX_RING_CAPACITY + 2U)
    return 6;
  if (nxhal_nxpax_read(nxhal_nxpax_latest(), &event) != 1 ||
      event.fault_pc != NXHAL_NXPAX_RING_CAPACITY - 1U ||
      event.flags != 0U || event.cpu_id != 7)
    return 7;
  if (nxhal_nxpax_dropped() != 0U)
    return 8;
  if (nxhal_nxpax_diagnostic(NXHAL_NXPAX_KIND_DIAGNOSTIC_WARNING,
                             "[C0] [WARN] recoverable", 0) != 0)
    return 9;
  if (nxhal_nxpax_read(nxhal_nxpax_latest(), &event) != 1 ||
      event.kind != NXHAL_NXPAX_KIND_DIAGNOSTIC_WARNING ||
      event.cpu_id != 0 || event.pid != -1 ||
      event.syndrome == 0U || event.fault_address != 0U)
    return 10;
  uint64_t fingerprint = event.syndrome;
  if (nxhal_nxpax_diagnostic(NXHAL_NXPAX_KIND_DIAGNOSTIC_ERROR,
                             "[C0] [WARN] recoverable", 1) != 0)
    return 11;
  if (nxhal_nxpax_read(nxhal_nxpax_latest(), &event) != 1 ||
      event.kind != NXHAL_NXPAX_KIND_DIAGNOSTIC_ERROR ||
      event.cpu_id != 1 || event.syndrome != fingerprint)
    return 12;
  if (nxhal_nxpax_diagnostic(NXHAL_NXPAX_KIND_HARDWARE, "invalid", 0) != -1 ||
      nxhal_nxpax_diagnostic(NXHAL_NXPAX_KIND_DIAGNOSTIC_WARNING, NULL, 0) != -1)
    return 13;
  if (nxhal_nxpax_dropped() != 0U)
    return 14;
  return 0;
}
