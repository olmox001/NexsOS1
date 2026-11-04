#include <kernel/fdt_parser.h>

#define FDT_HEADER_V16_SIZE 36U
#define FDT_HEADER_V17_SIZE 40U

#define FDT_BEGIN_NODE 1U
#define FDT_END_NODE   2U
#define FDT_PROP       3U
#define FDT_NOP        4U
#define FDT_END        9U

#define FDT_MAX_NODES 65536U
#define FDT_MAX_PROPERTIES 65536U

struct fdt_node_ctx {
    const uint8_t *name;
    size_t name_len;
    const uint8_t *reg_data;
    uint32_t reg_len;
    const uint8_t *size_data;
    uint32_t size_len;
    const uint8_t *alignment_data;
    uint32_t alignment_len;
    uint32_t reg_addr_cells;
    uint32_t reg_size_cells;
    uint32_t child_addr_cells;
    uint32_t child_size_cells;
    uint8_t parent_is_root;
    uint8_t parent_is_reserved;
    uint8_t parent_is_cpus;
    uint8_t name_is_memory;
    uint8_t name_is_reserved_root;
    uint8_t name_is_reserved_child;
    uint8_t name_is_cpu;
    uint8_t disabled;
    uint8_t device_type_memory;
    uint8_t device_type_cpu;
    uint8_t reg_seen;
    uint8_t size_seen;
    uint8_t alignment_seen;
    uint8_t ranges_seen;
    uint8_t ranges_empty;
    uint8_t children_seen;
    uint8_t status_seen;
    uint8_t address_cells_seen;
    uint8_t size_cells_seen;
    uint8_t device_type_seen;
    uint8_t no_map_seen;
};

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint64_t read_be64(const uint8_t *p) {
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a > UINT64_MAX - b)
        return 1;
    *out = a + b;
    return 0;
}

static int range_inside(uint32_t offset, uint32_t size, uint32_t total) {
    return offset <= total && size <= total - offset;
}

static int ranges_overlap(uint32_t a, uint32_t a_size,
                          uint32_t b, uint32_t b_size) {
    if (a_size == 0 || b_size == 0)
        return 0;
    return a < b + b_size && b < a + a_size;
}

static uint32_t align4_u32(uint32_t value, int *overflow) {
    if (value > UINT32_MAX - 3U) {
        *overflow = 1;
        return 0;
    }
    return (value + 3U) & ~3U;
}

static int string_terminated(const uint8_t *data, size_t size, size_t offset) {
    if (!data || offset >= size)
        return 0;
    for (size_t i = offset; i < size; i++) {
        if (data[i] == 0)
            return 1;
    }
    return 0;
}

static int string_equals(const uint8_t *data, size_t size,
                         const char *literal, size_t literal_len) {
    if (!data || size != literal_len + 1U)
        return 0;
    for (size_t i = 0; i < literal_len; i++) {
        if (data[i] != (uint8_t)literal[i])
            return 0;
    }
    return data[literal_len] == 0;
}

static int name_equals(const uint8_t *name, size_t len,
                       const char *literal, size_t literal_len) {
    if (!name || len != literal_len)
        return 0;
    for (size_t i = 0; i < literal_len; i++) {
        if (name[i] != (uint8_t)literal[i])
            return 0;
    }
    return 1;
}

static int name_has_prefix(const uint8_t *name, size_t len,
                           const char *prefix, size_t prefix_len) {
    if (!name || len < prefix_len)
        return 0;
    for (size_t i = 0; i < prefix_len; i++) {
        if (name[i] != (uint8_t)prefix[i])
            return 0;
    }
    return 1;
}

static int append_region(struct fdt_parser_region *regions, size_t capacity,
                         size_t *count, uint64_t base, uint64_t size,
                         uint32_t type) {
    uint64_t end;
    if (!regions || !count || size == 0 ||
        add_overflow_u64(base, size, &end))
        return FDT_PARSER_INVALID;
    (void)end;
    if (*count >= capacity)
        return FDT_PARSER_NO_SPACE;
    regions[*count].base = base;
    regions[*count].size = size;
    regions[*count].type = type;
    (*count)++;
    return FDT_PARSER_OK;
}

static int sort_merge_regions(struct fdt_parser_region *regions,
                              size_t *count) {
    if (!regions || !count)
        return FDT_PARSER_INVALID;

    for (size_t i = 1; i < *count; i++) {
        struct fdt_parser_region item = regions[i];
        size_t j = i;
        while (j > 0 && regions[j - 1].base > item.base) {
            regions[j] = regions[j - 1];
            j--;
        }
        regions[j] = item;
    }

    if (*count < 2)
        return FDT_PARSER_OK;

    size_t out = 0;
    for (size_t i = 0; i < *count; i++) {
        uint64_t current_end;
        if (regions[i].size == 0 ||
            add_overflow_u64(regions[i].base, regions[i].size,
                             &current_end))
            return FDT_PARSER_INVALID;

        if (out == 0) {
            regions[out++] = regions[i];
            continue;
        }

        uint64_t previous_end;
        if (add_overflow_u64(regions[out - 1].base,
                             regions[out - 1].size, &previous_end))
            return FDT_PARSER_INVALID;

        if (regions[i].base <= previous_end) {
            if (current_end > previous_end)
                regions[out - 1].size =
                    current_end - regions[out - 1].base;
        } else {
            regions[out++] = regions[i];
        }
    }

    *count = out;
    return FDT_PARSER_OK;
}

static int parse_cells(const uint8_t *data, size_t length,
                       uint32_t cells, uint64_t *value) {
    if (!data || !value || cells == 0 || cells > 2 ||
        length < (size_t)cells * 4U)
        return FDT_PARSER_INVALID;

    uint64_t result = 0;
    for (uint32_t i = 0; i < cells; i++)
        result = (result << 32) | read_be32(data + (size_t)i * 4U);
    *value = result;
    return FDT_PARSER_OK;
}

static int append_reg_tuples(const uint8_t *data, uint32_t length,
                             uint32_t address_cells, uint32_t size_cells,
                             uint32_t type,
                             struct fdt_parser_region *regions,
                             size_t capacity, size_t *count) {
    if (!data || address_cells == 0 || address_cells > 2 ||
        size_cells == 0 || size_cells > 2)
        return FDT_PARSER_INVALID;

    size_t tuple_cells = (size_t)address_cells + (size_t)size_cells;
    size_t tuple_size = tuple_cells * 4U;
    if (tuple_size == 0 || length == 0 ||
        ((size_t)length % tuple_size) != 0)
        return FDT_PARSER_INVALID;

    for (size_t offset = 0; offset < (size_t)length;
         offset += tuple_size) {
        uint64_t base, size;
        if (parse_cells(data + offset, length - (uint32_t)offset,
                        address_cells, &base) != FDT_PARSER_OK ||
            parse_cells(data + offset + (size_t)address_cells * 4U,
                        length - (uint32_t)offset -
                            (uint32_t)((size_t)address_cells * 4U),
                        size_cells, &size) != FDT_PARSER_OK)
            return FDT_PARSER_INVALID;

        int status = append_region(regions, capacity, count,
                                   base, size, type);
        if (status != FDT_PARSER_OK)
            return status;
    }
    return FDT_PARSER_OK;
}

static int validate_reserve_map(const uint8_t *blob, uint32_t total_size,
                                uint32_t reserve_offset,
                                uint32_t *reserve_end) {
    struct fdt_parser_region entries[FDT_PARSER_MAX_RESERVE_ENTRIES];
    size_t count = 0;
    uint32_t cursor = reserve_offset;

    for (uint32_t i = 0; i <= FDT_PARSER_MAX_RESERVE_ENTRIES; i++) {
        if (!range_inside(cursor, 16U, total_size))
            return FDT_PARSER_INVALID;

        uint64_t base = read_be64(blob + cursor);
        uint64_t size = read_be64(blob + cursor + 8U);
        cursor += 16U;

        if (base == 0 && size == 0) {
            *reserve_end = cursor;
            return FDT_PARSER_OK;
        }
        if (size == 0)
            return FDT_PARSER_INVALID;

        uint64_t end;
        if (add_overflow_u64(base, size, &end))
            return FDT_PARSER_INVALID;

        for (size_t j = 0; j < count; j++) {
            uint64_t other_end;
            if (add_overflow_u64(entries[j].base, entries[j].size,
                                 &other_end))
                return FDT_PARSER_INVALID;
            if (base < other_end && entries[j].base < end)
                return FDT_PARSER_INVALID;
        }

        if (count >= FDT_PARSER_MAX_RESERVE_ENTRIES)
            return FDT_PARSER_NO_SPACE;
        entries[count].base = base;
        entries[count].size = size;
        entries[count].type = FDT_PARSER_REGION_RESERVED;
        count++;
    }
    return FDT_PARSER_INVALID;
}

int fdt_parser_validate(const void *blob, size_t available,
                        struct fdt_blob_view *view) {
    if (!blob || !view || available < FDT_HEADER_V16_SIZE)
        return FDT_PARSER_INVALID;

    const uint8_t *data = (const uint8_t *)blob;
    if (read_be32(data) != FDT_PARSER_MAGIC)
        return FDT_PARSER_INVALID;

    uint32_t total_size = read_be32(data + 4U);
    uint32_t struct_offset = read_be32(data + 8U);
    uint32_t strings_offset = read_be32(data + 12U);
    uint32_t reserve_offset = read_be32(data + 16U);
    uint32_t version = read_be32(data + 20U);
    uint32_t last_comp_version = read_be32(data + 24U);
    uint32_t strings_size = read_be32(data + 32U);
    uint32_t struct_size;

    if (version < 16U || last_comp_version > 17U ||
        last_comp_version > version ||
        total_size < FDT_HEADER_V16_SIZE ||
        total_size > FDT_PARSER_MAX_TOTAL_SIZE ||
        (size_t)total_size > available)
        return FDT_PARSER_INVALID;

    uint32_t header_size = version >= 17U
        ? FDT_HEADER_V17_SIZE : FDT_HEADER_V16_SIZE;
    if (total_size < header_size || available < header_size)
        return FDT_PARSER_INVALID;

    if (version >= 17U) {
        struct_size = read_be32(data + 36U);
    } else {
        if (strings_offset <= struct_offset)
            return FDT_PARSER_INVALID;
        struct_size = strings_offset - struct_offset;
    }

    if ((struct_offset & 3U) != 0 ||
        (reserve_offset & 7U) != 0 ||
        struct_offset < header_size ||
        strings_offset < header_size ||
        reserve_offset < header_size ||
        struct_size == 0 ||
        !range_inside(struct_offset, struct_size, total_size) ||
        !range_inside(strings_offset, strings_size, total_size))
        return FDT_PARSER_INVALID;

    uint32_t reserve_end;
    int reserve_status = validate_reserve_map(data, total_size,
                                               reserve_offset, &reserve_end);
    if (reserve_status != FDT_PARSER_OK)
        return reserve_status;

    if (ranges_overlap(0, header_size, struct_offset, struct_size) ||
        ranges_overlap(0, header_size, strings_offset, strings_size) ||
        ranges_overlap(0, header_size, reserve_offset,
                       reserve_end - reserve_offset) ||
        ranges_overlap(struct_offset, struct_size,
                       strings_offset, strings_size) ||
        ranges_overlap(struct_offset, struct_size,
                       reserve_offset, reserve_end - reserve_offset) ||
        ranges_overlap(strings_offset, strings_size,
                       reserve_offset, reserve_end - reserve_offset))
        return FDT_PARSER_INVALID;

    uint32_t cursor = struct_offset;
    uint32_t struct_end = struct_offset + struct_size;
    uint32_t depth = 0;
    uint32_t node_count = 0;
    uint32_t property_count = 0;
    int root_seen = 0;
    int end_seen = 0;

    while (cursor <= struct_end && struct_end - cursor >= 4U) {
        uint32_t token = read_be32(data + cursor);
        cursor += 4U;

        if (token == FDT_BEGIN_NODE) {
            if (depth >= FDT_PARSER_MAX_DEPTH ||
                node_count >= FDT_MAX_NODES)
                return FDT_PARSER_NO_SPACE;

            uint32_t name_start = cursor;
            while (cursor < struct_end && data[cursor] != 0)
                cursor++;
            if (cursor >= struct_end)
                return FDT_PARSER_INVALID;

            size_t name_len = (size_t)(cursor - name_start);
            if (depth == 0) {
                if (root_seen || name_len != 0)
                    return FDT_PARSER_INVALID;
                root_seen = 1;
            } else if (name_len == 0) {
                return FDT_PARSER_INVALID;
            }

            cursor++;
            int overflow = 0;
            cursor = align4_u32(cursor, &overflow);
            if (overflow || cursor > struct_end)
                return FDT_PARSER_INVALID;

            depth++;
            node_count++;
        } else if (token == FDT_END_NODE) {
            if (depth == 0)
                return FDT_PARSER_INVALID;
            depth--;
        } else if (token == FDT_PROP) {
            if (depth == 0 || property_count >= FDT_MAX_PROPERTIES ||
                !range_inside(cursor, 8U, struct_end))
                return FDT_PARSER_INVALID;

            uint32_t value_len = read_be32(data + cursor);
            uint32_t name_offset = read_be32(data + cursor + 4U);
            cursor += 8U;

            if (!range_inside(cursor, value_len, struct_end) ||
                !string_terminated(data + strings_offset, strings_size,
                                   name_offset))
                return FDT_PARSER_INVALID;

            uint32_t next = cursor + value_len;
            int overflow = 0;
            next = align4_u32(next, &overflow);
            if (overflow || next > struct_end)
                return FDT_PARSER_INVALID;
            cursor = next;
            property_count++;
        } else if (token == FDT_NOP) {
            /* No payload. */
        } else if (token == FDT_END) {
            if (!root_seen || depth != 0)
                return FDT_PARSER_INVALID;
            for (uint32_t i = cursor; i < struct_end; i++) {
                if (data[i] != 0)
                    return FDT_PARSER_INVALID;
            }
            end_seen = 1;
            cursor = struct_end;
            break;
        } else {
            return FDT_PARSER_INVALID;
        }
    }

    if (!root_seen || !end_seen || depth != 0)
        return FDT_PARSER_INVALID;

    view->data = data;
    view->total_size = total_size;
    view->version = version;
    view->last_comp_version = last_comp_version;
    view->struct_offset = struct_offset;
    view->struct_size = struct_size;
    view->strings_offset = strings_offset;
    view->strings_size = strings_size;
    view->reserve_offset = reserve_offset;
    view->reserve_end = reserve_end;
    return FDT_PARSER_OK;
}

static int parse_cell_property(const uint8_t *value, uint32_t length,
                               uint32_t *cells, uint32_t minimum,
                               uint32_t maximum) {
    if (!value || !cells || length != 4U)
        return FDT_PARSER_INVALID;
    uint32_t parsed = read_be32(value);
    if (parsed < minimum || parsed > maximum)
        return FDT_PARSER_INVALID;
    *cells = parsed;
    return FDT_PARSER_OK;
}

static int emit_node_regions(const struct fdt_node_ctx *node,
                             struct fdt_parser_region *usable,
                             size_t *usable_count,
                             struct fdt_parser_region *reserved,
                             size_t *reserved_count) {
    if (node->disabled)
        return FDT_PARSER_OK;

    int is_memory = node->parent_is_root &&
                    (node->name_is_memory || node->device_type_memory);
    if (is_memory) {
        if (!node->reg_seen || !node->reg_data)
            return FDT_PARSER_INVALID;
        return append_reg_tuples(node->reg_data, node->reg_len,
                                 node->reg_addr_cells,
                                 node->reg_size_cells,
                                 FDT_PARSER_REGION_USABLE,
                                 usable, FDT_PARSER_MAX_MEMORY_REGIONS,
                                 usable_count);
    }

    if (node->name_is_reserved_child) {
        /* no-map forbids a normal linear mapping. Until VMM can represent and
         * subtract no-map ranges, reject the DTB rather than silently map it. */
        if (node->no_map_seen)
            return FDT_PARSER_UNSUPPORTED;
        if (node->reg_seen && (node->size_seen || node->alignment_seen))
            return FDT_PARSER_INVALID;
        if (node->reg_seen) {
            if (!node->reg_data)
                return FDT_PARSER_INVALID;
            return append_reg_tuples(node->reg_data, node->reg_len,
                                     node->reg_addr_cells,
                                     node->reg_size_cells,
                                     FDT_PARSER_REGION_RESERVED,
                                     reserved,
                                     FDT_PARSER_MAX_RESERVED_REGIONS,
                                     reserved_count);
        }
        if (node->size_seen || node->alignment_seen)
            return FDT_PARSER_UNSUPPORTED;
        return FDT_PARSER_INVALID;
    }

    return FDT_PARSER_OK;
}

static int parse_structure_regions(const struct fdt_blob_view *view,
                                   struct fdt_parser_region *usable,
                                   size_t *usable_count,
                                   struct fdt_parser_region *reserved,
                                   size_t *reserved_count,
                                   uint32_t *cpu_count) {
    struct fdt_node_ctx stack[FDT_PARSER_MAX_DEPTH];
    uint32_t depth = 0;
    uint32_t cursor = view->struct_offset;
    uint32_t end = view->struct_offset + view->struct_size;
    uint32_t cpus = 0;
    uint32_t root_addr_cells = 2U;
    uint32_t root_size_cells = 1U;

    while (cursor <= end && end - cursor >= 4U) {
        uint32_t token = read_be32(view->data + cursor);
        cursor += 4U;

        if (token == FDT_BEGIN_NODE) {
            if (depth >= FDT_PARSER_MAX_DEPTH)
                return FDT_PARSER_NO_SPACE;
            if (depth > 0)
                stack[depth - 1U].children_seen = 1;

            uint32_t name_start = cursor;
            while (cursor < end && view->data[cursor] != 0)
                cursor++;
            if (cursor >= end)
                return FDT_PARSER_INVALID;

            const uint8_t *name = view->data + name_start;
            size_t name_len = (size_t)(cursor - name_start);
            cursor++;
            int overflow = 0;
            cursor = align4_u32(cursor, &overflow);
            if (overflow || cursor > end)
                return FDT_PARSER_INVALID;

            struct fdt_node_ctx *node = &stack[depth];
            for (size_t i = 0; i < sizeof(*node); i++)
                ((uint8_t *)node)[i] = 0;

            node->name = name;
            node->name_len = name_len;
            node->child_addr_cells = 2U;
            node->child_size_cells = 1U;

            if (depth == 0) {
                node->parent_is_root = 0;
                node->reg_addr_cells = 2U;
                node->reg_size_cells = 1U;
            } else {
                const struct fdt_node_ctx *parent = &stack[depth - 1U];
                node->parent_is_root = (uint8_t)(depth == 1U);
                node->parent_is_reserved = parent->name_is_reserved_root;
                node->parent_is_cpus =
                    (uint8_t)name_equals(parent->name, parent->name_len,
                                         "cpus", 4U);
                node->disabled = parent->disabled;
                node->reg_addr_cells = parent->child_addr_cells;
                node->reg_size_cells = parent->child_size_cells;
            }

            node->name_is_memory =
                (uint8_t)(node->parent_is_root &&
                          (name_equals(name, name_len, "memory", 6U) ||
                           name_has_prefix(name, name_len, "memory@", 7U)));
            node->name_is_reserved_root =
                (uint8_t)(node->parent_is_root &&
                          name_equals(name, name_len, "reserved-memory", 15U));
            node->name_is_reserved_child = node->parent_is_reserved;
            node->name_is_cpu =
                (uint8_t)(node->parent_is_cpus &&
                          (name_equals(name, name_len, "cpu", 3U) ||
                           name_has_prefix(name, name_len, "cpu@", 4U)));
            depth++;
        } else if (token == FDT_PROP) {
            if (depth == 0 || !range_inside(cursor, 8U, end))
                return FDT_PARSER_INVALID;

            uint32_t value_len = read_be32(view->data + cursor);
            uint32_t name_offset = read_be32(view->data + cursor + 4U);
            cursor += 8U;
            if (!range_inside(cursor, value_len, end) ||
                !string_terminated(view->data + view->strings_offset,
                                   view->strings_size, name_offset))
                return FDT_PARSER_INVALID;

            const uint8_t *value = view->data + cursor;
            const uint8_t *property_name =
                view->data + view->strings_offset + name_offset;
            size_t property_name_len = 0;
            while (property_name_len < view->strings_size - name_offset &&
                   property_name[property_name_len] != 0)
                property_name_len++;

            struct fdt_node_ctx *node = &stack[depth - 1U];
            /* Device Tree properties must precede all child nodes. */
            if (node->children_seen)
                return FDT_PARSER_INVALID;

            if (name_equals(property_name, property_name_len,
                            "#address-cells", 14U)) {
                if (node->address_cells_seen)
                    return FDT_PARSER_INVALID;
                node->address_cells_seen = 1;
                int status = parse_cell_property(value, value_len,
                                                 &node->child_addr_cells,
                                                 1U, 3U);
                if (status != FDT_PARSER_OK)
                    return status;
                if (depth == 1U)
                    root_addr_cells = node->child_addr_cells;
            } else if (name_equals(property_name, property_name_len,
                                   "#size-cells", 11U)) {
                if (node->size_cells_seen)
                    return FDT_PARSER_INVALID;
                node->size_cells_seen = 1;
                int status = parse_cell_property(value, value_len,
                                                 &node->child_size_cells,
                                                 0U, 2U);
                if (status != FDT_PARSER_OK)
                    return status;
                if (depth == 1U)
                    root_size_cells = node->child_size_cells;
            } else if (name_equals(property_name, property_name_len,
                                   "status", 6U)) {
                if (value_len == 0 ||
                    !string_terminated(value, value_len, 0))
                    return FDT_PARSER_INVALID;
                if (node->status_seen)
                    return FDT_PARSER_INVALID;
                node->status_seen = 1;
                /* Only the standard enabled values are treated as active.
                 * Unknown/failure/reserved states are never promoted to RAM. */
                node->disabled = (uint8_t)(
                    node->disabled ||
                    (!string_equals(value, value_len, "okay", 4U) &&
                     !string_equals(value, value_len, "ok", 2U)));
            } else if (name_equals(property_name, property_name_len,
                                   "device_type", 11U)) {
                if (value_len == 0 ||
                    !string_terminated(value, value_len, 0) ||
                    node->device_type_seen)
                    return FDT_PARSER_INVALID;
                node->device_type_seen = 1;
                node->device_type_memory =
                    (uint8_t)string_equals(value, value_len, "memory", 6U);
                node->device_type_cpu =
                    (uint8_t)string_equals(value, value_len, "cpu", 3U);
            } else if (name_equals(property_name, property_name_len,
                                   "reg", 3U)) {
                if (node->reg_seen)
                    return FDT_PARSER_INVALID;
                node->reg_seen = 1;
                node->reg_data = value;
                node->reg_len = value_len;
            } else if (name_equals(property_name, property_name_len,
                                   "size", 4U)) {
                if (node->size_seen)
                    return FDT_PARSER_INVALID;
                node->size_seen = 1;
                node->size_data = value;
                node->size_len = value_len;
            } else if (name_equals(property_name, property_name_len,
                                   "alignment", 9U)) {
                if (node->alignment_seen)
                    return FDT_PARSER_INVALID;
                node->alignment_seen = 1;
                node->alignment_data = value;
                node->alignment_len = value_len;
            } else if (name_equals(property_name, property_name_len,
                                   "no-map", 6U) &&
                       node->parent_is_reserved) {
                if (node->no_map_seen || value_len != 0)
                    return FDT_PARSER_INVALID;
                node->no_map_seen = 1;
            } else if (name_equals(property_name, property_name_len,
                                   "ranges", 6U) &&
                       node->name_is_reserved_root) {
                if (node->ranges_seen)
                    return FDT_PARSER_INVALID;
                node->ranges_seen = 1;
                node->ranges_empty = (uint8_t)(value_len == 0);
                if (value_len != 0)
                    return FDT_PARSER_UNSUPPORTED;
            }

            uint32_t next = cursor + value_len;
            int overflow = 0;
            next = align4_u32(next, &overflow);
            if (overflow || next > end)
                return FDT_PARSER_INVALID;
            cursor = next;
        } else if (token == FDT_END_NODE) {
            if (depth == 0)
                return FDT_PARSER_INVALID;

            struct fdt_node_ctx *node = &stack[depth - 1U];
            if (node->name_is_reserved_root) {
                if (!node->address_cells_seen || !node->size_cells_seen ||
                    !node->ranges_seen || !node->ranges_empty ||
                    node->child_addr_cells != root_addr_cells ||
                    node->child_size_cells != root_size_cells)
                    return FDT_PARSER_INVALID;
            }

            int status = emit_node_regions(node, usable, usable_count,
                                           reserved, reserved_count);
            if (status != FDT_PARSER_OK)
                return status;

            if (!node->disabled && node->name_is_cpu) {
                if (cpus == UINT32_MAX)
                    return FDT_PARSER_NO_SPACE;
                cpus++;
            }

            if (depth == 1U) {
                root_addr_cells = node->child_addr_cells;
                root_size_cells = node->child_size_cells;
            }
            depth--;
        } else if (token == FDT_NOP) {
            /* No payload. */
        } else if (token == FDT_END) {
            break;
        } else {
            return FDT_PARSER_INVALID;
        }
    }

    if (depth != 0)
        return FDT_PARSER_INVALID;
    if (cpu_count)
        *cpu_count = cpus;
    return FDT_PARSER_OK;
}

int fdt_parser_collect_regions(const struct fdt_blob_view *view,
                               uint64_t blob_phys,
                               struct fdt_parser_region *regions,
                               size_t max_count, size_t *count) {
    if (count)
        *count = 0;
    if (!view || !view->data || !regions || !count || max_count == 0)
        return FDT_PARSER_INVALID;

    struct fdt_parser_region usable[FDT_PARSER_MAX_MEMORY_REGIONS];
    struct fdt_parser_region reserved[FDT_PARSER_MAX_RESERVED_REGIONS];
    size_t usable_count = 0;
    size_t reserved_count = 0;

    uint32_t cursor = view->reserve_offset;
    while (cursor < view->reserve_end) {
        if (view->reserve_end - cursor < 16U)
            return FDT_PARSER_INVALID;
        uint64_t base = read_be64(view->data + cursor);
        uint64_t size = read_be64(view->data + cursor + 8U);
        cursor += 16U;
        if (base == 0 && size == 0)
            break;
        int status = append_region(reserved,
                                   FDT_PARSER_MAX_RESERVED_REGIONS,
                                   &reserved_count, base, size,
                                   FDT_PARSER_REGION_RESERVED);
        if (status != FDT_PARSER_OK)
            return status;
    }

    int status = append_region(reserved,
                               FDT_PARSER_MAX_RESERVED_REGIONS,
                               &reserved_count, blob_phys,
                               view->total_size,
                               FDT_PARSER_REGION_RESERVED);
    if (status != FDT_PARSER_OK)
        return status;

    uint32_t ignored_cpu_count = 0;
    status = parse_structure_regions(view, usable, &usable_count,
                                     reserved, &reserved_count,
                                     &ignored_cpu_count);
    if (status != FDT_PARSER_OK)
        return status;

    status = sort_merge_regions(usable, &usable_count);
    if (status != FDT_PARSER_OK)
        return status;
    status = sort_merge_regions(reserved, &reserved_count);
    if (status != FDT_PARSER_OK)
        return status;

    size_t required = usable_count + reserved_count;
    if (required > max_count)
        return FDT_PARSER_NO_SPACE;

    size_t out = 0;
    for (size_t i = 0; i < usable_count; i++)
        regions[out++] = usable[i];
    for (size_t i = 0; i < reserved_count; i++)
        regions[out++] = reserved[i];
    *count = out;

    return usable_count == 0 ? FDT_PARSER_NO_MEMORY : FDT_PARSER_OK;
}

uint32_t fdt_parser_count_cpus(const struct fdt_blob_view *view) {
    if (!view || !view->data)
        return 0;

    struct fdt_parser_region usable[FDT_PARSER_MAX_MEMORY_REGIONS];
    struct fdt_parser_region reserved[FDT_PARSER_MAX_RESERVED_REGIONS];
    size_t usable_count = 0;
    size_t reserved_count = 0;
    uint32_t cpu_count = 0;

    int status = parse_structure_regions(view, usable, &usable_count,
                                         reserved, &reserved_count,
                                         &cpu_count);
    return status == FDT_PARSER_OK ? cpu_count : 0;
}
