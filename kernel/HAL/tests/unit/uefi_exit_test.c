#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "capability/uefi_exit.h"

#define TEST_BUFFER_CAPACITY ((size_t)4096U)
#define TEST_MAP_SIZE ((size_t)80U)
#define TEST_DESCRIPTOR_SIZE ((size_t)40U)

struct fixture {
    uint8_t initial_buffer[TEST_BUFFER_CAPACITY];
    uint8_t grown_buffer[TEST_BUFFER_CAPACITY];
    uint32_t get_calls;
    uint32_t allocate_calls;
    uint32_t exit_calls;
    uint32_t event_count;
    char events[16];
    enum preboot_uefi_status exit_status[PREBOOT_UEFI_EXIT_MAX_ATTEMPTS];
    uint32_t always_too_small;
    uint32_t invalid_map;
    uint32_t allocation_failure;
    size_t reported_size;
};

static void record_event(struct fixture *fixture, char event)
{
    assert(fixture->event_count < (uint32_t)sizeof(fixture->events));
    fixture->events[fixture->event_count] = event;
    ++fixture->event_count;
}

static enum preboot_uefi_status get_memory_map(
    void *context,
    void *buffer,
    size_t *buffer_size,
    uint64_t *map_key,
    size_t *descriptor_size,
    uint32_t *descriptor_version)
{
    struct fixture *fixture = context;
    ++fixture->get_calls;
    record_event(fixture, 'G');
    *descriptor_size = TEST_DESCRIPTOR_SIZE;
    *descriptor_version = PREBOOT_UEFI_MEMORY_DESCRIPTOR_V1;
    if (fixture->always_too_small != 0U) {
        if (fixture->reported_size <= *buffer_size) {
            fixture->reported_size = *buffer_size + 1U;
        }
        *buffer_size = fixture->reported_size;
        return PREBOOT_UEFI_STATUS_BUFFER_TOO_SMALL;
    }
    if (*buffer_size < TEST_MAP_SIZE) {
        *buffer_size = TEST_MAP_SIZE;
        return PREBOOT_UEFI_STATUS_BUFFER_TOO_SMALL;
    }
    if (buffer == (void *)0) {
        return PREBOOT_UEFI_STATUS_FAILURE;
    }
    *buffer_size = fixture->invalid_map != 0U ? 79U : TEST_MAP_SIZE;
    *map_key = UINT64_C(100) + (uint64_t)fixture->get_calls;
    return PREBOOT_UEFI_STATUS_SUCCESS;
}

static enum preboot_uefi_status allocate_map_buffer(
    void *context,
    size_t minimum_capacity,
    void **buffer,
    size_t *capacity)
{
    struct fixture *fixture = context;
    ++fixture->allocate_calls;
    record_event(fixture, 'A');
    if (fixture->allocation_failure != 0U) {
        return PREBOOT_UEFI_STATUS_FAILURE;
    }
    if (minimum_capacity > sizeof(fixture->grown_buffer)) {
        return PREBOOT_UEFI_STATUS_FAILURE;
    }
    *buffer = fixture->grown_buffer;
    *capacity = fixture->always_too_small != 0U ?
        minimum_capacity : sizeof(fixture->grown_buffer);
    return PREBOOT_UEFI_STATUS_SUCCESS;
}

static enum preboot_uefi_status exit_boot_services(
    void *context,
    uintptr_t image_handle,
    uint64_t map_key)
{
    struct fixture *fixture = context;
    uint32_t call = fixture->exit_calls;
    ++fixture->exit_calls;
    record_event(fixture, 'X');
    assert(image_handle == (uintptr_t)0x1234U);
    assert(map_key == (UINT64_C(100) + (uint64_t)fixture->get_calls));
    assert(call < PREBOOT_UEFI_EXIT_MAX_ATTEMPTS);
    return fixture->exit_status[call];
}

static const struct preboot_uefi_exit_services SERVICES = {
    get_memory_map,
    allocate_map_buffer,
    exit_boot_services
};

static struct fixture make_fixture(void)
{
    struct fixture fixture = {0};
    uint32_t index;
    for (index = 0U; index < PREBOOT_UEFI_EXIT_MAX_ATTEMPTS; ++index) {
        fixture.exit_status[index] = PREBOOT_UEFI_STATUS_SUCCESS;
    }
    return fixture;
}

static struct preboot_uefi_exit_map make_result(void)
{
    struct preboot_uefi_exit_map result = {
        (void *)(uintptr_t)1U, 1U, 1U, 1U, 1U, 1U, 1U
    };
    return result;
}

static void test_success_without_growth(void)
{
    struct fixture fixture = make_fixture();
    struct preboot_uefi_exit_map result = make_result();
    enum preboot_uefi_exit_error error = preboot_uefi_exit_boot_services(
        &SERVICES, &fixture, (uintptr_t)0x1234U,
        fixture.initial_buffer, sizeof(fixture.initial_buffer), &result);
    assert(error == PREBOOT_UEFI_EXIT_OK);
    assert(result.buffer == fixture.initial_buffer);
    assert(result.size == TEST_MAP_SIZE);
    assert(result.map_key == UINT64_C(101));
    assert(result.exit_attempts == 1U);
    assert(result.buffer_growths == 0U);
    assert(fixture.event_count == 2U);
    assert(fixture.events[0] == 'G' && fixture.events[1] == 'X');
}

static void test_buffer_growth(void)
{
    struct fixture fixture = make_fixture();
    struct preboot_uefi_exit_map result = make_result();
    enum preboot_uefi_exit_error error = preboot_uefi_exit_boot_services(
        &SERVICES, &fixture, (uintptr_t)0x1234U,
        fixture.initial_buffer, 0U, &result);
    assert(error == PREBOOT_UEFI_EXIT_OK);
    assert(result.buffer == fixture.grown_buffer);
    assert(result.buffer_growths == 1U);
    assert(fixture.allocate_calls == 1U);
    assert(fixture.event_count == 4U);
    assert(fixture.events[0] == 'G' && fixture.events[1] == 'A' &&
           fixture.events[2] == 'G' && fixture.events[3] == 'X');
}

static void test_stale_map_key_retry(void)
{
    struct fixture fixture = make_fixture();
    struct preboot_uefi_exit_map result = make_result();
    fixture.exit_status[0] = PREBOOT_UEFI_STATUS_INVALID_PARAMETER;
    enum preboot_uefi_exit_error error = preboot_uefi_exit_boot_services(
        &SERVICES, &fixture, (uintptr_t)0x1234U,
        fixture.initial_buffer, sizeof(fixture.initial_buffer), &result);
    assert(error == PREBOOT_UEFI_EXIT_OK);
    assert(result.map_key == UINT64_C(102));
    assert(result.exit_attempts == 2U);
    assert(fixture.event_count == 4U);
    assert(fixture.events[0] == 'G' && fixture.events[1] == 'X' &&
           fixture.events[2] == 'G' && fixture.events[3] == 'X');
}

static void test_retry_budget_and_failure_output(void)
{
    struct fixture fixture = make_fixture();
    struct preboot_uefi_exit_map result = make_result();
    uint32_t index;
    for (index = 0U; index < PREBOOT_UEFI_EXIT_MAX_ATTEMPTS; ++index) {
        fixture.exit_status[index] = PREBOOT_UEFI_STATUS_INVALID_PARAMETER;
    }
    enum preboot_uefi_exit_error error = preboot_uefi_exit_boot_services(
        &SERVICES, &fixture, (uintptr_t)0x1234U,
        fixture.initial_buffer, sizeof(fixture.initial_buffer), &result);
    assert(error == PREBOOT_UEFI_EXIT_E_RETRIES_EXHAUSTED);
    assert(fixture.exit_calls == PREBOOT_UEFI_EXIT_MAX_ATTEMPTS);
    assert(result.buffer == (void *)0);
    assert(result.size == 0U);
    assert(result.exit_attempts == 0U);
}

static void test_invalid_map_and_growth_limit(void)
{
    struct fixture fixture = make_fixture();
    struct preboot_uefi_exit_map result = make_result();
    enum preboot_uefi_exit_error error;
    fixture.always_too_small = 1U;
    error = preboot_uefi_exit_boot_services(
        &SERVICES, &fixture, (uintptr_t)0x1234U,
        fixture.initial_buffer, 0U, &result);
    assert(error == PREBOOT_UEFI_EXIT_E_MAP_CAPACITY);
    assert(fixture.exit_calls == 0U);
    assert(fixture.allocate_calls == PREBOOT_UEFI_MAP_MAX_GROWTHS);
    assert(result.buffer == (void *)0);

    fixture = make_fixture();
    result = make_result();
    fixture.invalid_map = 1U;
    error = preboot_uefi_exit_boot_services(
        &SERVICES, &fixture, (uintptr_t)0x1234U,
        fixture.initial_buffer, sizeof(fixture.initial_buffer), &result);
    assert(error == PREBOOT_UEFI_EXIT_E_BAD_MAP);
    assert(fixture.exit_calls == 0U);
    assert(result.buffer == (void *)0);

    fixture = make_fixture();
    result = make_result();
    fixture.allocation_failure = 1U;
    error = preboot_uefi_exit_boot_services(
        &SERVICES, &fixture, (uintptr_t)0x1234U,
        fixture.initial_buffer, 0U, &result);
    assert(error == PREBOOT_UEFI_EXIT_E_ALLOC_FAILED);
    assert(fixture.exit_calls == 0U);
    assert(result.buffer == (void *)0);
}

int main(void)
{
    test_success_without_growth();
    test_buffer_growth();
    test_stale_map_key_retry();
    test_retry_budget_and_failure_output();
    test_invalid_map_and_growth_limit();
    return 0;
}
