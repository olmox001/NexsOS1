#include "common/hcd_crc64.h"

static int test_standard_vector(void)
{
    static const uint8_t input[] = "123456789";
    uint64_t checksum = 0U;

    if (hcd_crc64_ecma(input, 9U, &checksum) != HCD_OK) {
        return 1;
    }
    if (checksum != UINT64_C(0x6C40DF5F0B497347)) {
        return 2;
    }
    return 0;
}

static int test_empty_input(void)
{
    static const uint8_t input[1] = {0U};
    uint64_t checksum = UINT64_MAX;

    if (hcd_crc64_ecma(input, 0U, &checksum) != HCD_OK) {
        return 1;
    }
    if (checksum != 0U) {
        return 2;
    }
    return 0;
}

static int test_hcd_checksum_field_is_zeroed(void)
{
    uint8_t buffer[HCD_HEADER_SIZE];
    uint64_t checksum = 0U;
    uint64_t index = 0U;

    for (index = 0U; index < HCD_HEADER_SIZE; ++index) {
        buffer[index] = (uint8_t)index;
    }
    for (index = 24U; index < 32U; ++index) {
        buffer[index] = UINT8_C(0xAA);
    }
    if (hcd_crc64_hcd(buffer, sizeof(buffer), &checksum) != HCD_OK) {
        return 1;
    }
    if (checksum != UINT64_C(0xC29F5C4AC075B014)) {
        return 2;
    }
    return 0;
}

static int test_invalid_arguments(void)
{
    uint64_t checksum = 0U;
    static const uint8_t input[1] = {0U};

    if (hcd_crc64_ecma(NULL, 1U, &checksum) != HCD_E_NULL_INPUT) {
        return 1;
    }
    if (hcd_crc64_ecma(input, HCD_MAX_TOTAL_SIZE + 1U, &checksum) !=
        HCD_E_BAD_TOTAL_SIZE) {
        return 2;
    }
    if (hcd_crc64_hcd(input, 1U, &checksum) != HCD_E_BAD_TOTAL_SIZE) {
        return 3;
    }
    return 0;
}

int main(void)
{
    if (test_standard_vector() != 0) {
        return 1;
    }
    if (test_empty_input() != 0) {
        return 2;
    }
    if (test_hcd_checksum_field_is_zeroed() != 0) {
        return 3;
    }
    if (test_invalid_arguments() != 0) {
        return 4;
    }
    return 0;
}
