#include <nxolmo/base/checked.h>
#include <stdint.h>
#include <limits.h>

static int check_sizes(void) {
  size_t output = 21U;
  if (nx_size_add(3U, 8U, &output) != NX_STATUS_OK || output != 11U)
    return 1;
  if (nx_size_add(SIZE_MAX, 1U, &output) != NX_STATUS_OVERFLOW ||
      output != 0U || nx_size_add(1U, 1U, NULL) != NX_STATUS_INVALID_ARGUMENT)
    return 2;
  if (nx_size_mul(6U, 7U, &output) != NX_STATUS_OK || output != 42U)
    return 3;
  if (nx_size_mul(SIZE_MAX, 2U, &output) != NX_STATUS_OVERFLOW ||
      output != 0U || nx_size_mul(1U, 1U, NULL) != NX_STATUS_INVALID_ARGUMENT)
    return 4;
  return 0;
}

static int check_u64(void) {
  uint64_t output = 21U;
  if (nx_u64_add(3U, 8U, &output) != NX_STATUS_OK || output != 11U)
    return 1;
  if (nx_u64_add(UINT64_MAX, 1U, &output) != NX_STATUS_OVERFLOW ||
      output != 0U || nx_u64_add(1U, 1U, NULL) != NX_STATUS_INVALID_ARGUMENT)
    return 2;
  if (nx_u64_mul(6U, 7U, &output) != NX_STATUS_OK || output != 42U)
    return 3;
  if (nx_u64_mul(UINT64_MAX, 2U, &output) != NX_STATUS_OVERFLOW ||
      output != 0U || nx_u64_mul(1U, 1U, NULL) != NX_STATUS_INVALID_ARGUMENT)
    return 4;
  return 0;
}

static int check_ranges(void) {
  uint64_t end = 45U;
  if (nx_range_validate(3U, 5U, 10U, &end) != NX_STATUS_OK || end != 8U)
    return 1;
  if (nx_range_validate(6U, 5U, 10U, &end) != NX_STATUS_LIMIT_EXCEEDED ||
      end != 0U)
    return 2;
  if (nx_range_validate(UINT64_MAX, 1U, UINT64_MAX, &end) != NX_STATUS_OVERFLOW ||
      end != 0U || nx_range_validate(0U, 1U, 3U, NULL) != NX_STATUS_INVALID_ARGUMENT)
    return 3;
  return 0;
}

static int check_spans(void) {
  uint8_t buffer[32] = {0};
  nx_const_span input = {buffer, sizeof(buffer)};
  nx_mut_span output = {buffer, sizeof(buffer)};
  nx_const_span invalid = {NULL, 1U};
  nx_mut_span bad_out = {NULL, 1U};
  if (nx_const_span_validate(input, 31U) != NX_STATUS_OK ||
      nx_const_span_validate(input, 33U) != NX_STATUS_TRUNCATED ||
      nx_const_span_validate(invalid, 1U) != NX_STATUS_INVALID_ARGUMENT)
    return 1;
  if (nx_mut_span_validate(output, 32U) != NX_STATUS_OK ||
      nx_mut_span_validate(output, 33U) != NX_STATUS_NO_SPACE ||
      nx_mut_span_validate(bad_out, 1U) != NX_STATUS_INVALID_ARGUMENT)
    return 2;
  if (nx_pointer_ranges_overlap(buffer, 10U, buffer + 5U, 10U) != 1 ||
      nx_pointer_ranges_overlap(buffer, 10U, buffer + 10U, 10U) != 0)
    return 3;
  if (nx_pointer_ranges_overlap(NULL, 5U, buffer, 10U) != -1 ||
      nx_pointer_ranges_overlap((void *)(uintptr_t)(UINTPTR_MAX-1U),
                                10U, buffer, 10U) != -1)
    return 4;
  return 0;
}

int main(void) {
  int result = check_sizes();
  if (result != 0) return result;
  result = check_u64();
  if (result != 0) return 10 + result;
  result = check_ranges();
  if (result != 0) return 20 + result;
  result = check_spans();
  if (result != 0) return 30 + result;
  return 0;
}
