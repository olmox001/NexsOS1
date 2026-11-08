#ifndef NXEC_PREBOOT_MEMORY_PLAN_H
#define NXEC_PREBOOT_MEMORY_PLAN_H

#include <stddef.h>
#include <stdint.h>

#include "hcd_parser.h"

#define PREBOOT_MAX_ALLOCATION_REQUESTS UINT32_C(64)
#define PREBOOT_MEM_PLAN_MAGIC UINT32_C(0x4E584D50)
#define PREBOOT_MEMORY_PLAN_INITIALIZER {0}
#define PREBOOT_MAX_MEMORY_EXTENTS \
    (HCD_MAX_MEMORY_REGIONS + PREBOOT_MAX_ALLOCATION_REQUESTS)

#define PREBOOT_MEM_F_ALLOW_NUMA_FALLBACK UINT32_C(0x00000001)
#define PREBOOT_MEM_F_KNOWN_MASK PREBOOT_MEM_F_ALLOW_NUMA_FALLBACK

enum preboot_memory_resource_kind {
    PREBOOT_MEM_RESOURCE_KERNEL_IMAGE = 1,
    PREBOOT_MEM_RESOURCE_KERNEL_STACK = 2,
    PREBOOT_MEM_RESOURCE_PAGE_TABLE_POOL = 3,
    PREBOOT_MEM_RESOURCE_HCD_BUFFER = 4,
    PREBOOT_MEM_RESOURCE_BHD = 5,
    PREBOOT_MEM_RESOURCE_DECOMPRESS_WORKSPACE = 6,
    PREBOOT_MEM_RESOURCE_MODULE_BUNDLE = 7,
    PREBOOT_MEM_RESOURCE_USERLAND_IMAGE = 8,
    PREBOOT_MEM_RESOURCE_DMA_POOL = 9
};

enum preboot_memory_owner {
    PREBOOT_MEM_OWNER_PREBOOT = 1,
    PREBOOT_MEM_OWNER_KERNEL = 2,
    PREBOOT_MEM_OWNER_MACHINE_MONITOR = 3,
    PREBOOT_MEM_OWNER_FIRMWARE_RUNTIME = 4
};

enum preboot_memory_error {
    PREBOOT_MEM_OK = 0,
    PREBOOT_MEM_E_NULL_INPUT = 1,
    PREBOOT_MEM_E_BAD_STATE = 2,
    PREBOOT_MEM_E_BAD_REQUEST = 3,
    PREBOOT_MEM_E_NO_USABLE_MEMORY = 4,
    PREBOOT_MEM_E_NO_MEMORY = 5,
    PREBOOT_MEM_E_CAPACITY = 6,
    PREBOOT_MEM_E_OVERFLOW = 7,
    PREBOOT_MEM_E_DUPLICATE_ID = 8,
    PREBOOT_MEM_E_BAD_BUFFER_RANGE = 9,
    PREBOOT_MEM_E_BUFFER_OVERLAP = 10
};

enum preboot_memory_state {
    PREBOOT_MEM_UNINITIALIZED = 0,
    PREBOOT_MEM_READY = 1,
    PREBOOT_MEM_SEALED = 2
};

struct preboot_memory_extent {
    uint64_t base;
    uint64_t length;
    uint32_t attributes;
    uint32_t numa_node_id;
    uint32_t owner;
    uint32_t reserved0;
};

struct preboot_memory_request {
    uint32_t request_id;
    uint32_t resource_kind;
    uint32_t owner_id;
    uint32_t required_attributes;
    uint32_t forbidden_attributes;
    uint32_t numa_node_id;
    uint32_t flags;
    uint64_t size;
    uint64_t alignment;
    uint64_t min_address;
    uint64_t max_address;
    uint64_t boundary;
};

struct preboot_memory_allocation {
    uint32_t request_id;
    uint32_t resource_kind;
    uint64_t base;
    uint64_t size;
    uint32_t attributes;
    uint32_t numa_node_id;
    uint32_t owner_id;
    uint32_t source_owner_id;
};

/*
 * The plan is a fixed, caller-owned, zero-initialized workspace. Production
 * preboot code must place it in an explicitly reserved bootstrap region, not
 * on a small stack. Initialize only after memory ownership transfers to the
 * loader: UEFI requires a successful ExitBootServices; firmware-active maps
 * are rejected. The workspace must not overlap the validated HCD view or its
 * input buffer. Initialization is one-shot; seal after the final allocation
 * and before handoff. A sealed plan cannot allocate or be reinitialized.
 */
struct preboot_memory_plan {
    struct preboot_memory_extent extents[PREBOOT_MAX_MEMORY_EXTENTS];
    struct preboot_memory_allocation allocations[PREBOOT_MAX_ALLOCATION_REQUESTS];
    uint32_t extent_count;
    uint32_t allocation_count;
    uint32_t state;
    uint32_t magic;
};

_Static_assert(sizeof(struct preboot_memory_extent) == 32U,
               "Preboot extent ABI size");
_Static_assert(sizeof(struct preboot_memory_allocation) == 40U,
               "Preboot allocation ABI size");
_Static_assert(sizeof(struct preboot_memory_plan) <= (size_t)147456U,
               "Preboot memory plan exceeds fixed workspace budget");

enum preboot_memory_error preboot_memory_plan_init(
    struct preboot_memory_plan *plan,
    const struct hcd_validated_view *hcd);

/* The plan, request and result buffers must be pairwise disjoint. */
enum preboot_memory_error preboot_memory_allocate(
    struct preboot_memory_plan *plan,
    const struct preboot_memory_request *request,
    struct preboot_memory_allocation *allocation);

enum preboot_memory_error preboot_memory_plan_seal(
    struct preboot_memory_plan *plan);

#endif
