#define main hcd_core_profile_original_main
#include "hcd_core_profile_test.c"
#undef main

#include "common/memory_plan.h"
#include "common/hcd_endian.h"

static int build_memory_view(
    uint8_t *buffer,
    struct hcd_validation_workspace *workspace,
    struct hcd_validation_error *detail,
    struct hcd_validated_view *view)
{
    uint64_t usable_offset = CORE_MEMORY_OFFSET +
        (uint64_t)sizeof(struct hcd_memory_record);

    if (build_core_fixture(buffer) != 0) {
        return 1;
    }
    put_u32(buffer, usable_offset + 16U, HCD_MEMORY_USABLE);
    put_u32(buffer, usable_offset + 20U,
            HCD_MEMORY_A_READ | HCD_MEMORY_A_WRITE);
    put_u32(buffer, usable_offset + 24U, 1U);
    put_u32(buffer, usable_offset + 28U, HCD_ID_UNKNOWN);
    put_u64(buffer, CORE_BOOT_OFFSET + 24U, UINT64_C(0x8000));
    put_u64(buffer, CORE_BOOT_OFFSET + 32U, UINT64_C(0x108000));
    put_u64(buffer, CORE_BOOT_OFFSET + 40U, UINT64_C(0x108000));
    put_u64(buffer, CORE_BOOT_OFFSET + 48U, UINT64_C(0x8000));
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, CORE_TOTAL_SIZE, HCD_PROFILE_CORE,
                     workspace, detail, view) != HCD_OK) {
        return 3;
    }
    return 0;
}

static struct preboot_memory_request make_request(uint32_t request_id)
{
    struct preboot_memory_request request;

    request.request_id = request_id;
    request.resource_kind = PREBOOT_MEM_RESOURCE_KERNEL_IMAGE;
    request.owner_id = PREBOOT_MEM_OWNER_PREBOOT;
    request.required_attributes = HCD_MEMORY_A_READ;
    request.forbidden_attributes = HCD_MEMORY_A_EXECUTE;
    request.numa_node_id = 1U;
    request.flags = 0U;
    request.size = UINT64_C(0x2000);
    request.alignment = UINT64_C(0x1000);
    request.min_address = UINT64_C(0x100000);
    request.max_address = UINT64_C(0x300000);
    request.boundary = 0U;
    return request;
}

static int test_memory_plan_initializes_usable_extent(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    struct preboot_memory_request request = make_request(1U);
    struct preboot_memory_allocation allocation = {0};

    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 2;
    }
    if ((plan.state != PREBOOT_MEM_READY) || (plan.extent_count != 1U)) {
        return 3;
    }
    if (preboot_memory_allocate(&plan, &request, &allocation) != PREBOOT_MEM_OK) {
        return 4;
    }
    if ((allocation.base != UINT64_C(0x200000)) ||
        (allocation.size != request.size) ||
        (allocation.numa_node_id != 1U) ||
        (allocation.owner_id != PREBOOT_MEM_OWNER_PREBOOT)) {
        return 5;
    }
    return 0;
}

static int test_memory_plan_splits_and_preserves_order(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    struct preboot_memory_request first = make_request(1U);
    struct preboot_memory_request second = make_request(2U);
    struct preboot_memory_allocation allocation = {0};

    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 2;
    }
    if (preboot_memory_allocate(&plan, &first, &allocation) != PREBOOT_MEM_OK) {
        return 3;
    }
    second.size = UINT64_C(0x3000);
    if (preboot_memory_allocate(&plan, &second, &allocation) != PREBOOT_MEM_OK) {
        return 4;
    }
    if (allocation.base != UINT64_C(0x202000)) {
        return 5;
    }
    if ((plan.extent_count != 1U) ||
        (plan.extents[0].base != UINT64_C(0x205000)) ||
        (plan.extents[0].length != UINT64_C(0xB000))) {
        return 6;
    }
    return 0;
}

static int test_memory_plan_requires_explicit_numa_fallback(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    struct preboot_memory_request request = make_request(1U);
    struct preboot_memory_allocation allocation = {0};

    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 2;
    }
    request.numa_node_id = 7U;
    if (preboot_memory_allocate(&plan, &request, &allocation) !=
        PREBOOT_MEM_E_NO_MEMORY) {
        return 3;
    }
    request.flags = PREBOOT_MEM_F_ALLOW_NUMA_FALLBACK;
    if (preboot_memory_allocate(&plan, &request, &allocation) != PREBOOT_MEM_OK) {
        return 4;
    }
    if (allocation.base != UINT64_C(0x200000)) {
        return 5;
    }
    return 0;
}

static int test_memory_plan_rejects_boundary_crossing(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    struct preboot_memory_request request = make_request(1U);
    struct preboot_memory_allocation allocation = {0};

    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 2;
    }
    request.min_address = UINT64_C(0x20F000);
    request.max_address = UINT64_C(0x220000);
    request.boundary = UINT64_C(0x10000);
    if (preboot_memory_allocate(&plan, &request, &allocation) !=
        PREBOOT_MEM_E_NO_MEMORY) {
        return 3;
    }
    return 0;
}

static int test_memory_plan_rejects_bad_request_and_duplicate(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    struct preboot_memory_request request = make_request(1U);
    struct preboot_memory_allocation allocation = {0};

    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 2;
    }
    request.alignment = 3U;
    if (preboot_memory_allocate(&plan, &request, &allocation) !=
        PREBOOT_MEM_E_BAD_REQUEST) {
        return 3;
    }
    request.alignment = UINT64_C(0x1000);
    if (preboot_memory_allocate(&plan, &request, &allocation) != PREBOOT_MEM_OK) {
        return 4;
    }
    if (preboot_memory_allocate(&plan, &request, &allocation) !=
        PREBOOT_MEM_E_DUPLICATE_ID) {
        return 5;
    }
    return 0;
}

static int test_memory_plan_rejects_attribute_mismatch(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    struct preboot_memory_request request = make_request(1U);
    struct preboot_memory_allocation allocation = {0};

    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 2;
    }
    request.forbidden_attributes = HCD_MEMORY_A_WRITE;
    if (preboot_memory_allocate(&plan, &request, &allocation) !=
        PREBOOT_MEM_E_NO_MEMORY) {
        return 3;
    }
    return 0;
}


static int build_uefi_memory_view(
    uint8_t *buffer,
    struct hcd_validation_workspace *workspace,
    struct hcd_validation_error *detail,
    struct hcd_validated_view *view,
    uint32_t header_flags)
{
    if (build_memory_view(buffer, workspace, detail, view) != 0) {
        return 1;
    }
    put_u32(buffer, CORE_BOOT_OFFSET, HCD_BOOT_UEFI_APPLICATION);
    put_u32(buffer, CORE_BOOT_OFFSET + 4U, HCD_BOOT_MODE_UEFI);
    put_u32(buffer, CORE_BOOT_OFFSET + 8U, HCD_ENTRY_AMD64_LONG64);
    header_flags |= hcd_load_le32(buffer, 32U) &
        (HCD_F_HETEROGENEOUS_CPUS | HCD_F_EXTERNAL_REFERENCES |
         HCD_F_SECURE_BOOT_REPORTED);
    put_u32(buffer, 32U, header_flags);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    return 0;
}

static int test_memory_plan_rejects_active_uefi_boot_services(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};

    if (build_uefi_memory_view(buffer, &workspace, &detail, &view,
            HCD_F_FIRMWARE_BOOT_SERVICES_ACTIVE) != 0) {
        return 1;
    }
    if (hcd_validate(buffer, CORE_TOTAL_SIZE, HCD_PROFILE_CORE,
                     &workspace, &detail, &view) == HCD_OK) {
        return 2;
    }
    return 0;
}

static int test_memory_plan_accepts_uefi_after_exit_boot_services(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;

    if (build_uefi_memory_view(buffer, &workspace, &detail, &view,
            HCD_F_EFI_EXIT_BOOT_SERVICES_DONE) != 0) {
        return 1;
    }
    if (hcd_validate(buffer, CORE_TOTAL_SIZE, HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_OK) {
        return 2;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 2;
    }
    return 0;
}


static int test_memory_plan_never_allocates_unaccepted_memory(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    uint64_t usable_offset = CORE_MEMORY_OFFSET +
        (uint64_t)sizeof(struct hcd_memory_record);

    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    put_u32(buffer, usable_offset + 16U, HCD_MEMORY_UNACCEPTED);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, CORE_TOTAL_SIZE, HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_OK) {
        return 3;
    }
    if (preboot_memory_plan_init(&plan, &view) !=
        PREBOOT_MEM_E_NO_USABLE_MEMORY) {
        return 4;
    }
    return 0;
}

static int test_memory_plan_seal_is_one_way(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    struct preboot_memory_request first = make_request(1U);
    struct preboot_memory_request second = make_request(2U);
    struct preboot_memory_allocation allocation = {0};
    struct preboot_memory_allocation rejected = {
        UINT32_C(0xA5A5A5A5), UINT32_C(0x5A5A5A5A),
        UINT64_C(0x1122334455667788), UINT64_C(0x8877665544332211),
        UINT32_C(0xA5A5A5A5), UINT32_C(0x5A5A5A5A),
        UINT32_C(0xA5A5A5A5), UINT32_C(0x5A5A5A5A)
    };
    struct preboot_memory_allocation rejected_before = rejected;
    uint32_t allocation_count = 0U;
    uint32_t initial_extent_count = 0U;

    if (preboot_memory_plan_seal(NULL) != PREBOOT_MEM_E_NULL_INPUT) {
        return 1;
    }
    if (preboot_memory_plan_seal(&plan) != PREBOOT_MEM_E_BAD_STATE) {
        return 2;
    }
    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 3;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 4;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_E_BAD_STATE) {
        return 5;
    }
    initial_extent_count = plan.extent_count;
    if (initial_extent_count == 0U) {
        return 6;
    }
    plan.extent_count = 0U;
    if (preboot_memory_plan_seal(&plan) != PREBOOT_MEM_E_BAD_STATE) {
        return 7;
    }
    plan.extent_count = initial_extent_count;
    if (preboot_memory_allocate(&plan, &first, &allocation) != PREBOOT_MEM_OK) {
        return 8;
    }
    allocation_count = plan.allocation_count;
    if (preboot_memory_plan_seal(&plan) != PREBOOT_MEM_OK) {
        return 9;
    }
    if (plan.state != PREBOOT_MEM_SEALED) {
        return 10;
    }
    if (preboot_memory_allocate(&plan, &second, &rejected) !=
        PREBOOT_MEM_E_BAD_STATE) {
        return 11;
    }
    if ((plan.allocation_count != allocation_count) ||
        (rejected.request_id != rejected_before.request_id) ||
        (rejected.base != rejected_before.base) ||
        (rejected.size != rejected_before.size)) {
        return 12;
    }
    if (preboot_memory_plan_seal(&plan) != PREBOOT_MEM_E_BAD_STATE) {
        return 13;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_E_BAD_STATE) {
        return 14;
    }
    return 0;
}

static int test_memory_plan_seals_when_all_extents_are_allocated(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    struct preboot_memory_request request = make_request(1U);
    struct preboot_memory_request rejected_request = make_request(2U);
    struct preboot_memory_allocation allocation = {0};

    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 2;
    }
    request.size = UINT64_C(0x10000);
    request.min_address = UINT64_C(0x200000);
    request.max_address = UINT64_C(0x300000);
    if (preboot_memory_allocate(&plan, &request, &allocation) != PREBOOT_MEM_OK) {
        return 3;
    }
    if ((plan.extent_count != 0U) ||
        (plan.allocation_count != 1U) ||
        (allocation.base != UINT64_C(0x200000)) ||
        (allocation.size != request.size)) {
        return 4;
    }
    if (preboot_memory_plan_seal(&plan) != PREBOOT_MEM_OK) {
        return 5;
    }
    if (plan.state != PREBOOT_MEM_SEALED) {
        return 6;
    }
    if (preboot_memory_allocate(&plan, &rejected_request, &allocation) !=
        PREBOOT_MEM_E_BAD_STATE) {
        return 7;
    }
    return 0;
}

static int test_memory_plan_failed_initialization_is_retryable(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    uint64_t usable_offset = CORE_MEMORY_OFFSET +
        (uint64_t)sizeof(struct hcd_memory_record);

    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    put_u32(buffer, usable_offset + 16U, HCD_MEMORY_UNACCEPTED);
    if (seal_fixture(buffer) != 0) {
        return 2;
    }
    if (hcd_validate(buffer, CORE_TOTAL_SIZE, HCD_PROFILE_CORE,
                     &workspace, &detail, &view) != HCD_OK) {
        return 3;
    }
    if (preboot_memory_plan_init(&plan, &view) !=
        PREBOOT_MEM_E_NO_USABLE_MEMORY) {
        return 4;
    }
    if ((plan.magic != 0U) || (plan.state != PREBOOT_MEM_UNINITIALIZED) ||
        (plan.extent_count != 0U) || (plan.allocation_count != 0U)) {
        return 5;
    }
    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 6;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 7;
    }
    return 0;
}

static int test_memory_plan_rejects_corrupt_counts(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    struct preboot_memory_request request = make_request(1U);
    struct preboot_memory_allocation allocation = {
        UINT32_C(0xA5A5A5A5), UINT32_C(0x5A5A5A5A),
        UINT64_C(0x1122334455667788), UINT64_C(0x8877665544332211),
        UINT32_C(0xA5A5A5A5), UINT32_C(0x5A5A5A5A),
        UINT32_C(0xA5A5A5A5), UINT32_C(0x5A5A5A5A)
    };
    struct preboot_memory_allocation before = allocation;

    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 2;
    }
    plan.extent_count = PREBOOT_MAX_MEMORY_EXTENTS + 1U;
    if (preboot_memory_allocate(&plan, &request, &allocation) !=
        PREBOOT_MEM_E_BAD_STATE) {
        return 3;
    }
    plan.extent_count = 1U;
    plan.allocation_count = PREBOOT_MAX_ALLOCATION_REQUESTS + 1U;
    if (preboot_memory_allocate(&plan, &request, &allocation) !=
        PREBOOT_MEM_E_BAD_STATE) {
        return 4;
    }
    if ((allocation.request_id != before.request_id) ||
        (allocation.base != before.base) ||
        (allocation.size != before.size)) {
        return 5;
    }
    return 0;
}

static int test_memory_plan_rejects_workspace_input_overlap(void)
{
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    uint8_t *buffer = (uint8_t *)&plan;
    uint8_t original[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    size_t index = 0U;

    _Static_assert(CORE_TOTAL_SIZE <= sizeof(struct preboot_memory_plan),
                   "HCD fixture must fit in the planner workspace");
    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    for (index = 0U; index < sizeof(original); ++index) {
        original[index] = buffer[index];
    }
    if (preboot_memory_plan_init(&plan, &view) !=
        PREBOOT_MEM_E_BUFFER_OVERLAP) {
        return 2;
    }
    for (index = 0U; index < sizeof(original); ++index) {
        if (buffer[index] != original[index]) {
            return 3;
        }
    }
    return 0;
}

static int test_memory_plan_rejects_output_aliasing_workspace(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    struct preboot_memory_request request = make_request(1U);
    struct preboot_memory_extent first_extent = {0};
    struct preboot_memory_allocation *allocation = NULL;

    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 2;
    }
    first_extent = plan.extents[0];
    allocation = &plan.allocations[0];
    if (preboot_memory_allocate(&plan, &request, allocation) !=
        PREBOOT_MEM_E_BUFFER_OVERLAP) {
        return 3;
    }
    if ((plan.extent_count != 1U) || (plan.allocation_count != 0U) ||
        (plan.extents[0].base != first_extent.base) ||
        (plan.extents[0].length != first_extent.length)) {
        return 4;
    }
    return 0;
}

static int test_memory_plan_rejects_request_output_overlap(void)
{
    uint8_t buffer[CORE_TOTAL_SIZE];
    struct hcd_validation_workspace workspace = {0};
    struct hcd_validation_error detail = {0};
    struct hcd_validated_view view = {0};
    struct preboot_memory_plan plan = PREBOOT_MEMORY_PLAN_INITIALIZER;
    union {
        struct preboot_memory_request request;
        struct preboot_memory_allocation allocation;
    } io;
    uint8_t original[sizeof(io)];
    size_t index = 0U;

    if (build_memory_view(buffer, &workspace, &detail, &view) != 0) {
        return 1;
    }
    if (preboot_memory_plan_init(&plan, &view) != PREBOOT_MEM_OK) {
        return 2;
    }
    io.request = make_request(1U);
    for (index = 0U; index < sizeof(io); ++index) {
        original[index] = ((const uint8_t *)&io)[index];
    }
    if (preboot_memory_allocate(&plan, &io.request, &io.allocation) !=
        PREBOOT_MEM_E_BUFFER_OVERLAP) {
        return 3;
    }
    for (index = 0U; index < sizeof(io); ++index) {
        if (((const uint8_t *)&io)[index] != original[index]) {
            return 4;
        }
    }
    if ((plan.extent_count != 1U) || (plan.allocation_count != 0U)) {
        return 5;
    }
    return 0;
}

int main(void)
{
    if (hcd_core_profile_original_main() != 0) {
        return 1;
    }
    if (test_memory_plan_initializes_usable_extent() != 0) {
        return 2;
    }
    if (test_memory_plan_splits_and_preserves_order() != 0) {
        return 3;
    }
    if (test_memory_plan_requires_explicit_numa_fallback() != 0) {
        return 4;
    }
    if (test_memory_plan_rejects_boundary_crossing() != 0) {
        return 5;
    }
    if (test_memory_plan_rejects_bad_request_and_duplicate() != 0) {
        return 6;
    }
    if (test_memory_plan_rejects_attribute_mismatch() != 0) {
        return 7;
    }
    if (test_memory_plan_rejects_active_uefi_boot_services() != 0) {
        return 8;
    }
    if (test_memory_plan_accepts_uefi_after_exit_boot_services() != 0) {
        return 9;
    }
    if (test_memory_plan_never_allocates_unaccepted_memory() != 0) {
        return 10;
    }
    if (test_memory_plan_seal_is_one_way() != 0) {
        return 11;
    }
    if (test_memory_plan_failed_initialization_is_retryable() != 0) {
        return 12;
    }
    if (test_memory_plan_rejects_corrupt_counts() != 0) {
        return 13;
    }
    if (test_memory_plan_seals_when_all_extents_are_allocated() != 0) {
        return 14;
    }
    if (test_memory_plan_rejects_workspace_input_overlap() != 0) {
        return 15;
    }
    if (test_memory_plan_rejects_output_aliasing_workspace() != 0) {
        return 16;
    }
    if (test_memory_plan_rejects_request_output_overlap() != 0) {
        return 17;
    }
    return 0;
}
