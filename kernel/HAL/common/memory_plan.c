#include <stddef.h>
#include <stdint.h>

#include "memory_plan.h"
#include "hcd_parser.h"
#include "hcd_wire.h"

#include "hcd_endian.h"
#include "hcd_assert.h"


static void preboot_memory_plan_reset_failed(
    struct preboot_memory_plan *plan)
{
    plan->extent_count = 0U;
    plan->allocation_count = 0U;
    plan->state = PREBOOT_MEM_UNINITIALIZED;
    plan->magic = 0U;
}

static int preboot_memory_span(
    const void *buffer,
    size_t size,
    uintptr_t *start,
    uintptr_t *end)
{
    uintptr_t span_start = 0U;

    if ((buffer == NULL) || (size == 0U) || (start == NULL) || (end == NULL)) {
        return 0;
    }
    span_start = (uintptr_t)buffer;
    if ((uintmax_t)size > (uintmax_t)(UINTPTR_MAX - span_start)) {
        return 0;
    }
    *start = span_start;
    *end = span_start + (uintptr_t)size;
    return 1;
}

static enum preboot_memory_error preboot_memory_check_disjoint(
    const void *left,
    size_t left_size,
    const void *right,
    size_t right_size)
{
    uintptr_t left_start = 0U;
    uintptr_t left_end = 0U;
    uintptr_t right_start = 0U;
    uintptr_t right_end = 0U;

    if (!preboot_memory_span(left, left_size, &left_start, &left_end) ||
        !preboot_memory_span(right, right_size, &right_start, &right_end)) {
        return PREBOOT_MEM_E_BAD_BUFFER_RANGE;
    }
    if ((left_start < right_end) && (right_start < left_end)) {
        return PREBOOT_MEM_E_BUFFER_OVERLAP;
    }
    return PREBOOT_MEM_OK;
}

static void preboot_memory_copy_extent(
    struct preboot_memory_extent *destination,
    const struct preboot_memory_extent *source)
{
    destination->base = source->base;
    destination->length = source->length;
    destination->attributes = source->attributes;
    destination->numa_node_id = source->numa_node_id;
    destination->owner = source->owner;
    destination->reserved0 = source->reserved0;
}

static void preboot_memory_copy_allocation(
    struct preboot_memory_allocation *destination,
    const struct preboot_memory_allocation *source)
{
    destination->request_id = source->request_id;
    destination->resource_kind = source->resource_kind;
    destination->base = source->base;
    destination->size = source->size;
    destination->attributes = source->attributes;
    destination->numa_node_id = source->numa_node_id;
    destination->owner_id = source->owner_id;
    destination->source_owner_id = source->source_owner_id;
}

static int preboot_memory_power_of_two(uint64_t value)
{
    return (value != 0U) && ((value & (value - 1U)) == 0U);
}

static int preboot_memory_align_up(
    uint64_t value,
    uint64_t alignment,
    uint64_t *aligned)
{
    if (value > (UINT64_MAX - (alignment - 1U))) {
        return 0;
    }
    *aligned = (value + alignment - 1U) & ~(alignment - 1U);
    return 1;
}

static int preboot_memory_attributes_match(
    const struct preboot_memory_extent *extent,
    const struct preboot_memory_request *request)
{
    return ((extent->attributes & request->required_attributes) ==
            request->required_attributes) &&
           ((extent->attributes & request->forbidden_attributes) == 0U);
}

static void preboot_memory_remove_extent(
    struct preboot_memory_plan *plan,
    uint32_t index)
{
    uint32_t cursor = index;

    for (cursor = index; (cursor + 1U) < plan->extent_count; ++cursor) {
        preboot_memory_copy_extent(&plan->extents[cursor],
                                  &plan->extents[cursor + 1U]);
    }
    plan->extent_count--;
}

static void preboot_memory_insert_extent(
    struct preboot_memory_plan *plan,
    uint32_t index,
    const struct preboot_memory_extent *extent)
{
    uint32_t cursor = plan->extent_count;

    while (cursor > index) {
        preboot_memory_copy_extent(&plan->extents[cursor],
                                   &plan->extents[cursor - 1U]);
        cursor--;
    }
    preboot_memory_copy_extent(&plan->extents[index], extent);
    plan->extent_count++;
}

static int preboot_memory_same_extent_class(
    const struct preboot_memory_extent *left,
    const struct preboot_memory_extent *right)
{
    return (left->attributes == right->attributes) &&
           (left->numa_node_id == right->numa_node_id) &&
           (left->owner == right->owner);
}



static enum preboot_memory_error preboot_memory_check_ownership(
    const struct hcd_validated_view *hcd)
{
    struct hcd_section_view boot;
    uint32_t mode = 0U;
    uint32_t flags = hcd->header.flags;

    if ((flags & HCD_F_FIRMWARE_BOOT_SERVICES_ACTIVE) != 0U) {
        return PREBOOT_MEM_E_BAD_STATE;
    }
    if (hcd_find_section(hcd, HCD_SECTION_BOOT_CONTEXT, &boot) != HCD_OK) {
        return PREBOOT_MEM_E_BAD_STATE;
    }
    mode = hcd_load_le32(hcd->buffer, boot.offset + 4U);
    if (mode == HCD_BOOT_MODE_UEFI) {
        if ((flags & HCD_F_EFI_EXIT_BOOT_SERVICES_DONE) == 0U) {
            return PREBOOT_MEM_E_BAD_STATE;
        }
    } else if (mode == HCD_BOOT_MODE_BIOS) {
        if ((flags & HCD_F_EFI_EXIT_BOOT_SERVICES_DONE) != 0U) {
            return PREBOOT_MEM_E_BAD_STATE;
        }
    } else {
        return PREBOOT_MEM_E_BAD_STATE;
    }
    return PREBOOT_MEM_OK;
}

static enum preboot_memory_error preboot_memory_add_usable_record(
    struct preboot_memory_plan *plan,
    const struct hcd_validated_view *hcd,
    const struct hcd_section_view *memory,
    uint32_t index)
{
    uint64_t offset = memory->offset +
        ((uint64_t)index * (uint64_t)memory->entry_size);
    struct preboot_memory_extent extent;
    uint32_t type = hcd_load_le32(hcd->buffer, offset + 16U);

    if (type != HCD_MEMORY_USABLE) {
        return PREBOOT_MEM_OK;
    }
    extent.base = hcd_load_le64(hcd->buffer, offset);
    extent.length = hcd_load_le64(hcd->buffer, offset + 8U);
    extent.attributes = hcd_load_le32(hcd->buffer, offset + 20U);
    extent.numa_node_id = hcd_load_le32(hcd->buffer, offset + 24U);
    extent.owner = hcd_load_le32(hcd->buffer, offset + 28U);
    extent.reserved0 = 0U;
    if ((extent.length == 0U) ||
        (extent.base > (UINT64_MAX - extent.length))) {
        return PREBOOT_MEM_E_OVERFLOW;
    }
    if (plan->extent_count > 0U) {
        struct preboot_memory_extent *previous =
            &plan->extents[plan->extent_count - 1U];
        uint64_t previous_end = previous->base + previous->length;

        if ((previous_end == extent.base) &&
            preboot_memory_same_extent_class(previous, &extent)) {
            previous->length += extent.length;
            return PREBOOT_MEM_OK;
        }
    }
    if (plan->extent_count >= PREBOOT_MAX_MEMORY_EXTENTS) {
        return PREBOOT_MEM_E_CAPACITY;
    }
    preboot_memory_copy_extent(&plan->extents[plan->extent_count], &extent);
    plan->extent_count++;
    return PREBOOT_MEM_OK;
}

enum preboot_memory_error preboot_memory_plan_init(
    struct preboot_memory_plan *plan,
    const struct hcd_validated_view *hcd)
{
    struct hcd_section_view memory;
    enum preboot_memory_error result = PREBOOT_MEM_OK;
    uint32_t index = 0U;

    if ((plan == NULL) || (hcd == NULL)) {
        return PREBOOT_MEM_E_NULL_INPUT;
    }
    result = preboot_memory_check_disjoint(
        plan, sizeof(*plan), hcd, sizeof(*hcd));
    if (result != PREBOOT_MEM_OK) {
        return result;
    }
    if (hcd->buffer == NULL) {
        return PREBOOT_MEM_E_NULL_INPUT;
    }
    if (hcd->size > (uint64_t)SIZE_MAX) {
        return PREBOOT_MEM_E_BAD_BUFFER_RANGE;
    }
    result = preboot_memory_check_disjoint(
        plan, sizeof(*plan), hcd->buffer, (size_t)hcd->size);
    if (result != PREBOOT_MEM_OK) {
        return result;
    }
    if (plan->magic != 0U) {
        return PREBOOT_MEM_E_BAD_STATE;
    }
    plan->extent_count = 0U;
    plan->allocation_count = 0U;
    plan->state = PREBOOT_MEM_UNINITIALIZED;
    plan->magic = PREBOOT_MEM_PLAN_MAGIC;
    result = preboot_memory_check_ownership(hcd);
    if (result != PREBOOT_MEM_OK) {
        preboot_memory_plan_reset_failed(plan);
        return result;
    }
    if (hcd_find_section(hcd, HCD_SECTION_MEMORY_MAP, &memory) != HCD_OK) {
        preboot_memory_plan_reset_failed(plan);
        return PREBOOT_MEM_E_BAD_STATE;
    }
    if (memory.entry_count > HCD_MAX_MEMORY_REGIONS) {
        preboot_memory_plan_reset_failed(plan);
        return PREBOOT_MEM_E_CAPACITY;
    }
    for (index = 0U; index < memory.entry_count; ++index) {
        result = preboot_memory_add_usable_record(plan, hcd, &memory, index);
        if (result != PREBOOT_MEM_OK) {
            preboot_memory_plan_reset_failed(plan);
            return result;
        }
    }
    if (plan->extent_count == 0U) {
        preboot_memory_plan_reset_failed(plan);
        return PREBOOT_MEM_E_NO_USABLE_MEMORY;
    }
    plan->state = PREBOOT_MEM_READY;
    return PREBOOT_MEM_OK;
}

static enum preboot_memory_error preboot_memory_validate_request(
    const struct preboot_memory_plan *plan,
    const struct preboot_memory_request *request)
{
    uint32_t index = 0U;

    if ((plan->magic != PREBOOT_MEM_PLAN_MAGIC) ||
        (plan->state != PREBOOT_MEM_READY) ||
        (plan->extent_count > PREBOOT_MAX_MEMORY_EXTENTS) ||
        (plan->allocation_count > PREBOOT_MAX_ALLOCATION_REQUESTS)) {
        return PREBOOT_MEM_E_BAD_STATE;
    }
    if ((request->request_id == HCD_ID_UNKNOWN) ||
        (request->resource_kind < PREBOOT_MEM_RESOURCE_KERNEL_IMAGE) ||
        (request->resource_kind > PREBOOT_MEM_RESOURCE_DMA_POOL) ||
        (request->owner_id == HCD_ID_UNKNOWN) || (request->size == 0U) ||
        !preboot_memory_power_of_two(request->alignment) ||
        (request->min_address >= request->max_address) ||
        ((request->flags & ~PREBOOT_MEM_F_KNOWN_MASK) != 0U) ||
        ((request->required_attributes & request->forbidden_attributes) != 0U) ||
        (((request->required_attributes | request->forbidden_attributes) &
          ~HCD_MEMORY_A_KNOWN_MASK) != 0U)) {
        return PREBOOT_MEM_E_BAD_REQUEST;
    }
    if ((request->boundary != 0U) &&
        (!preboot_memory_power_of_two(request->boundary) ||
         (request->size > request->boundary))) {
        return PREBOOT_MEM_E_BAD_REQUEST;
    }
    for (index = 0U; index < plan->allocation_count; ++index) {
        if (plan->allocations[index].request_id == request->request_id) {
            return PREBOOT_MEM_E_DUPLICATE_ID;
        }
    }
    HCD_ASSERT(plan->allocation_count <= PREBOOT_MAX_ALLOCATION_REQUESTS);
    return PREBOOT_MEM_OK;
}

static int preboot_memory_fit_extent(
    const struct preboot_memory_extent *extent,
    const struct preboot_memory_request *request,
    uint64_t *allocation_base)
{
    uint64_t extent_end = 0U;
    uint64_t lower = 0U;
    uint64_t upper = 0U;
    uint64_t candidate = 0U;
    uint64_t allocation_end = 0U;

    if (!preboot_memory_attributes_match(extent, request) ||
        (extent->length == 0U) ||
        (extent->base > (UINT64_MAX - extent->length))) {
        return 0;
    }
    extent_end = extent->base + extent->length;
    lower = (extent->base > request->min_address) ?
        extent->base : request->min_address;
    upper = (extent_end < request->max_address) ?
        extent_end : request->max_address;
    if ((lower >= upper) ||
        !preboot_memory_align_up(lower, request->alignment, &candidate)) {
        return 0;
    }
    if (request->boundary != 0U) {
        if (candidate > (UINT64_MAX - request->size)) {
            return 0;
        }
        allocation_end = candidate + request->size;
        if (((candidate / request->boundary) !=
             ((allocation_end - 1U) / request->boundary))) {
            uint64_t boundary_base =
                candidate & ~(request->boundary - 1U);
            uint64_t next_boundary = 0U;
            if (boundary_base > (UINT64_MAX - request->boundary)) {
                return 0;
            }
            next_boundary = boundary_base + request->boundary;
            if (!preboot_memory_align_up(next_boundary, request->alignment,
                                         &candidate)) {
                return 0;
            }
        }
    }
    if (candidate > (UINT64_MAX - request->size)) {
        return 0;
    }
    allocation_end = candidate + request->size;
    if ((candidate < lower) || (allocation_end > upper)) {
        return 0;
    }
    *allocation_base = candidate;
    return 1;
}

static enum preboot_memory_error preboot_memory_find_candidate(
    const struct preboot_memory_plan *plan,
    const struct preboot_memory_request *request,
    int preferred_node_only,
    uint32_t *extent_index,
    uint64_t *allocation_base)
{
    uint32_t index = 0U;

    for (index = 0U; index < plan->extent_count; ++index) {
        const struct preboot_memory_extent *extent = &plan->extents[index];

        if ((preferred_node_only != 0) &&
            (extent->numa_node_id != request->numa_node_id)) {
            continue;
        }
        if (preboot_memory_fit_extent(extent, request, allocation_base) != 0) {
            *extent_index = index;
            return PREBOOT_MEM_OK;
        }
    }
    return PREBOOT_MEM_E_NO_MEMORY;
}

static enum preboot_memory_error preboot_memory_consume_extent(
    struct preboot_memory_plan *plan,
    uint32_t index,
    uint64_t allocation_base,
    uint64_t allocation_size)
{
    struct preboot_memory_extent *extent = &plan->extents[index];
    struct preboot_memory_extent suffix;
    uint64_t old_end = 0U;
    uint64_t allocation_end = 0U;
    uint64_t old_base = extent->base;

    if ((extent->base > (UINT64_MAX - extent->length)) ||
        (allocation_base > (UINT64_MAX - allocation_size))) {
        return PREBOOT_MEM_E_OVERFLOW;
    }
    old_end = extent->base + extent->length;
    allocation_end = allocation_base + allocation_size;
    if ((allocation_base < old_base) || (allocation_end > old_end) ||
        (allocation_end <= allocation_base)) {
        return PREBOOT_MEM_E_OVERFLOW;
    }
    if ((allocation_base > old_base) && (allocation_end < old_end) &&
        (plan->extent_count >= PREBOOT_MAX_MEMORY_EXTENTS)) {
        return PREBOOT_MEM_E_CAPACITY;
    }
    if ((allocation_base == old_base) && (allocation_end == old_end)) {
        preboot_memory_remove_extent(plan, index);
    } else if (allocation_base == old_base) {
        extent->base = allocation_end;
        extent->length = old_end - allocation_end;
    } else if (allocation_end == old_end) {
        extent->length = allocation_base - old_base;
    } else {
        preboot_memory_copy_extent(&suffix, extent);
        suffix.base = allocation_end;
        suffix.length = old_end - allocation_end;
        extent->length = allocation_base - old_base;
        preboot_memory_insert_extent(plan, index + 1U, &suffix);
    }
    return PREBOOT_MEM_OK;
}

static enum preboot_memory_error preboot_memory_try_pass(
    struct preboot_memory_plan *plan,
    const struct preboot_memory_request *request,
    int preferred_node_only,
    struct preboot_memory_allocation *allocation)
{
    uint32_t extent_index = 0U;
    uint64_t base = 0U;
    enum preboot_memory_error result = preboot_memory_find_candidate(
        plan, request, preferred_node_only, &extent_index, &base);
    uint32_t attributes = 0U;
    uint32_t numa_node_id = HCD_ID_UNKNOWN;
    uint32_t owner = HCD_ID_UNKNOWN;

    if (result != PREBOOT_MEM_OK) {
        return result;
    }
    if (plan->allocation_count >= PREBOOT_MAX_ALLOCATION_REQUESTS) {
        return PREBOOT_MEM_E_CAPACITY;
    }
    attributes = plan->extents[extent_index].attributes;
    numa_node_id = plan->extents[extent_index].numa_node_id;
    owner = plan->extents[extent_index].owner;
    result = preboot_memory_consume_extent(plan, extent_index, base,
                                           request->size);
    if (result != PREBOOT_MEM_OK) {
        return result;
    }
    allocation->request_id = request->request_id;
    allocation->resource_kind = request->resource_kind;
    allocation->base = base;
    allocation->size = request->size;
    allocation->attributes = attributes;
    allocation->numa_node_id = numa_node_id;
    allocation->owner_id = request->owner_id;
    allocation->source_owner_id = owner;
    preboot_memory_copy_allocation(
        &plan->allocations[plan->allocation_count], allocation);
    plan->allocation_count++;
    return PREBOOT_MEM_OK;
}

enum preboot_memory_error preboot_memory_allocate(
    struct preboot_memory_plan *plan,
    const struct preboot_memory_request *request,
    struct preboot_memory_allocation *allocation)
{
    enum preboot_memory_error result = PREBOOT_MEM_OK;
    int has_numa_preference = 0;

    if ((plan == NULL) || (request == NULL) || (allocation == NULL)) {
        return PREBOOT_MEM_E_NULL_INPUT;
    }
    result = preboot_memory_check_disjoint(
        plan, sizeof(*plan), request, sizeof(*request));
    if (result != PREBOOT_MEM_OK) {
        return result;
    }
    result = preboot_memory_check_disjoint(
        plan, sizeof(*plan), allocation, sizeof(*allocation));
    if (result != PREBOOT_MEM_OK) {
        return result;
    }
    result = preboot_memory_check_disjoint(
        request, sizeof(*request), allocation, sizeof(*allocation));
    if (result != PREBOOT_MEM_OK) {
        return result;
    }
    result = preboot_memory_validate_request(plan, request);
    if (result != PREBOOT_MEM_OK) {
        return result;
    }
    has_numa_preference = (request->numa_node_id != HCD_ID_UNKNOWN);
    if (has_numa_preference != 0) {
        result = preboot_memory_try_pass(plan, request, 1, allocation);
        if ((result == PREBOOT_MEM_E_NO_MEMORY) &&
            ((request->flags & PREBOOT_MEM_F_ALLOW_NUMA_FALLBACK) != 0U)) {
            result = preboot_memory_try_pass(plan, request, 0, allocation);
        }
    } else {
        result = preboot_memory_try_pass(plan, request, 0, allocation);
    }
    HCD_ASSERT(plan->allocation_count <= PREBOOT_MAX_ALLOCATION_REQUESTS);
    HCD_ASSERT(plan->extent_count <= PREBOOT_MAX_MEMORY_EXTENTS);
    return result;
}

enum preboot_memory_error preboot_memory_plan_seal(
    struct preboot_memory_plan *plan)
{
    if (plan == NULL) {
        return PREBOOT_MEM_E_NULL_INPUT;
    }
    if ((plan->magic != PREBOOT_MEM_PLAN_MAGIC) ||
        (plan->state != PREBOOT_MEM_READY) ||
        ((plan->extent_count == 0U) && (plan->allocation_count == 0U)) ||
        (plan->extent_count > PREBOOT_MAX_MEMORY_EXTENTS) ||
        (plan->allocation_count > PREBOOT_MAX_ALLOCATION_REQUESTS)) {
        return PREBOOT_MEM_E_BAD_STATE;
    }
    plan->state = PREBOOT_MEM_SEALED;
    return PREBOOT_MEM_OK;
}
