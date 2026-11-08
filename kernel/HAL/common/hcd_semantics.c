#include <stdint.h>

#include "hcd_wire.h"
#include "hcd_parser.h"
#include "hcd_semantics.h"

#include "hcd_assert.h"
#include "hcd_endian.h"

struct hcd_cpu_accumulator {
    uint64_t supported_union[HCD_FEATURE_WORDS];
    uint64_t enabled_intersection[HCD_FEATURE_WORDS];
    uint64_t usable_intersection[HCD_FEATURE_WORDS];
    uint64_t first_supported[HCD_FEATURE_WORDS];
    uint64_t first_enabled[HCD_FEATURE_WORDS];
    uint64_t first_usable[HCD_FEATURE_WORDS];
    uint32_t online_count;
    uint32_t schedulable_count;
    uint32_t boot_count;
    uint32_t heterogeneous;
    uint32_t first_record_seen;
};

static enum hcd_error hcd_semantic_fail(
    struct hcd_validation_error *detail,
    enum hcd_error code,
    uint32_t section_type,
    uint32_t record_index,
    uint64_t offset)
{
    if (detail != NULL) {
        detail->code = (uint32_t)code;
        detail->section_type = section_type;
        detail->record_index = record_index;
        detail->reserved0 = 0U;
        detail->offset = offset;
    }
    HCD_ASSERT(code != HCD_OK);
    HCD_ASSERT((uint32_t)code <= (uint32_t)HCD_E_SECTION_NOT_FOUND);
    return code;
}

static const struct hcd_section_view *hcd_semantic_section(
    const struct hcd_validation_workspace *workspace,
    uint32_t type)
{
    uint32_t index = 0U;

    HCD_ASSERT(workspace != NULL);
    HCD_ASSERT(workspace->section_count <= HCD_MAX_SECTIONS);
    for (index = 0U; index < workspace->section_count; ++index) {
        if (workspace->sections[index].type == type) {
            return &workspace->sections[index];
        }
    }
    return NULL;
}

static enum hcd_error hcd_validate_cpu_catalog(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_section_view *catalog,
    uint64_t catalog_bits[HCD_FEATURE_WORDS],
    struct hcd_validation_error *detail)
{
    uint32_t index = 0U;
    uint32_t architecture_mask = 0U;

    if ((catalog == NULL) || (catalog->entry_count == 0U)) {
        return hcd_semantic_fail(detail, HCD_E_CPU_FEATURE_SET_INVALID,
                                 HCD_SECTION_CPU_FEATURE_CATALOG,
                                 HCD_ID_UNKNOWN, 0U);
    }
    architecture_mask = UINT32_C(1) << header->architecture;
    for (index = 0U; index < catalog->entry_count; ++index) {
        uint64_t offset = catalog->offset +
            ((uint64_t)index * (uint64_t)catalog->entry_size);
        uint32_t feature_id = hcd_load_le32(buffer, offset);
        uint32_t arch_mask = hcd_load_le32(buffer, offset + 4U);
        uint32_t flags = hcd_load_le32(buffer, offset + 8U);
        uint32_t reserved = hcd_load_le32(buffer, offset + 28U);
        uint32_t word = feature_id / 64U;
        uint32_t bit = feature_id % 64U;

        if ((feature_id >= HCD_FEATURE_BITS) ||
            ((arch_mask & architecture_mask) == 0U) ||
            ((arch_mask & ~(HCD_ARCH_MASK_AMD64 | HCD_ARCH_MASK_ARM64)) != 0U) ||
            (flags != 0U) || (reserved != 0U)) {
            return hcd_semantic_fail(detail, HCD_E_CPU_FEATURE_SET_INVALID,
                                     HCD_SECTION_CPU_FEATURE_CATALOG,
                                     index, offset);
        }
        if ((catalog_bits[word] & (UINT64_C(1) << bit)) != 0U) {
            return hcd_semantic_fail(detail, HCD_E_CPU_FEATURE_SET_INVALID,
                                     HCD_SECTION_CPU_FEATURE_CATALOG,
                                     index, offset);
        }
        catalog_bits[word] |= UINT64_C(1) << bit;
    }
    HCD_ASSERT(catalog->entry_count <= HCD_FEATURE_BITS);
    HCD_ASSERT(architecture_mask != 0U);
    return HCD_OK;
}

static enum hcd_error hcd_validate_cpu_summary(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_section_view *summary,
    const struct hcd_section_view *cpus,
    uint32_t *cpu_count,
    uint32_t *boot_cpu_id,
    struct hcd_validation_error *detail)
{
    uint64_t offset = summary->offset;
    uint32_t flags = hcd_load_le32(buffer, offset + 12U);

    *cpu_count = hcd_load_le32(buffer, offset);
    *boot_cpu_id = hcd_load_le32(buffer, offset + 4U);
    if ((*cpu_count == 0U) || (*cpu_count > HCD_MAX_CPUS) ||
        (*cpu_count != cpus->entry_count) || (*boot_cpu_id >= *cpu_count)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_CPU_SUMMARY, 0U, offset);
    }
    if ((hcd_load_le32(buffer, offset + 8U) != header->architecture) ||
        (hcd_load_le32(buffer, offset + 16U) != HCD_FEATURE_WORDS) ||
        (hcd_load_le32(buffer, offset + 20U) != 0U) ||
        (hcd_load_le64(buffer, offset + 216U) != 0U) ||
        ((flags & ~HCD_CPU_SUMMARY_F_KNOWN_MASK) != 0U)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_CPU_SUMMARY, 0U, offset + 8U);
    }
    HCD_ASSERT(*cpu_count <= HCD_MAX_CPUS);
    HCD_ASSERT(*boot_cpu_id < *cpu_count);
    return HCD_OK;
}


static enum hcd_error hcd_validate_cpu_identity(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_section_view *raw_registers,
    uint64_t offset,
    uint32_t index,
    uint32_t cpu_count,
    uint32_t boot_cpu_id,
    uint32_t *flags_out,
    struct hcd_validation_error *detail)
{
    uint32_t flags = hcd_load_le32(buffer, offset + 28U);
    uint32_t raw_index = hcd_load_le32(buffer, offset + 272U);
    uint32_t raw_count = (raw_registers == NULL) ? 0U :
                         raw_registers->entry_count;

    if ((hcd_load_le32(buffer, offset) != index) ||
        (hcd_load_le32(buffer, offset + 32U) != header->architecture) ||
        ((flags & HCD_CPU_F_PRESENT) == 0U) ||
        ((flags & ~HCD_CPU_F_KNOWN_MASK) != 0U) ||
        (hcd_load_le32(buffer, offset + 276U) != 0U) ||
        (hcd_load_le64(buffer, offset + 280U) != 0U)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_CPU_RECORDS, index, offset);
    }
    if (((flags & HCD_CPU_F_SCHEDULABLE) != 0U) &&
        ((flags & HCD_CPU_F_ONLINE) == 0U)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_CPU_RECORDS, index, offset + 28U);
    }
    if (((flags & HCD_CPU_F_BOOT) != 0U) != (index == boot_cpu_id)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_CPU_RECORDS, index, offset + 28U);
    }
    if ((raw_index != HCD_ID_UNKNOWN) && (raw_index >= raw_count)) {
        return hcd_semantic_fail(detail, HCD_E_CPU_FEATURE_SET_INVALID,
                                 HCD_SECTION_CPU_RECORDS, index, offset + 272U);
    }
    if ((index == boot_cpu_id) && ((flags & HCD_CPU_F_ONLINE) == 0U)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_CPU_RECORDS, index, offset + 28U);
    }
    *flags_out = flags;
    HCD_ASSERT(index < cpu_count);
    HCD_ASSERT(*flags_out == flags);
    return HCD_OK;
}

static enum hcd_error hcd_accumulate_cpu_features(
    const uint8_t *buffer,
    uint64_t offset,
    uint32_t flags,
    const uint64_t catalog_bits[HCD_FEATURE_WORDS],
    uint32_t index,
    struct hcd_cpu_accumulator *accumulator,
    struct hcd_validation_error *detail)
{
    uint32_t word = 0U;
    int online = ((flags & HCD_CPU_F_ONLINE) != 0U);
    int schedulable = ((flags & HCD_CPU_F_SCHEDULABLE) != 0U);

    for (word = 0U; word < HCD_FEATURE_WORDS; ++word) {
        uint64_t supported = hcd_load_le64(buffer, offset + 80U +
                                      ((uint64_t)word * 8U));
        uint64_t enabled = hcd_load_le64(buffer, offset + 144U +
                                    ((uint64_t)word * 8U));
        uint64_t usable = hcd_load_le64(buffer, offset + 208U +
                                   ((uint64_t)word * 8U));

        if (((enabled & ~supported) != 0U) ||
            ((usable & ~enabled) != 0U) ||
            ((supported & ~catalog_bits[word]) != 0U)) {
            return hcd_semantic_fail(detail, HCD_E_CPU_FEATURE_SET_INVALID,
                                     HCD_SECTION_CPU_RECORDS, index,
                                     offset + 80U + ((uint64_t)word * 8U));
        }
        accumulator->supported_union[word] |= supported;
        if (online != 0) {
            accumulator->enabled_intersection[word] &= enabled;
        }
        if (schedulable != 0) {
            accumulator->usable_intersection[word] &= usable;
        }
        if (accumulator->first_record_seen == 0U) {
            accumulator->first_supported[word] = supported;
            accumulator->first_enabled[word] = enabled;
            accumulator->first_usable[word] = usable;
        } else if ((accumulator->first_supported[word] != supported) ||
                   (accumulator->first_enabled[word] != enabled) ||
                   (accumulator->first_usable[word] != usable)) {
            accumulator->heterogeneous = 1U;
        }
    }
    accumulator->first_record_seen = 1U;
    HCD_ASSERT(accumulator->first_record_seen == 1U);
    HCD_ASSERT(word == HCD_FEATURE_WORDS);
    return HCD_OK;
}

static enum hcd_error hcd_validate_cpu_record(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_section_view *raw_registers,
    const uint64_t catalog_bits[HCD_FEATURE_WORDS],
    uint32_t index,
    uint64_t section_offset,
    uint32_t cpu_count,
    uint32_t boot_cpu_id,
    struct hcd_cpu_accumulator *accumulator,
    struct hcd_validation_error *detail)
{
    uint64_t offset = section_offset +
        ((uint64_t)index * (uint64_t)sizeof(struct hcd_cpu_record));
    uint32_t flags = 0U;
    enum hcd_error result = hcd_validate_cpu_identity(
        buffer, header, raw_registers, offset, index, cpu_count,
        boot_cpu_id, &flags, detail);

    if (result != HCD_OK) {
        return result;
    }
    if ((flags & HCD_CPU_F_BOOT) != 0U) {
        accumulator->boot_count++;
    }
    if ((flags & HCD_CPU_F_ONLINE) != 0U) {
        accumulator->online_count++;
    }
    if ((flags & HCD_CPU_F_SCHEDULABLE) != 0U) {
        accumulator->schedulable_count++;
    }
    result = hcd_accumulate_cpu_features(buffer, offset, flags,
                                         catalog_bits, index,
                                         accumulator, detail);
    HCD_ASSERT(index < cpu_count);
    HCD_ASSERT(offset < header->total_size);
    return result;
}

static enum hcd_error hcd_check_cpu_aggregates(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_section_view *summary,
    const struct hcd_section_view *cpus,
    const struct hcd_cpu_accumulator *accumulator,
    struct hcd_validation_error *detail)
{
    uint64_t summary_offset = summary->offset;
    uint32_t word = 0U;

    if ((accumulator->boot_count != 1U) ||
        (accumulator->online_count == 0U) ||
        (accumulator->schedulable_count == 0U)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_CPU_RECORDS, HCD_ID_UNKNOWN,
                                 cpus->offset);
    }
    if (((accumulator->heterogeneous != 0U) !=
         ((hcd_load_le32(buffer, summary_offset + 12U) &
           HCD_CPU_SUMMARY_F_HETEROGENEOUS) != 0U)) ||
        ((accumulator->heterogeneous != 0U) !=
         ((header->flags & HCD_F_HETEROGENEOUS_CPUS) != 0U))) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_CPU_SUMMARY, 0U,
                                 summary_offset + 12U);
    }
    for (word = 0U; word < HCD_FEATURE_WORDS; ++word) {
        if ((hcd_load_le64(buffer, summary_offset + 24U +
                           ((uint64_t)word * 8U)) !=
             accumulator->supported_union[word]) ||
            (hcd_load_le64(buffer, summary_offset + 88U +
                           ((uint64_t)word * 8U)) !=
             accumulator->enabled_intersection[word]) ||
            (hcd_load_le64(buffer, summary_offset + 152U +
                           ((uint64_t)word * 8U)) !=
             accumulator->usable_intersection[word])) {
            return hcd_semantic_fail(detail, HCD_E_CPU_FEATURE_SET_INVALID,
                                     HCD_SECTION_CPU_SUMMARY, 0U,
                                     summary_offset + 24U +
                                     ((uint64_t)word * 8U));
        }
    }
    HCD_ASSERT(accumulator->boot_count == 1U);
    HCD_ASSERT(accumulator->online_count > 0U);
    return HCD_OK;
}

static enum hcd_error hcd_validate_cpu_records(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_validation_workspace *workspace,
    const struct hcd_section_view *summary,
    const struct hcd_section_view *cpus,
    const uint64_t catalog_bits[HCD_FEATURE_WORDS],
    uint32_t cpu_count,
    uint32_t boot_cpu_id,
    struct hcd_validation_error *detail)
{
    struct hcd_cpu_accumulator accumulator;
    const struct hcd_section_view *raw_registers = hcd_semantic_section(
        workspace, HCD_SECTION_CPU_RAW_REGISTERS);
    uint32_t index = 0U;
    uint32_t word = 0U;
    enum hcd_error result = HCD_OK;

    accumulator.online_count = 0U;
    accumulator.schedulable_count = 0U;
    accumulator.boot_count = 0U;
    accumulator.heterogeneous = 0U;
    accumulator.first_record_seen = 0U;
    for (word = 0U; word < HCD_FEATURE_WORDS; ++word) {
        accumulator.supported_union[word] = 0U;
        accumulator.first_supported[word] = 0U;
        accumulator.first_enabled[word] = 0U;
        accumulator.first_usable[word] = 0U;
        accumulator.enabled_intersection[word] = UINT64_MAX;
        accumulator.usable_intersection[word] = UINT64_MAX;
    }
    for (index = 0U; index < cpu_count; ++index) {
        result = hcd_validate_cpu_record(
            buffer, header, raw_registers, catalog_bits, index,
            cpus->offset, cpu_count, boot_cpu_id, &accumulator, detail);
        if (result != HCD_OK) {
            return result;
        }
    }
    result = hcd_check_cpu_aggregates(buffer, header, summary, cpus,
                                      &accumulator, detail);
    HCD_ASSERT(cpu_count <= HCD_MAX_CPUS);
    HCD_ASSERT((uint32_t)result <= (uint32_t)HCD_E_SECTION_NOT_FOUND);
    return result;
}


static enum hcd_error hcd_validate_memory_record(
    const uint8_t *buffer,
    const struct hcd_section_view *memory,
    uint32_t index,
    uint64_t previous_end,
    uint64_t *current_end,
    struct hcd_validation_error *detail)
{
    uint64_t offset = memory->offset +
        ((uint64_t)index * (uint64_t)memory->entry_size);
    uint64_t base = hcd_load_le64(buffer, offset);
    uint64_t length = hcd_load_le64(buffer, offset + 8U);
    uint32_t type = hcd_load_le32(buffer, offset + 16U);
    uint32_t attributes = hcd_load_le32(buffer, offset + 20U);

    if ((length == 0U) || (base > (UINT64_MAX - length)) ||
        (type < HCD_MEMORY_USABLE) || (type > HCD_MEMORY_UNACCEPTED) ||
        ((attributes & ~HCD_MEMORY_A_KNOWN_MASK) != 0U) ||
        (hcd_load_le64(buffer, offset + 32U) != 0U)) {
        return hcd_semantic_fail(detail, HCD_E_MEMORY_MAP_INVALID,
                                 HCD_SECTION_MEMORY_MAP, index, offset);
    }
    if ((index != 0U) && (base < previous_end)) {
        return hcd_semantic_fail(detail, HCD_E_MEMORY_MAP_INVALID,
                                 HCD_SECTION_MEMORY_MAP, index, offset);
    }
    *current_end = base + length;
    HCD_ASSERT(*current_end > base);
    HCD_ASSERT(type >= HCD_MEMORY_USABLE);
    return HCD_OK;
}

static enum hcd_error hcd_validate_memory_map(
    const uint8_t *buffer,
    const struct hcd_section_view *memory,
    struct hcd_validation_error *detail)
{
    uint64_t previous_end = 0U;
    uint32_t index = 0U;
    enum hcd_error result = HCD_OK;

    if ((memory == NULL) || (memory->entry_count == 0U)) {
        return hcd_semantic_fail(detail, HCD_E_MEMORY_MAP_INVALID,
                                 HCD_SECTION_MEMORY_MAP, HCD_ID_UNKNOWN, 0U);
    }
    for (index = 0U; index < memory->entry_count; ++index) {
        uint64_t current_end = 0U;

        result = hcd_validate_memory_record(buffer, memory, index,
                                            previous_end, &current_end, detail);
        if (result != HCD_OK) {
            return result;
        }
        previous_end = current_end;
    }
    HCD_ASSERT(memory->entry_count <=
               (HCD_MAX_TOTAL_SIZE / sizeof(struct hcd_memory_record)));
    HCD_ASSERT(previous_end > 0U);
    return HCD_OK;
}



static int hcd_range_in_memory_types(
    const uint8_t *buffer,
    const struct hcd_section_view *memory,
    uint64_t base,
    uint64_t length,
    uint32_t type_mask)
{
    uint32_t low = 0U;
    uint32_t high = 0U;
    uint32_t iteration = 0U;
    uint64_t end = 0U;

    if ((buffer == NULL) || (memory == NULL) || (length == 0U) ||
        (base > (UINT64_MAX - length))) {
        return 0;
    }
    high = memory->entry_count;
    end = base + length;
    for (iteration = 0U; iteration < 64U; ++iteration) {
        uint32_t middle = 0U;
        uint64_t offset = 0U;
        uint64_t region_base = 0U;

        if (low >= high) {
            break;
        }
        middle = low + ((high - low) / 2U);
        offset = memory->offset +
            ((uint64_t)middle * (uint64_t)memory->entry_size);
        region_base = hcd_load_le64(buffer, offset);
        if (region_base <= base) {
            low = middle + 1U;
        } else {
            high = middle;
        }
    }
    if (low == 0U) {
        return 0;
    }
    {
        uint32_t index = low - 1U;
        uint64_t offset = memory->offset +
            ((uint64_t)index * (uint64_t)memory->entry_size);
        uint64_t region_base = hcd_load_le64(buffer, offset);
        uint64_t region_length = hcd_load_le64(buffer, offset + 8U);
        uint64_t region_end = region_base + region_length;
        uint32_t type = hcd_load_le32(buffer, offset + 16U);

        if ((type < 32U) && ((type_mask & (UINT32_C(1) << type)) != 0U) &&
            (base >= region_base) && (end <= region_end)) {
            return 1;
        }
    }
    HCD_ASSERT(iteration <= 64U);
    HCD_ASSERT(length > 0U);
    return 0;
}


static enum hcd_error hcd_validate_topology_node(
    const uint8_t *buffer,
    const struct hcd_section_view *topology,
    uint32_t index,
    uint32_t cpu_count,
    struct hcd_validation_workspace *workspace,
    struct hcd_validation_error *detail)
{
    uint64_t offset = topology->offset +
        ((uint64_t)index * (uint64_t)topology->entry_size);
    uint32_t node_id = hcd_load_le32(buffer, offset);
    uint32_t parent_id = hcd_load_le32(buffer, offset + 4U);
    uint32_t logical_id = hcd_load_le32(buffer, offset + 8U);
    uint32_t level = hcd_load_le32(buffer, offset + 12U);
    uint32_t flags = hcd_load_le32(buffer, offset + 16U);

    if ((node_id != index) || (level < HCD_TOPOLOGY_PACKAGE) ||
        (level > HCD_TOPOLOGY_THREAD) || (flags != 0U) ||
        (hcd_load_le64(buffer, offset + 24U) != 0U) ||
        ((parent_id != HCD_ID_UNKNOWN) && (parent_id >= node_id))) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_TOPOLOGY, index, offset);
    }
    if (parent_id != HCD_ID_UNKNOWN) {
        uint64_t parent_offset = topology->offset +
            ((uint64_t)parent_id * (uint64_t)topology->entry_size);
        uint32_t parent_level = hcd_load_le32(buffer, parent_offset + 12U);

        if (parent_level >= level) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_TOPOLOGY, index, offset + 4U);
        }
    } else if (level != HCD_TOPOLOGY_PACKAGE) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_TOPOLOGY, index, offset + 12U);
    }
    if (logical_id != HCD_ID_UNKNOWN) {
        uint32_t word = logical_id / 64U;
        uint32_t bit = logical_id % 64U;

        if ((logical_id >= cpu_count) ||
            (level != HCD_TOPOLOGY_THREAD) ||
            ((workspace->cpu_seen[word] & (UINT64_C(1) << bit)) != 0U)) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_TOPOLOGY, index, offset + 8U);
        }
        workspace->cpu_seen[word] |= UINT64_C(1) << bit;
    } else if (level == HCD_TOPOLOGY_THREAD) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_TOPOLOGY, index, offset + 8U);
    }
    HCD_ASSERT(index < topology->entry_count);
    HCD_ASSERT(cpu_count <= HCD_MAX_CPUS);
    return HCD_OK;
}

static enum hcd_error hcd_validate_topology(
    const uint8_t *buffer,
    const struct hcd_section_view *topology,
    const struct hcd_section_view *cpus,
    uint32_t cpu_count,
    struct hcd_validation_workspace *workspace,
    struct hcd_validation_error *detail)
{
    uint32_t index = 0U;
    enum hcd_error result = HCD_OK;

    if ((topology == NULL) || (topology->entry_count < cpu_count)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_TOPOLOGY, HCD_ID_UNKNOWN, 0U);
    }
    for (index = 0U; index < (HCD_MAX_CPUS / 64U); ++index) {
        workspace->cpu_seen[index] = 0U;
    }
    for (index = 0U; index < topology->entry_count; ++index) {
        result = hcd_validate_topology_node(buffer, topology, index,
                                            cpu_count, workspace, detail);
        if (result != HCD_OK) {
            return result;
        }
    }
    for (index = 0U; index < cpu_count; ++index) {
        uint64_t cpu_offset = cpus->offset +
            ((uint64_t)index * (uint64_t)cpus->entry_size);
        uint32_t cpu_flags = hcd_load_le32(buffer, cpu_offset + 28U);
        uint32_t word = index / 64U;
        uint32_t bit = index % 64U;

        if (((cpu_flags & HCD_CPU_F_ONLINE) != 0U) &&
            ((workspace->cpu_seen[word] & (UINT64_C(1) << bit)) == 0U)) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_TOPOLOGY, index, cpu_offset);
        }
    }
    HCD_ASSERT(topology->entry_count >= cpu_count);
    HCD_ASSERT(cpu_count <= HCD_MAX_CPUS);
    return HCD_OK;
}

static enum hcd_error hcd_validate_mmu(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_section_view *mmu,
    uint32_t cpu_count,
    struct hcd_validation_error *detail)
{
    uint32_t index = 0U;

    if ((mmu == NULL) || (mmu->entry_count != cpu_count)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_MMU_CAPABILITIES,
                                 HCD_ID_UNKNOWN, 0U);
    }
    for (index = 0U; index < cpu_count; ++index) {
        uint64_t offset = mmu->offset +
            ((uint64_t)index * (uint64_t)mmu->entry_size);
        uint32_t levels_supported = hcd_load_le32(buffer, offset + 24U);
        uint32_t levels_active = hcd_load_le32(buffer, offset + 28U);
        uint32_t physical_bits = hcd_load_le32(buffer, offset + 8U);
        uint32_t virtual_bits = hcd_load_le32(buffer, offset + 12U);
        uint32_t page_granules = hcd_load_le32(buffer, offset + 16U);
        uint32_t block_granules = hcd_load_le32(buffer, offset + 20U);
        uint32_t maximum_virtual_bits =
            (header->architecture == HCD_ARCH_AMD64) ? 57U : 52U;

        if ((hcd_load_le32(buffer, offset) != header->architecture) ||
            (hcd_load_le32(buffer, offset + 4U) != 0U) ||
            (physical_bits == 0U) || (physical_bits > 52U) ||
            (virtual_bits == 0U) || (virtual_bits > maximum_virtual_bits) ||
            (page_granules == 0U) || ((page_granules & ~UINT32_C(7)) != 0U) ||
            ((block_granules & ~UINT32_C(7)) != 0U) ||
            (levels_supported == 0U) ||
            ((levels_supported & ~UINT32_C(31)) != 0U) ||
            (levels_active == 0U) ||
            ((levels_active & ~levels_supported) != 0U) ||
            (hcd_load_le32(buffer, offset + 44U) != index)) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_MMU_CAPABILITIES,
                                     index, offset);
        }
    }
    HCD_ASSERT(mmu->entry_count == cpu_count);
    HCD_ASSERT(cpu_count <= HCD_MAX_CPUS);
    return HCD_OK;
}

static int hcd_mmu_u64_is_power_of_two(uint64_t value)
{
    return (value != 0U) && ((value & (value - UINT64_C(1))) == 0U);
}

static enum hcd_error hcd_validate_mmu_v2(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_validation_workspace *workspace,
    const struct hcd_section_view *mmu,
    uint32_t cpu_count,
    struct hcd_validation_error *detail)
{
    const struct hcd_section_view *configs = hcd_semantic_section(
        workspace, HCD_SECTION_MMU_TRANSLATION_CONFIGS);
    const struct hcd_section_view *mappings = hcd_semantic_section(
        workspace, HCD_SECTION_MMU_MAPPING_SIZES);
    uint32_t config_index = 0U;
    uint32_t mapping_index = 0U;
    uint32_t previous_cpu = HCD_ID_UNKNOWN;
    uint32_t previous_config = HCD_ID_UNKNOWN;
    uint32_t group_active_count = 0U;
    uint32_t previous_mapping_cpu = HCD_ID_UNKNOWN;
    uint32_t previous_mapping_config = HCD_ID_UNKNOWN;
    uint32_t previous_mapping_level = HCD_ID_UNKNOWN;
    uint32_t previous_mapping_kind = 0U;
    uint64_t previous_mapping_size = 0U;

    if ((configs == NULL) || (mappings == NULL) || (mmu == NULL) ||
        (configs->entry_count < cpu_count) ||
        (mappings->entry_count == 0U) ||
        (configs->entry_count > HCD_MAX_MMU_CONFIGURATIONS) ||
        (mappings->entry_count > HCD_MAX_MMU_MAPPING_SIZES)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_MMU_TRANSLATION_CONFIGS,
                                 HCD_ID_UNKNOWN, 0U);
    }

    for (config_index = 0U; config_index < configs->entry_count;
         ++config_index) {
        uint64_t config_offset = configs->offset +
            ((uint64_t)config_index * (uint64_t)configs->entry_size);
        uint32_t architecture = hcd_load_le32(buffer, config_offset);
        uint32_t cpu_id = hcd_load_le32(buffer, config_offset + 4U);
        uint32_t configuration_id = hcd_load_le32(buffer, config_offset + 8U);
        uint32_t regime = hcd_load_le32(buffer, config_offset + 12U);
        uint32_t flags = hcd_load_le32(buffer, config_offset + 16U);
        uint32_t exception_level = hcd_load_le32(buffer, config_offset + 20U);
        uint32_t physical_bits = hcd_load_le32(buffer, config_offset + 24U);
        uint32_t intermediate_bits = hcd_load_le32(buffer, config_offset + 28U);
        uint32_t virtual_bits = hcd_load_le32(buffer, config_offset + 32U);
        uint32_t granule = hcd_load_le32(buffer, config_offset + 36U);
        uint32_t level_count = hcd_load_le32(buffer, config_offset + 40U);
        uint32_t va_range = hcd_load_le32(buffer, config_offset + 44U);
        uint32_t tlb_features = hcd_load_le32(buffer, config_offset + 48U);
        uint32_t memory_attributes = hcd_load_le32(buffer, config_offset + 52U);
        uint32_t is_current =
            (flags & (uint32_t)HCD_MMU_CONFIG_F_CURRENT) != 0U;
        uint32_t config_mapping_count = 0U;
        uint32_t terminal_page_seen = 0U;
        uint32_t per_cpu_config_count = 0U;
        uint64_t legacy_offset = 0U;
        int regime_valid = 0;

        if ((cpu_id >= cpu_count) ||
            (architecture != header->architecture) ||
            ((flags & ~(uint32_t)HCD_MMU_CONFIG_F_KNOWN_MASK) != 0U) ||
            (((flags &
               (uint32_t)HCD_MMU_CONFIG_F_TRANSLATION_ENABLED) != 0U) &&
             (is_current == 0U)) ||
            (physical_bits == 0U) || (physical_bits > 52U) ||
            ((tlb_features & ~HCD_MMU_TLB_KNOWN_MASK) != 0U) ||
            ((memory_attributes & ~HCD_MMU_ATTR_KNOWN_MASK) != 0U) ||
            (hcd_load_le32(buffer, config_offset + 56U) != 0U) ||
            (hcd_load_le32(buffer, config_offset + 60U) != 0U)) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_MMU_TRANSLATION_CONFIGS,
                                     config_index, config_offset);
        }

        if (architecture == HCD_ARCH_AMD64) {
            regime_valid =
                (regime == HCD_MMU_REGIME_AMD64) &&
                (exception_level == HCD_MMU_EXCEPTION_LEVEL_NONE) &&
                (intermediate_bits == 0U) &&
                (virtual_bits > 0U) &&
                (level_count >= 4U) && (level_count <= 5U) &&
                (virtual_bits <= ((level_count == 4U) ? 48U : 57U)) &&
                (granule == 4096U) &&
                (va_range == HCD_MMU_VA_RANGE_AMD64_CANONICAL);
        } else if (regime == HCD_MMU_REGIME_ARM64_STAGE1) {
            regime_valid =
                ((exception_level == HCD_MMU_EXCEPTION_LEVEL_EL1) ||
                 (exception_level == HCD_MMU_EXCEPTION_LEVEL_EL2)) &&
                (intermediate_bits == 0U) &&
                (virtual_bits > 0U) && (virtual_bits <= 52U) &&
                (level_count > 0U) && (level_count <= 5U) &&
                ((granule == 4096U) || (granule == 16384U) ||
                 (granule == 65536U)) &&
                ((va_range == HCD_MMU_VA_RANGE_ARM64_LOWER) ||
                 (va_range == HCD_MMU_VA_RANGE_ARM64_UPPER));
        } else if (regime == HCD_MMU_REGIME_ARM64_STAGE2) {
            regime_valid =
                (exception_level == HCD_MMU_EXCEPTION_LEVEL_EL2) &&
                (intermediate_bits > 0U) && (intermediate_bits <= 52U) &&
                (virtual_bits == 0U) &&
                (level_count > 0U) && (level_count <= 5U) &&
                ((granule == 4096U) || (granule == 16384U) ||
                 (granule == 65536U)) &&
                (va_range == HCD_MMU_VA_RANGE_ARM64_STAGE2);
        }
        if (regime_valid == 0) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_MMU_TRANSLATION_CONFIGS,
                                     config_index, config_offset + 12U);
        }

        legacy_offset = mmu->offset +
            ((uint64_t)cpu_id * (uint64_t)mmu->entry_size);
        if ((hcd_load_le32(buffer, legacy_offset) != architecture) ||
            (hcd_load_le32(buffer, legacy_offset + 44U) != cpu_id)) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_MMU_TRANSLATION_CONFIGS,
                                     config_index, config_offset);
        }

        if (cpu_id != previous_cpu) {
            if ((previous_cpu != HCD_ID_UNKNOWN) &&
                ((group_active_count != 1U) ||
                 (cpu_id != previous_cpu + 1U))) {
                return hcd_semantic_fail(
                    detail, HCD_E_SEMANTIC_INCONSISTENCY,
                    HCD_SECTION_MMU_TRANSLATION_CONFIGS, config_index,
                    config_offset + 4U);
            }
            if (configuration_id != 0U) {
                return hcd_semantic_fail(
                    detail, HCD_E_SEMANTIC_INCONSISTENCY,
                    HCD_SECTION_MMU_TRANSLATION_CONFIGS, config_index,
                    config_offset + 8U);
            }
            group_active_count = 0U;
            per_cpu_config_count = 1U;
        } else {
            if (configuration_id != previous_config + 1U) {
                return hcd_semantic_fail(
                    detail, HCD_E_SEMANTIC_INCONSISTENCY,
                    HCD_SECTION_MMU_TRANSLATION_CONFIGS, config_index,
                    config_offset + 8U);
            }
            per_cpu_config_count++;
        }
        if ((per_cpu_config_count > 16U) ||
            ((cpu_id == 0U) && (config_index == 0U) &&
             (configuration_id != 0U))) {
            return hcd_semantic_fail(detail, HCD_E_COUNT_LIMIT,
                                     HCD_SECTION_MMU_TRANSLATION_CONFIGS,
                                     config_index, config_offset + 8U);
        }
        if (is_current != 0U) {
            group_active_count++;
            if (group_active_count > 1U) {
                return hcd_semantic_fail(
                    detail, HCD_E_SEMANTIC_INCONSISTENCY,
                    HCD_SECTION_MMU_TRANSLATION_CONFIGS, config_index,
                    config_offset + 16U);
            }
        }

        while (mapping_index < mappings->entry_count) {
            uint64_t mapping_offset = mappings->offset +
                ((uint64_t)mapping_index * (uint64_t)mappings->entry_size);
            uint32_t mapping_cpu = hcd_load_le32(buffer, mapping_offset);
            uint32_t mapping_config =
                hcd_load_le32(buffer, mapping_offset + 4U);
            uint32_t mapping_level =
                hcd_load_le32(buffer, mapping_offset + 8U);
            uint32_t mapping_kind =
                hcd_load_le32(buffer, mapping_offset + 12U);
            uint64_t mapping_size = hcd_load_le64(buffer, mapping_offset + 16U);
            uint32_t mapping_flags =
                hcd_load_le32(buffer, mapping_offset + 24U);
            int same_config =
                (mapping_cpu == cpu_id) &&
                (mapping_config == configuration_id);
            int ordered = 1;
            int mapping_kind_valid =
                (mapping_kind == HCD_MMU_MAPPING_PAGE) ||
                (mapping_kind == HCD_MMU_MAPPING_BLOCK);
            int mapping_shape_valid = 0;

            if ((mapping_cpu < cpu_id) ||
                ((mapping_cpu == cpu_id) &&
                 (mapping_config < configuration_id))) {
                return hcd_semantic_fail(
                    detail, HCD_E_SEMANTIC_INCONSISTENCY,
                    HCD_SECTION_MMU_MAPPING_SIZES, mapping_index,
                    mapping_offset);
            }
            if (!same_config) {
                break;
            }
            if (mapping_index != 0U) {
                ordered = (mapping_cpu > previous_mapping_cpu) ||
                    ((mapping_cpu == previous_mapping_cpu) &&
                     ((mapping_config > previous_mapping_config) ||
                      ((mapping_config == previous_mapping_config) &&
                       ((mapping_level > previous_mapping_level) ||
                        ((mapping_level == previous_mapping_level) &&
                         ((mapping_kind > previous_mapping_kind) ||
                          ((mapping_kind == previous_mapping_kind) &&
                           (mapping_size > previous_mapping_size))))))));
            }
            if (mapping_kind == HCD_MMU_MAPPING_PAGE) {
                mapping_shape_valid =
                    (mapping_size == (uint64_t)granule) &&
                    (mapping_level == (level_count - 1U));
            } else if (mapping_kind == HCD_MMU_MAPPING_BLOCK) {
                mapping_shape_valid =
                    (mapping_size > (uint64_t)granule) &&
                    (mapping_level < (level_count - 1U));
            }
            if (!ordered ||
                (mapping_level >= level_count) ||
                !mapping_kind_valid ||
                !hcd_mmu_u64_is_power_of_two(mapping_size) ||
                (mapping_size < (uint64_t)granule) ||
                (mapping_size > (UINT64_C(1) << 52U)) ||
                (mapping_flags != 0U) ||
                (hcd_load_le32(buffer, mapping_offset + 28U) != 0U) ||
                (mapping_shape_valid == 0)) {
                return hcd_semantic_fail(
                    detail, HCD_E_SEMANTIC_INCONSISTENCY,
                    HCD_SECTION_MMU_MAPPING_SIZES, mapping_index,
                    mapping_offset);
            }
            if (mapping_kind == HCD_MMU_MAPPING_PAGE) {
                terminal_page_seen = 1U;
            }
            previous_mapping_cpu = mapping_cpu;
            previous_mapping_config = mapping_config;
            previous_mapping_level = mapping_level;
            previous_mapping_kind = mapping_kind;
            previous_mapping_size = mapping_size;
            mapping_index++;
            config_mapping_count++;
            if (config_mapping_count > 32U) {
                return hcd_semantic_fail(
                    detail, HCD_E_COUNT_LIMIT,
                    HCD_SECTION_MMU_MAPPING_SIZES, mapping_index - 1U,
                    mapping_offset + 24U);
            }
        }
        if ((config_mapping_count == 0U) || (terminal_page_seen == 0U)) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_MMU_MAPPING_SIZES,
                                     mapping_index, config_offset);
        }
        previous_cpu = cpu_id;
        previous_config = configuration_id;
    }
    if ((previous_cpu + 1U != cpu_count) ||
        (group_active_count != 1U) ||
        (mapping_index != mappings->entry_count)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_MMU_TRANSLATION_CONFIGS,
                                 HCD_ID_UNKNOWN, configs->offset);
    }
    HCD_ASSERT(configs->entry_count >= cpu_count);
    HCD_ASSERT(mapping_index == mappings->entry_count);
    return HCD_OK;
}

static enum hcd_error hcd_validate_mmu_arm64_contexts(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_validation_workspace *workspace,
    uint32_t cpu_count,
    struct hcd_validation_error *detail)
{
    const struct hcd_section_view *contexts = hcd_semantic_section(
        workspace, HCD_SECTION_MMU_ARM64_CONTEXTS);
    const struct hcd_section_view *configs = hcd_semantic_section(
        workspace, HCD_SECTION_MMU_TRANSLATION_CONFIGS);
    uint32_t context_index = 0U;
    uint32_t config_index = 0U;
    uint32_t group_cpu = HCD_ID_UNKNOWN;
    uint32_t group_el = 0U;
    uint32_t group_context_mask = 0U;
    uint32_t group_context_count = 0U;

    if (header->architecture != HCD_ARCH_ARM64) {
        if (contexts != NULL) {
            return hcd_semantic_fail(
                detail, HCD_E_SEMANTIC_INCONSISTENCY,
                HCD_SECTION_MMU_ARM64_CONTEXTS, HCD_ID_UNKNOWN,
                contexts->offset);
        }
        return HCD_OK;
    }
    if ((contexts == NULL) || (configs == NULL) ||
        (contexts->entry_count < cpu_count) ||
        (contexts->entry_count > HCD_MAX_MMU_CONFIGURATIONS)) {
        return hcd_semantic_fail(
            detail, HCD_E_SEMANTIC_INCONSISTENCY,
            HCD_SECTION_MMU_ARM64_CONTEXTS, HCD_ID_UNKNOWN, 0U);
    }

    for (config_index = 0U; config_index < configs->entry_count;
         ++config_index) {
        uint64_t config_offset = configs->offset +
            ((uint64_t)config_index * (uint64_t)configs->entry_size);
        uint32_t config_cpu = hcd_load_le32(buffer, config_offset + 4U);
        uint32_t regime = hcd_load_le32(buffer, config_offset + 12U);
        uint32_t flags = hcd_load_le32(buffer, config_offset + 16U);

        if (config_cpu >= cpu_count) {
            return hcd_semantic_fail(
                detail, HCD_E_SEMANTIC_INCONSISTENCY,
                HCD_SECTION_MMU_TRANSLATION_CONFIGS, config_index,
                config_offset + 4U);
        }
        if (config_cpu != group_cpu) {
            if ((group_cpu != HCD_ID_UNKNOWN) &&
                ((group_context_count == 0U) ||
                 ((group_el == HCD_MMU_EXCEPTION_LEVEL_EL1) &&
                  (group_context_mask != UINT32_C(3))) ||
                 ((group_el == HCD_MMU_EXCEPTION_LEVEL_EL2) &&
                  (group_context_mask != UINT32_C(1))))) {
                return hcd_semantic_fail(
                    detail, HCD_E_SEMANTIC_INCONSISTENCY,
                    HCD_SECTION_MMU_ARM64_CONTEXTS, group_cpu,
                    contexts->offset);
            }
            group_cpu = config_cpu;
            group_el = 0U;
            group_context_mask = 0U;
            group_context_count = 0U;
        }
        if (regime != HCD_MMU_REGIME_ARM64_STAGE1) {
            continue;
        }
        if (context_index >= contexts->entry_count) {
            return hcd_semantic_fail(
                detail, HCD_E_SEMANTIC_INCONSISTENCY,
                HCD_SECTION_MMU_ARM64_CONTEXTS, context_index,
                contexts->offset);
        }
        {
            uint64_t context_offset = contexts->offset +
                ((uint64_t)context_index * (uint64_t)contexts->entry_size);
            uint32_t context_cpu = hcd_load_le32(buffer, context_offset);
            uint32_t context_config =
                hcd_load_le32(buffer, context_offset + 4U);
            uint32_t context_kind =
                hcd_load_le32(buffer, context_offset + 8U);
            uint32_t context_flags =
                hcd_load_le32(buffer, context_offset + 12U);
            uint32_t exception_level =
                hcd_load_le32(buffer, context_offset + 16U);
            uint32_t reserved0 =
                hcd_load_le32(buffer, context_offset + 20U);
            uint32_t config_id = hcd_load_le32(buffer, config_offset + 8U);
            uint32_t config_el = hcd_load_le32(buffer, config_offset + 20U);
            uint32_t va_range = hcd_load_le32(buffer, config_offset + 44U);
            uint32_t expected_kind = 0U;
            uint32_t context_bit = 0U;
            uint32_t current =
                (flags & (uint32_t)HCD_MMU_CONFIG_F_CURRENT) != 0U;
            uint32_t context_enabled =
                (context_flags &
                 (uint32_t)HCD_MMU_ARM64_CONTEXT_F_TRANSLATION_ENABLED) != 0U;

            if (va_range == HCD_MMU_VA_RANGE_ARM64_LOWER) {
                expected_kind = HCD_MMU_ARM64_CONTEXT_TTBR0;
                context_bit = UINT32_C(1);
            } else if (va_range == HCD_MMU_VA_RANGE_ARM64_UPPER) {
                expected_kind = HCD_MMU_ARM64_CONTEXT_TTBR1;
                context_bit = UINT32_C(2);
            }
            if ((context_cpu != config_cpu) ||
                (context_config != config_id) ||
                (context_kind != expected_kind) ||
                (context_kind == 0U) ||
                ((context_flags &
                  ~(uint32_t)HCD_MMU_ARM64_CONTEXT_F_KNOWN_MASK) != 0U) ||
                (exception_level != config_el) ||
                ((exception_level != HCD_MMU_EXCEPTION_LEVEL_EL1) &&
                 (exception_level != HCD_MMU_EXCEPTION_LEVEL_EL2)) ||
                (reserved0 != 0U) ||
                ((current != 0U) &&
                 (context_enabled !=
                  ((flags &
                    (uint32_t)HCD_MMU_CONFIG_F_TRANSLATION_ENABLED) != 0U))) ||
                ((group_context_mask & context_bit) != 0U) ||
                ((group_el != 0U) && (group_el != exception_level))) {
                return hcd_semantic_fail(
                    detail, HCD_E_SEMANTIC_INCONSISTENCY,
                    HCD_SECTION_MMU_ARM64_CONTEXTS, context_index,
                    context_offset);
            }
            group_el = exception_level;
            group_context_mask |= context_bit;
            group_context_count++;
            context_index++;
        }
    }
    if ((group_cpu == HCD_ID_UNKNOWN) || (group_context_count == 0U) ||
        ((group_el == HCD_MMU_EXCEPTION_LEVEL_EL1) &&
         (group_context_mask != UINT32_C(3))) ||
        ((group_el == HCD_MMU_EXCEPTION_LEVEL_EL2) &&
         (group_context_mask != UINT32_C(1))) ||
        (context_index != contexts->entry_count)) {
        return hcd_semantic_fail(
            detail, HCD_E_SEMANTIC_INCONSISTENCY,
            HCD_SECTION_MMU_ARM64_CONTEXTS, context_index,
            contexts->offset);
    }
    HCD_ASSERT(context_index == contexts->entry_count);
    return HCD_OK;
}


static uint32_t hcd_interrupt_parent(
    const uint8_t *buffer,
    const struct hcd_section_view *controllers,
    uint32_t controller_id)
{
    uint64_t offset = controllers->offset +
        ((uint64_t)controller_id * (uint64_t)controllers->entry_size);

    HCD_ASSERT(controller_id < controllers->entry_count);
    HCD_ASSERT(controllers->entry_count <= HCD_MAX_INTERRUPT_CONTROLLERS);
    return hcd_load_le32(buffer, offset + 40U);
}

static int hcd_interrupt_parent_chain_acyclic(
    const uint8_t *buffer,
    const struct hcd_section_view *controllers,
    uint32_t start_id)
{
    uint32_t slow = start_id;
    uint32_t fast = start_id;
    uint32_t step = 0U;

    for (step = 0U; step < HCD_MAX_INTERRUPT_CONTROLLERS; ++step) {
        slow = hcd_interrupt_parent(buffer, controllers, slow);
        if (slow == HCD_ID_UNKNOWN) {
            return 1;
        }
        fast = hcd_interrupt_parent(buffer, controllers, fast);
        if (fast == HCD_ID_UNKNOWN) {
            return 1;
        }
        fast = hcd_interrupt_parent(buffer, controllers, fast);
        if (fast == HCD_ID_UNKNOWN) {
            return 1;
        }
        if (slow == fast) {
            return 0;
        }
    }
    HCD_ASSERT(step <= HCD_MAX_INTERRUPT_CONTROLLERS);
    HCD_ASSERT(controllers->entry_count <= HCD_MAX_INTERRUPT_CONTROLLERS);
    return 0;
}

static enum hcd_error hcd_validate_interrupt_record(
    const uint8_t *buffer,
    const struct hcd_section_view *controllers,
    const struct hcd_section_view *memory,
    uint32_t index,
    struct hcd_validation_error *detail)
{
    uint64_t offset = controllers->offset +
        ((uint64_t)index * (uint64_t)controllers->entry_size);
    uint32_t kind = hcd_load_le32(buffer, offset + 4U);
    uint32_t parent = hcd_load_le32(buffer, offset + 40U);
    uint64_t physical = hcd_load_le64(buffer, offset + 16U);
    uint64_t register_size = hcd_load_le64(buffer, offset + 24U);

    if ((hcd_load_le32(buffer, offset) != index) ||
        (kind < HCD_INTERRUPT_XAPIC) || (kind > HCD_INTERRUPT_IMSIC) ||
        (hcd_load_le32(buffer, offset + 8U) != 0U) ||
        ((parent != HCD_ID_UNKNOWN) &&
         (parent >= controllers->entry_count)) ||
        (hcd_load_le32(buffer, offset + 36U) == 0U) ||
        (hcd_load_le32(buffer, offset + 52U) > 3U) ||
        (hcd_load_le32(buffer, offset + 56U) > 3U) ||
        (hcd_load_le32(buffer, offset + 60U) != 0U) ||
        ((register_size == 0U) && (physical != 0U)) ||
        (physical > (UINT64_MAX - register_size))) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_INTERRUPT_CONTROLLERS,
                                 index, offset);
    }
    if ((register_size != 0U) &&
        (hcd_range_in_memory_types(buffer, memory, physical,
            register_size, (UINT32_C(1) << (uint32_t)HCD_MEMORY_MMIO)) == 0)) {
        return hcd_semantic_fail(detail, HCD_E_EXTERNAL_REFERENCE_INVALID,
                                 HCD_SECTION_INTERRUPT_CONTROLLERS,
                                 index, offset + 16U);
    }
    HCD_ASSERT(index < controllers->entry_count);
    HCD_ASSERT(index < controllers->entry_count);
    HCD_ASSERT(controllers->entry_count <= HCD_MAX_INTERRUPT_CONTROLLERS);
    return HCD_OK;
}

static enum hcd_error hcd_validate_interrupt_controllers(
    const uint8_t *buffer,
    const struct hcd_section_view *controllers,
    const struct hcd_section_view *memory,
    struct hcd_validation_error *detail)
{
    uint64_t previous_end = 0U;
    uint32_t index = 0U;
    enum hcd_error result = HCD_OK;

    if ((controllers == NULL) || (controllers->entry_count == 0U) ||
        (controllers->entry_count > HCD_MAX_INTERRUPT_CONTROLLERS)) {
        return hcd_semantic_fail(detail, HCD_E_COUNT_LIMIT,
                                 HCD_SECTION_INTERRUPT_CONTROLLERS,
                                 HCD_ID_UNKNOWN, 0U);
    }
    for (index = 0U; index < controllers->entry_count; ++index) {
        uint64_t offset = controllers->offset +
            ((uint64_t)index * (uint64_t)controllers->entry_size);
        uint64_t base = hcd_load_le32(buffer, offset + 32U);
        uint64_t count = hcd_load_le32(buffer, offset + 36U);
        uint64_t end = base + count;

        result = hcd_validate_interrupt_record(buffer, controllers, memory,
                                               index, detail);
        if (result != HCD_OK) {
            return result;
        }
        if ((end > (UINT64_C(1) << 32U)) ||
            ((index != 0U) && (base < previous_end))) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_INTERRUPT_CONTROLLERS,
                                     index, offset + 32U);
        }
        previous_end = end;
    }
    for (index = 0U; index < controllers->entry_count; ++index) {
        if (hcd_interrupt_parent_chain_acyclic(buffer, controllers, index) == 0) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_INTERRUPT_CONTROLLERS,
                                     index, controllers->offset);
        }
    }
    HCD_ASSERT(controllers->entry_count <= HCD_MAX_INTERRUPT_CONTROLLERS);
    HCD_ASSERT(previous_end <= (UINT64_C(1) << 32U));
    return HCD_OK;
}

static enum hcd_error hcd_validate_vectors(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_section_view *vectors,
    uint32_t cpu_count,
    struct hcd_validation_error *detail)
{
    uint32_t index = 0U;
    uint32_t previous_owner = 0U;
    uint32_t previous_vector = 0U;
    uint32_t vector_limit = (header->architecture == HCD_ARCH_AMD64) ?
                            256U : 16U;

    if ((vectors == NULL) || (vectors->entry_count == 0U)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_VECTOR_LAYOUT, HCD_ID_UNKNOWN, 0U);
    }
    for (index = 0U; index < vectors->entry_count; ++index) {
        uint64_t offset = vectors->offset +
            ((uint64_t)index * (uint64_t)vectors->entry_size);
        uint32_t vector_id = hcd_load_le32(buffer, offset);
        uint32_t kind = hcd_load_le32(buffer, offset + 4U);
        uint32_t owner = hcd_load_le32(buffer, offset + 12U);

        if ((vector_id >= vector_limit) ||
            (kind < HCD_VECTOR_EXCEPTION) || (kind > HCD_VECTOR_RESERVED) ||
            (hcd_load_le32(buffer, offset + 8U) != 0U) ||
            ((owner != HCD_ID_UNKNOWN) && (owner >= cpu_count)) ||
            (hcd_load_le32(buffer, offset + 20U) != 0U) ||
            (hcd_load_le64(buffer, offset + 24U) != 0U)) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_VECTOR_LAYOUT, index, offset);
        }
        if ((index != 0U) &&
            ((owner < previous_owner) ||
             ((owner == previous_owner) && (vector_id <= previous_vector)))) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_VECTOR_LAYOUT, index, offset);
        }
        previous_owner = owner;
        previous_vector = vector_id;
    }
    HCD_ASSERT(vector_limit <= 256U);
    HCD_ASSERT(vectors->entry_count <= HCD_MAX_TOTAL_SIZE /
               sizeof(struct hcd_vector_record));
    return HCD_OK;
}

static int hcd_interrupt_id_is_mapped(
    const uint8_t *buffer,
    const struct hcd_section_view *controllers,
    uint32_t interrupt_id)
{
    uint32_t low = 0U;
    uint32_t high = controllers->entry_count;
    uint32_t iteration = 0U;

    for (iteration = 0U; iteration < 32U; ++iteration) {
        uint32_t middle = 0U;
        uint64_t offset = 0U;
        uint64_t base = 0U;
        uint64_t count = 0U;

        if (low >= high) {
            break;
        }
        middle = low + ((high - low) / 2U);
        offset = controllers->offset +
            ((uint64_t)middle * (uint64_t)controllers->entry_size);
        base = hcd_load_le32(buffer, offset + 32U);
        count = hcd_load_le32(buffer, offset + 36U);
        if ((uint64_t)interrupt_id < base) {
            high = middle;
        } else if ((uint64_t)interrupt_id >= (base + count)) {
            low = middle + 1U;
        } else {
            return 1;
        }
    }
    HCD_ASSERT(low >= high);
    HCD_ASSERT(iteration <= 32U);
    return 0;
}

static enum hcd_error hcd_validate_timers(
    const uint8_t *buffer,
    const struct hcd_section_view *timers,
    const struct hcd_section_view *controllers,
    const struct hcd_section_view *memory,
    uint32_t cpu_count,
    struct hcd_validation_error *detail)
{
    uint32_t index = 0U;

    if ((timers == NULL) || (timers->entry_count == 0U)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_TIMER_CAPABILITIES,
                                 HCD_ID_UNKNOWN, 0U);
    }
    for (index = 0U; index < timers->entry_count; ++index) {
        uint64_t offset = timers->offset +
            ((uint64_t)index * (uint64_t)timers->entry_size);
        uint32_t kind = hcd_load_le32(buffer, offset + 4U);
        uint32_t width = hcd_load_le32(buffer, offset + 12U);
        uint32_t cpu_id = hcd_load_le32(buffer, offset + 36U);
        uint32_t interrupt_id = hcd_load_le32(buffer, offset + 32U);
        uint64_t physical = hcd_load_le64(buffer, offset + 24U);

        if ((hcd_load_le32(buffer, offset) != index) ||
            (hcd_load_le32(buffer, offset + 8U) != 0U) ||
            (kind < HCD_TIMER_TSC) || (kind > HCD_TIMER_PLATFORM) ||
            (width == 0U) || (width > 64U) ||
            (hcd_load_le64(buffer, offset + 16U) == 0U) ||
            ((cpu_id != HCD_ID_UNKNOWN) && (cpu_id >= cpu_count)) ||
            (hcd_load_le32(buffer, offset + 44U) != 0U)) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_TIMER_CAPABILITIES,
                                     index, offset);
        }
        if ((interrupt_id != HCD_ID_UNKNOWN) &&
            (hcd_interrupt_id_is_mapped(buffer, controllers,
                                        interrupt_id) == 0)) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_TIMER_CAPABILITIES,
                                     index, offset + 32U);
        }
        if ((physical != 0U) &&
            (hcd_range_in_memory_types(buffer, memory, physical, 1U,
                (UINT32_C(1) << (uint32_t)HCD_MEMORY_MMIO)) == 0)) {
            return hcd_semantic_fail(detail, HCD_E_EXTERNAL_REFERENCE_INVALID,
                                     HCD_SECTION_TIMER_CAPABILITIES,
                                     index, offset + 24U);
        }
    }
    HCD_ASSERT(timers->entry_count > 0U);
    HCD_ASSERT(cpu_count <= HCD_MAX_CPUS);
    return HCD_OK;
}

static enum hcd_error hcd_validate_boot_mode(
    const struct hcd_header *header,
    uint32_t protocol,
    uint32_t mode,
    uint32_t entry_state,
    uint64_t offset,
    struct hcd_validation_error *detail)
{
    if ((protocol < HCD_BOOT_MULTIBOOT2) ||
        (protocol > HCD_BOOT_UEFI_APPLICATION) ||
        (mode < HCD_BOOT_MODE_BIOS) || (mode > HCD_BOOT_MODE_UEFI)) {
        return hcd_semantic_fail(detail, HCD_E_PROFILE_MISMATCH,
                                 HCD_SECTION_BOOT_CONTEXT, 0U, offset);
    }
    if ((header->architecture == HCD_ARCH_AMD64) &&
        !((mode == HCD_BOOT_MODE_BIOS &&
           protocol == HCD_BOOT_MULTIBOOT2 &&
           entry_state == HCD_ENTRY_AMD64_PROTECTED32) ||
          (mode == HCD_BOOT_MODE_UEFI &&
           entry_state == HCD_ENTRY_AMD64_LONG64))) {
        return hcd_semantic_fail(detail, HCD_E_PROFILE_MISMATCH,
                                 HCD_SECTION_BOOT_CONTEXT, 0U, offset);
    }
    if ((header->architecture == HCD_ARCH_ARM64) &&
        !((mode == HCD_BOOT_MODE_UEFI) &&
          (protocol == HCD_BOOT_UEFI_APPLICATION) &&
          ((entry_state == HCD_ENTRY_ARM64_EL1) ||
           (entry_state == HCD_ENTRY_ARM64_EL2)))) {
        return hcd_semantic_fail(detail, HCD_E_PROFILE_MISMATCH,
                                 HCD_SECTION_BOOT_CONTEXT, 0U, offset);
    }
    HCD_ASSERT(protocol <= HCD_BOOT_UEFI_APPLICATION);
    HCD_ASSERT(mode <= HCD_BOOT_MODE_UEFI);
    return HCD_OK;
}

static enum hcd_error hcd_validate_boot_ranges(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_section_view *boot,
    const struct hcd_section_view *memory,
    struct hcd_validation_error *detail)
{
    uint64_t offset = boot->offset;
    uint64_t loader_base = hcd_load_le64(buffer, offset + 16U);
    uint64_t loader_size = hcd_load_le64(buffer, offset + 24U);
    uint64_t hcd_base = hcd_load_le64(buffer, offset + 32U);
    uint64_t reserved_base = hcd_load_le64(buffer, offset + 40U);
    uint64_t reserved_size = hcd_load_le64(buffer, offset + 48U);
    uint32_t reserved_types = (UINT32_C(1) << (uint32_t)HCD_MEMORY_RESERVED) |
        (UINT32_C(1) << (uint32_t)HCD_MEMORY_FIRMWARE_RUNTIME) |
        (UINT32_C(1) << (uint32_t)HCD_MEMORY_BOOTSTRAP);

    if ((loader_size == 0U) || (hcd_base == 0U) ||
        (reserved_size < header->total_size) ||
        (loader_base > (UINT64_MAX - loader_size)) ||
        (reserved_base > (UINT64_MAX - reserved_size)) ||
        (hcd_base < reserved_base) ||
        (hcd_base > (UINT64_MAX - header->total_size)) ||
        ((hcd_base + header->total_size) > (reserved_base + reserved_size))) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_BOOT_CONTEXT, 0U, offset + 16U);
    }
    if ((hcd_range_in_memory_types(buffer, memory, loader_base, loader_size,
                                   reserved_types) == 0) ||
        (hcd_range_in_memory_types(buffer, memory, reserved_base, reserved_size,
                                   reserved_types) == 0)) {
        return hcd_semantic_fail(detail, HCD_E_MEMORY_MAP_INVALID,
                                 HCD_SECTION_BOOT_CONTEXT, 0U, offset + 16U);
    }
    HCD_ASSERT(reserved_size >= header->total_size);
    HCD_ASSERT(hcd_base >= reserved_base);
    return HCD_OK;
}

static enum hcd_error hcd_validate_boot_context(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_section_view *boot,
    const struct hcd_section_view *memory,
    enum hcd_validation_profile profile,
    struct hcd_validation_error *detail)
{
    uint64_t offset = boot->offset;
    uint32_t protocol = hcd_load_le32(buffer, offset);
    uint32_t mode = hcd_load_le32(buffer, offset + 4U);
    uint32_t entry_state = hcd_load_le32(buffer, offset + 8U);
    enum hcd_error result = HCD_OK;

    if (profile == HCD_PROFILE_RECOVERY) {
        return HCD_OK;
    }
    if ((hcd_load_le32(buffer, offset + 12U) != 0U) ||
        (hcd_load_le32(buffer, offset + 60U) != 0U)) {
        return hcd_semantic_fail(detail, HCD_E_RESERVED_NONZERO,
                                 HCD_SECTION_BOOT_CONTEXT, 0U, offset + 12U);
    }
    result = hcd_validate_boot_mode(header, protocol, mode,
                                    entry_state, offset, detail);
    if (result != HCD_OK) {
        return result;
    }
    if (mode == HCD_BOOT_MODE_UEFI) {
        if (((header->flags & HCD_F_FIRMWARE_BOOT_SERVICES_ACTIVE) != 0U) ||
            ((header->flags & HCD_F_EFI_EXIT_BOOT_SERVICES_DONE) == 0U)) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_BOOT_CONTEXT, 0U, 32U);
        }
    } else if ((header->flags &
                (HCD_F_FIRMWARE_BOOT_SERVICES_ACTIVE |
                 HCD_F_EFI_EXIT_BOOT_SERVICES_DONE)) != 0U) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_BOOT_CONTEXT, 0U, 32U);
    }
    return hcd_validate_boot_ranges(buffer, header, boot, memory, detail);
}


struct hcd_raw_order_state {
    uint32_t cpu_id;
    uint32_t register_space;
    uint32_t selector;
    uint32_t subselector;
    uint32_t has_previous;
};

static enum hcd_error hcd_validate_raw_record(
    const uint8_t *buffer,
    const struct hcd_section_view *raw,
    uint32_t index,
    uint32_t cpu_count,
    struct hcd_raw_order_state *order,
    struct hcd_validation_error *detail)
{
    uint64_t offset = raw->offset +
        ((uint64_t)index * (uint64_t)raw->entry_size);
    uint32_t cpu_id = hcd_load_le32(buffer, offset);
    uint32_t space = hcd_load_le32(buffer, offset + 4U);
    uint32_t selector = hcd_load_le32(buffer, offset + 8U);
    uint32_t subselector = hcd_load_le32(buffer, offset + 12U);
    uint32_t state = hcd_load_le32(buffer, offset + 24U);
    uint64_t value = hcd_load_le64(buffer, offset + 16U);

    if ((cpu_id >= cpu_count) || (space == 0U) ||
        (state < HCD_RAW_ACCESS_READ) || (state > HCD_RAW_ACCESS_FAULT) ||
        (hcd_load_le32(buffer, offset + 28U) != 0U) ||
        ((state != HCD_RAW_ACCESS_READ) && (value != 0U))) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_CPU_RAW_REGISTERS, index, offset);
    }
    if ((order->has_previous != 0U) &&
        ((cpu_id < order->cpu_id) ||
         ((cpu_id == order->cpu_id) && (space < order->register_space)) ||
         ((cpu_id == order->cpu_id) && (space == order->register_space) &&
          (selector < order->selector)) ||
         ((cpu_id == order->cpu_id) && (space == order->register_space) &&
          (selector == order->selector) &&
          (subselector <= order->subselector)))) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_CPU_RAW_REGISTERS, index, offset);
    }
    order->cpu_id = cpu_id;
    order->register_space = space;
    order->selector = selector;
    order->subselector = subselector;
    order->has_previous = 1U;
    HCD_ASSERT(index < raw->entry_count);
    HCD_ASSERT(order->has_previous == 1U);
    return HCD_OK;
}

static enum hcd_error hcd_validate_raw_sequence(
    const uint8_t *buffer,
    const struct hcd_section_view *raw,
    uint32_t cpu_count,
    struct hcd_validation_error *detail)
{
    struct hcd_raw_order_state order;
    order.cpu_id = 0U;
    order.register_space = 0U;
    order.selector = 0U;
    order.subselector = 0U;
    order.has_previous = 0U;
    uint32_t index = 0U;
    enum hcd_error result = HCD_OK;

    if (raw == NULL) {
        return HCD_OK;
    }
    for (index = 0U; index < raw->entry_count; ++index) {
        result = hcd_validate_raw_record(buffer, raw, index,
                                         cpu_count, &order, detail);
        if (result != HCD_OK) {
            return result;
        }
    }
    HCD_ASSERT(order.has_previous <= 1U);
    HCD_ASSERT(raw->entry_count <=
               HCD_MAX_TOTAL_SIZE / sizeof(struct hcd_cpu_raw_register_record));
    return HCD_OK;
}

static enum hcd_error hcd_validate_raw_links(
    const uint8_t *buffer,
    const struct hcd_section_view *raw,
    const struct hcd_section_view *cpus,
    uint32_t cpu_count,
    struct hcd_validation_error *detail)
{
    uint32_t raw_cursor = 0U;
    uint32_t cpu_id = 0U;
    uint32_t raw_count = (raw == NULL) ? 0U : raw->entry_count;

    for (cpu_id = 0U; cpu_id < cpu_count; ++cpu_id) {
        uint32_t expected_first = HCD_ID_UNKNOWN;
        uint64_t cpu_offset = cpus->offset +
            ((uint64_t)cpu_id * (uint64_t)cpus->entry_size);
        uint32_t actual_first = hcd_load_le32(buffer, cpu_offset + 272U);

        if (raw_cursor < raw_count) {
            uint64_t raw_offset = raw->offset +
                ((uint64_t)raw_cursor * (uint64_t)raw->entry_size);
            if (hcd_load_le32(buffer, raw_offset) == cpu_id) {
                expected_first = raw_cursor;
                while (raw_cursor < raw_count) {
                    raw_offset = raw->offset +
                        ((uint64_t)raw_cursor * (uint64_t)raw->entry_size);
                    if (hcd_load_le32(buffer, raw_offset) != cpu_id) {
                        break;
                    }
                    raw_cursor++;
                }
            }
        }
        if (actual_first != expected_first) {
            return hcd_semantic_fail(detail, HCD_E_CPU_FEATURE_SET_INVALID,
                                     HCD_SECTION_CPU_RECORDS, cpu_id,
                                     cpu_offset + 272U);
        }
    }
    HCD_ASSERT(raw_cursor == raw_count);
    HCD_ASSERT(cpu_count <= HCD_MAX_CPUS);
    return HCD_OK;
}

static enum hcd_error hcd_validate_cpu_raw_registers(
    const uint8_t *buffer,
    const struct hcd_section_view *raw,
    const struct hcd_section_view *cpus,
    uint32_t cpu_count,
    struct hcd_validation_error *detail)
{
    enum hcd_error result = hcd_validate_raw_sequence(
        buffer, raw, cpu_count, detail);

    if (result != HCD_OK) {
        return result;
    }
    return hcd_validate_raw_links(buffer, raw, cpus, cpu_count, detail);
}

static enum hcd_error hcd_validate_cache(
    const uint8_t *buffer,
    const struct hcd_section_view *cache,
    uint32_t cpu_count,
    struct hcd_validation_error *detail)
{
    uint32_t index = 0U;

    if (cache == NULL) {
        return HCD_OK;
    }
    for (index = 0U; index < cache->entry_count; ++index) {
        uint64_t offset = cache->offset +
            ((uint64_t)index * (uint64_t)cache->entry_size);
        uint32_t cpu_id = hcd_load_le32(buffer, offset);
        uint32_t level = hcd_load_le16(buffer, offset + 8U);
        uint32_t type = hcd_load_le16(buffer, offset + 10U);
        uint64_t size = hcd_load_le64(buffer, offset + 16U);
        uint32_t line_size = hcd_load_le32(buffer, offset + 24U);
        uint32_t ways = hcd_load_le32(buffer, offset + 28U);
        uint32_t sets = hcd_load_le32(buffer, offset + 32U);

        if (((cpu_id != HCD_ID_UNKNOWN) && (cpu_id >= cpu_count)) ||
            (hcd_load_le32(buffer, offset + 4U) != index) ||
            (level == 0U) || (level > 4U) ||
            (type < 1U) || (type > 3U) || (size == 0U) ||
            (hcd_load_le32(buffer, offset + 12U) != 0U) ||
            ((line_size != 0U) &&
             ((line_size & (line_size - 1U)) != 0U)) ||
            (hcd_load_le32(buffer, offset + 44U) != 0U)) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_CACHE, index, offset);
        }
        if ((line_size != 0U) && (ways != 0U) && (sets != 0U)) {
            uint64_t line_ways = (uint64_t)line_size * (uint64_t)ways;
            uint64_t minimum = 0U;

            if ((line_ways / (uint64_t)line_size != ways) ||
                ((uint64_t)sets > (UINT64_MAX / line_ways))) {
                return hcd_semantic_fail(detail, HCD_E_INTEGER_OVERFLOW,
                                         HCD_SECTION_CACHE, index, offset + 24U);
            }
            minimum = line_ways * (uint64_t)sets;
            if (minimum > size) {
                return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                         HCD_SECTION_CACHE, index, offset + 24U);
            }
        }
    }
    HCD_ASSERT(cache->entry_count <=
               HCD_MAX_TOTAL_SIZE / sizeof(struct hcd_cache_record));
    HCD_ASSERT(cpu_count <= HCD_MAX_CPUS);
    return HCD_OK;
}

static enum hcd_error hcd_validate_virtualization(
    const uint8_t *buffer,
    const struct hcd_header *header,
    const struct hcd_section_view *virtualization,
    uint32_t cpu_count,
    struct hcd_validation_error *detail)
{
    uint32_t index = 0U;

    if (virtualization == NULL) {
        return HCD_OK;
    }
    if ((virtualization->entry_count != 0U) &&
        (virtualization->entry_count != cpu_count)) {
        return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                 HCD_SECTION_VIRTUALIZATION,
                                 HCD_ID_UNKNOWN, virtualization->offset);
    }
    for (index = 0U; index < virtualization->entry_count; ++index) {
        uint64_t offset = virtualization->offset +
            ((uint64_t)index * (uint64_t)virtualization->entry_size);
        uint32_t supported = hcd_load_le32(buffer, offset + 4U);
        uint32_t enabled = hcd_load_le32(buffer, offset + 8U);
        uint32_t hypervisor = hcd_load_le32(buffer, offset + 20U);

        if ((hcd_load_le32(buffer, offset) != header->architecture) ||
            (hcd_load_le32(buffer, offset + 28U) != index) ||
            (hcd_load_le32(buffer, offset + 12U) != 0U) ||
            (hypervisor > 1U) || ((enabled & ~supported) != 0U)) {
            return hcd_semantic_fail(detail, HCD_E_SEMANTIC_INCONSISTENCY,
                                     HCD_SECTION_VIRTUALIZATION, index, offset);
        }
    }
    HCD_ASSERT(virtualization->entry_count <= HCD_MAX_CPUS);
    HCD_ASSERT(cpu_count <= HCD_MAX_CPUS);
    return HCD_OK;
}

static enum hcd_error hcd_validate_firmware_references(
    const uint8_t *buffer,
    const struct hcd_section_view *references,
    const struct hcd_section_view *memory,
    struct hcd_validation_error *detail)
{
    uint32_t index = 0U;
    uint32_t allowed_types = (UINT32_C(1) << (uint32_t)HCD_MEMORY_RESERVED) |
        (UINT32_C(1) << (uint32_t)HCD_MEMORY_FIRMWARE_RUNTIME) |
        (UINT32_C(1) << (uint32_t)HCD_MEMORY_ACPI_RECLAIMABLE) |
        (UINT32_C(1) << (uint32_t)HCD_MEMORY_ACPI_NVS) |
        (UINT32_C(1) << (uint32_t)HCD_MEMORY_BOOTSTRAP);

    if (references == NULL) {
        return HCD_OK;
    }
    for (index = 0U; index < references->entry_count; ++index) {
        uint64_t offset = references->offset +
            ((uint64_t)index * (uint64_t)references->entry_size);
        uint32_t kind = hcd_load_le32(buffer, offset);
        uint32_t memory_type = hcd_load_le32(buffer, offset + 12U);
        uint32_t declared_type_mask = (memory_type < 32U) ?
            (UINT32_C(1) << memory_type) : 0U;
        uint64_t base = hcd_load_le64(buffer, offset + 16U);
        uint64_t length = hcd_load_le64(buffer, offset + 24U);

        if ((kind < 1U) || (kind > 5U) ||
            (hcd_load_le32(buffer, offset + 8U) != 0U) ||
            (memory_type < HCD_MEMORY_USABLE) ||
            (memory_type > HCD_MEMORY_UNACCEPTED) ||
            ((allowed_types & declared_type_mask) == 0U) ||
            (length == 0U) || (base > (UINT64_MAX - length)) ||
            (hcd_load_le32(buffer, offset + 44U) != 0U) ||
            (hcd_range_in_memory_types(buffer, memory, base, length,
                                       declared_type_mask) == 0)) {
            return hcd_semantic_fail(detail, HCD_E_EXTERNAL_REFERENCE_INVALID,
                                     HCD_SECTION_FIRMWARE_REFERENCES,
                                     index, offset);
        }
    }
    HCD_ASSERT(references->entry_count <=
               HCD_MAX_TOTAL_SIZE / sizeof(struct hcd_firmware_reference));
    HCD_ASSERT(memory->entry_count > 0U);
    return HCD_OK;
}

static enum hcd_error hcd_validate_platform_devices(
    const uint8_t *buffer,
    const struct hcd_section_view *devices,
    const struct hcd_section_view *memory,
    struct hcd_validation_error *detail)
{
    uint32_t index = 0U;
    uint32_t mmio_mask = UINT32_C(1) << (uint32_t)HCD_MEMORY_MMIO;

    if (devices == NULL) {
        return HCD_OK;
    }
    for (index = 0U; index < devices->entry_count; ++index) {
        uint64_t offset = devices->offset +
            ((uint64_t)index * (uint64_t)devices->entry_size);
        uint64_t base = hcd_load_le64(buffer, offset + 16U);
        uint64_t length = hcd_load_le64(buffer, offset + 24U);

        if ((hcd_load_le32(buffer, offset) != index) ||
            (hcd_load_le32(buffer, offset + 4U) < HCD_DEVICE_PCI) ||
            (hcd_load_le32(buffer, offset + 4U) > HCD_DEVICE_OTHER) ||
            (hcd_load_le32(buffer, offset + 8U) != 0U) ||
            (base > (UINT64_MAX - length)) ||
            (hcd_load_le32(buffer, offset + 56U) != 0U) ||
            (hcd_load_le32(buffer, offset + 60U) != 0U)) {
            return hcd_semantic_fail(detail, HCD_E_EXTERNAL_REFERENCE_INVALID,
                                     HCD_SECTION_PLATFORM_DEVICES,
                                     index, offset);
        }
        if ((length != 0U) &&
            (hcd_range_in_memory_types(buffer, memory, base, length,
                                      mmio_mask) == 0)) {
            return hcd_semantic_fail(detail, HCD_E_EXTERNAL_REFERENCE_INVALID,
                                     HCD_SECTION_PLATFORM_DEVICES,
                                     index, offset + 16U);
        }
    }
    HCD_ASSERT(devices->entry_count <=
               HCD_MAX_TOTAL_SIZE / sizeof(struct hcd_platform_device_record));
    HCD_ASSERT(memory->entry_count > 0U);
    return HCD_OK;
}


static enum hcd_error hcd_validate_optional_sections(
    const uint8_t *buffer,
    const struct hcd_header *header,
    struct hcd_validation_workspace *workspace,
    const struct hcd_section_view *cpus,
    const struct hcd_section_view *memory,
    uint32_t cpu_count,
    struct hcd_validation_error *detail)
{
    const struct hcd_section_view *raw = hcd_semantic_section(
        workspace, HCD_SECTION_CPU_RAW_REGISTERS);
    const struct hcd_section_view *cache = hcd_semantic_section(
        workspace, HCD_SECTION_CACHE);
    const struct hcd_section_view *virtualization = hcd_semantic_section(
        workspace, HCD_SECTION_VIRTUALIZATION);
    const struct hcd_section_view *references = hcd_semantic_section(
        workspace, HCD_SECTION_FIRMWARE_REFERENCES);
    const struct hcd_section_view *devices = hcd_semantic_section(
        workspace, HCD_SECTION_PLATFORM_DEVICES);
    enum hcd_error result = HCD_OK;

    result = hcd_validate_cpu_raw_registers(buffer, raw, cpus,
                                            cpu_count, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_validate_cache(buffer, cache, cpu_count, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_validate_virtualization(buffer, header, virtualization,
                                         cpu_count, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_validate_firmware_references(buffer, references,
                                              memory, detail);
    if (result != HCD_OK) {
        return result;
    }
    return hcd_validate_platform_devices(buffer, devices, memory, detail);
}

static enum hcd_error hcd_validate_core_platform(
    const uint8_t *buffer,
    const struct hcd_header *header,
    struct hcd_validation_workspace *workspace,
    const struct hcd_section_view *cpus,
    const struct hcd_section_view *memory,
    uint32_t cpu_count,
    struct hcd_validation_error *detail)
{
    const struct hcd_section_view *topology = hcd_semantic_section(
        workspace, HCD_SECTION_TOPOLOGY);
    const struct hcd_section_view *mmu = hcd_semantic_section(
        workspace, HCD_SECTION_MMU_CAPABILITIES);
    const struct hcd_section_view *controllers = hcd_semantic_section(
        workspace, HCD_SECTION_INTERRUPT_CONTROLLERS);
    const struct hcd_section_view *vectors = hcd_semantic_section(
        workspace, HCD_SECTION_VECTOR_LAYOUT);
    const struct hcd_section_view *timers = hcd_semantic_section(
        workspace, HCD_SECTION_TIMER_CAPABILITIES);
    const struct hcd_section_view *boot = hcd_semantic_section(
        workspace, HCD_SECTION_BOOT_CONTEXT);
    enum hcd_error result = HCD_OK;

    result = hcd_validate_topology(buffer, topology, cpus, cpu_count,
                                   workspace, detail);
    if (result != HCD_OK) {
        return result;
    }
    if (header->version_major == HCD_VERSION_MAJOR_V1) {
        result = hcd_validate_mmu(buffer, header, mmu, cpu_count, detail);
        if (result != HCD_OK) {
            return result;
        }
    }
    result = hcd_validate_interrupt_controllers(buffer, controllers, memory, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_validate_vectors(buffer, header, vectors, cpu_count, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_validate_timers(buffer, timers, controllers, memory, cpu_count, detail);
    if (result != HCD_OK) {
        return result;
    }
    return hcd_validate_boot_context(buffer, header, boot, memory,
                                     HCD_PROFILE_CORE, detail);
}


static enum hcd_error hcd_validate_cpu_base(
    const uint8_t *buffer,
    const struct hcd_header *header,
    struct hcd_validation_workspace *workspace,
    const struct hcd_section_view *summary,
    const struct hcd_section_view *cpus,
    const struct hcd_section_view *catalog,
    const struct hcd_section_view *memory,
    uint32_t *cpu_count,
    uint32_t *boot_cpu_id,
    struct hcd_validation_error *detail)
{
    uint64_t catalog_bits[HCD_FEATURE_WORDS];
    uint32_t word = 0U;
    enum hcd_error result = HCD_OK;

    for (word = 0U; word < HCD_FEATURE_WORDS; ++word) {
        catalog_bits[word] = 0U;
    }
    result = hcd_validate_cpu_catalog(
        buffer, header, catalog, catalog_bits, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_validate_cpu_summary(buffer, header, summary, cpus,
                                      cpu_count, boot_cpu_id, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_validate_cpu_records(buffer, header, workspace, summary,
                                      cpus, catalog_bits, *cpu_count,
                                      *boot_cpu_id, detail);
    if (result != HCD_OK) {
        return result;
    }
    result = hcd_validate_memory_map(buffer, memory, detail);
    if (result != HCD_OK) {
        return result;
    }
    return hcd_validate_optional_sections(buffer, header, workspace,
                                          cpus, memory, *cpu_count, detail);
}

enum hcd_error hcd_validate_semantics(
    const uint8_t *buffer,
    const struct hcd_header *header,
    struct hcd_validation_workspace *workspace,
    enum hcd_validation_profile profile,
    struct hcd_validation_error *detail)
{
    const struct hcd_section_view *summary = hcd_semantic_section(
        workspace, HCD_SECTION_CPU_SUMMARY);
    const struct hcd_section_view *cpus = hcd_semantic_section(
        workspace, HCD_SECTION_CPU_RECORDS);
    const struct hcd_section_view *catalog = hcd_semantic_section(
        workspace, HCD_SECTION_CPU_FEATURE_CATALOG);
    const struct hcd_section_view *memory = hcd_semantic_section(
        workspace, HCD_SECTION_MEMORY_MAP);
    const struct hcd_section_view *mmu = hcd_semantic_section(
        workspace, HCD_SECTION_MMU_CAPABILITIES);
    uint32_t cpu_count = 0U;
    uint32_t boot_cpu_id = 0U;
    enum hcd_error result = HCD_OK;

    if ((buffer == NULL) || (header == NULL) || (workspace == NULL)) {
        return hcd_semantic_fail(detail, HCD_E_NULL_INPUT,
                                 0U, HCD_ID_UNKNOWN, 0U);
    }
    if ((summary == NULL) || (cpus == NULL) ||
        (catalog == NULL) || (memory == NULL)) {
        return hcd_semantic_fail(detail, HCD_E_BAD_SECTION_DIRECTORY,
                                 0U, HCD_ID_UNKNOWN, 0U);
    }
    HCD_ASSERT(buffer != NULL);
    HCD_ASSERT(header->total_size <= HCD_MAX_TOTAL_SIZE);
    result = hcd_validate_cpu_base(buffer, header, workspace, summary,
                                   cpus, catalog, memory, &cpu_count,
                                   &boot_cpu_id, detail);
    if (result != HCD_OK) {
        return result;
    }
    if (header->version_major == HCD_VERSION_MAJOR_V2) {
        result = hcd_validate_mmu(buffer, header, mmu, cpu_count, detail);
        if (result != HCD_OK) {
            return result;
        }
        result = hcd_validate_mmu_v2(buffer, header, workspace, mmu,
                                     cpu_count, detail);
        if (result != HCD_OK) {
            return result;
        }
        result = hcd_validate_mmu_arm64_contexts(
            buffer, header, workspace, cpu_count, detail);
        if (result != HCD_OK) {
            return result;
        }
    }
    if (profile == HCD_PROFILE_CORE) {
        result = hcd_validate_core_platform(buffer, header, workspace,
                                            cpus, memory, cpu_count, detail);
        if (result != HCD_OK) {
            return result;
        }
    } else if (profile != HCD_PROFILE_RECOVERY) {
        return hcd_semantic_fail(detail, HCD_E_PROFILE_MISMATCH,
                                 0U, HCD_ID_UNKNOWN, 0U);
    }
    HCD_ASSERT(cpu_count > 0U);
    HCD_ASSERT(boot_cpu_id < cpu_count);
    return HCD_OK;
}
