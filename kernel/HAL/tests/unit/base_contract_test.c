#include <nxolmo/base/result.h>
#include <nxolmo/base/status.h>
#include <nxolmo/base/types.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int check_identity(void) {
  nx_object_ref a = {{7U, 9U}, 2U};
  nx_object_ref b = a;
  if (!nx_object_ref_is_valid(a) ||
      !nx_object_ref_equal(a, b) ||
      !nx_object_id_equal(a.id, b.id))
    return 1;
  b.generation = 3U;
  if (nx_object_ref_equal(a, b) || !nx_object_id_equal(a.id, b.id))
    return 2;
  a.id.high = 0U;
  a.id.low = 0U;
  if (nx_object_ref_is_valid(a) || !nx_object_id_is_zero(a.id))
    return 3;
  a.id.low = 11U;
  a.generation = 0U;
  if (nx_object_ref_is_valid(a) || nx_object_id_is_zero(a.id))
    return 4;
  return 0;
}

static int check_result_roundtrip(void) {
  nx_result r;
  if (nx_result_init(&r, NX_STATUS_BUSY, NX_RESULT_DOMAIN_SECURITY,
                     42U, 3U, 8U, NX_CYCLES_UNKNOWN, 4U) != NX_STATUS_OK)
    return 1;
  if (nx_result_validate(&r) != NX_STATUS_OK ||
      r.status != NX_STATUS_BUSY || r.operation_id != 42U ||
      r.completed != 3U || r.detail != 8U || r.fuel_used != 4U ||
      r.flags != NX_RESULT_FLAG_CYCLES_UNKNOWN)
    return 2;
  if (nx_result_init(&r, NX_STATUS_OK, NX_RESULT_DOMAIN_RUNTIME,
                     43U, 1U, 0U, 300U, 3U) != NX_STATUS_OK ||
      nx_result_validate(&r) != NX_STATUS_OK ||
      r.cycles_used != 300U || r.flags != 0U)
    return 3;
  return 0;
}

static int check_corrupt_result(void) {
  nx_result r;
  if (nx_result_init(&r, NX_STATUS_OK, NX_RESULT_DOMAIN_BASE,
                     55U, 0U, 0U, NX_CYCLES_UNKNOWN, 0U) != NX_STATUS_OK)
    return 1;
  r.schema_version++;
  if (nx_result_validate(&r) != NX_STATUS_VERSION_MISMATCH)
    return 2;
  r.schema_version--;
  r.flags |= UINT16_C(0x8000);
  if (nx_result_validate(&r) != NX_STATUS_MALFORMED)
    return 3;
  r.flags = 0U;
  if (nx_result_validate(&r) != NX_STATUS_MALFORMED)
    return 4;
  r.flags = NX_RESULT_FLAG_CYCLES_UNKNOWN;
  r.domain = 99U;
  if (nx_result_validate(&r) != NX_STATUS_MALFORMED)
    return 5;
  return 0;
}

static int check_null_and_status(void) {
  nx_result r;
  if (nx_result_init(NULL, NX_STATUS_OK, NX_RESULT_DOMAIN_BASE,
                     1U, 0U, 0U, 0U, 0U) != NX_STATUS_INVALID_ARGUMENT ||
      nx_result_validate(NULL) != NX_STATUS_INVALID_ARGUMENT)
    return 1;
  if (nx_status_name(NX_STATUS_OK)[0] != 'O' ||
      nx_status_name(NX_STATUS_OVERFLOW)[0] != 'O' ||
      nx_status_name(UINT32_MAX)[0] != 'U')
    return 2;
  if (sizeof(nx_object_ref) != 24U || sizeof(nx_digest256) != 32U ||
      sizeof(r) != 56U)
    return 3;
  if (memset(&r, 0, sizeof(r)) != &r)
    return 4;
  return 0;
}

int main(void) {
  int status = check_identity();
  if (status != 0) return status;
  status = check_result_roundtrip();
  if (status != 0) return 10 + status;
  status = check_corrupt_result();
  if (status != 0) return 20 + status;
  status = check_null_and_status();
  if (status != 0) return 30 + status;
  return 0;
}
