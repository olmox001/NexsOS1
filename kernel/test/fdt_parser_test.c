#include <kernel/fdt_parser.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CAPACITY 8192U
#define TEST_MAGIC 0xd00dfeedU

#define TOK_BEGIN_NODE 1U
#define TOK_END_NODE   2U
#define TOK_PROP       3U
#define TOK_NOP        4U
#define TOK_END        9U

struct builder {
    uint8_t data[TEST_CAPACITY];
    size_t pos;
    size_t struct_start;
    char strings[1024];
    size_t strings_size;
};

static unsigned failures;
static unsigned checks;

#define CHECK(condition, message) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
        failures++; \
    } \
} while (0)

static void put_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void put_be64(uint8_t *p, uint64_t value) {
    put_be32(p, (uint32_t)(value >> 32));
    put_be32(p + 4, (uint32_t)value);
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void emit32(struct builder *b, uint32_t value) {
    put_be32(b->data + b->pos, value);
    b->pos += 4U;
}

static void emit_bytes(struct builder *b, const void *data, size_t length) {
    memcpy(b->data + b->pos, data, length);
    b->pos += length;
}

static void emit_name(struct builder *b, const char *name) {
    size_t length = strlen(name) + 1U;
    emit_bytes(b, name, length);
    while ((b->pos & 3U) != 0)
        b->data[b->pos++] = 0;
}

static uint32_t string_offset(struct builder *b, const char *name) {
    size_t length = strlen(name) + 1U;
    size_t offset = 0;
    while (offset < b->strings_size) {
        if (strcmp(b->strings + offset, name) == 0)
            return (uint32_t)offset;
        offset += strlen(b->strings + offset) + 1U;
    }
    if (b->strings_size + length > sizeof(b->strings))
        abort();
    uint32_t result = (uint32_t)b->strings_size;
    memcpy(b->strings + b->strings_size, name, length);
    b->strings_size += length;
    return result;
}

static void begin_node(struct builder *b, const char *name) {
    emit32(b, TOK_BEGIN_NODE);
    emit_name(b, name);
}

static void end_node(struct builder *b) {
    emit32(b, TOK_END_NODE);
}

static void property(struct builder *b, const char *name,
                     const void *value, uint32_t length) {
    emit32(b, TOK_PROP);
    emit32(b, length);
    emit32(b, string_offset(b, name));
    if (length != 0)
        emit_bytes(b, value, length);
    while ((b->pos & 3U) != 0)
        b->data[b->pos++] = 0;
}

static void property_u32(struct builder *b, const char *name, uint32_t value) {
    uint8_t data[4];
    put_be32(data, value);
    property(b, name, data, sizeof(data));
}

static void property_string(struct builder *b, const char *name,
                            const char *value) {
    property(b, name, value, (uint32_t)(strlen(value) + 1U));
}

static void property_reg_pair(struct builder *b, uint64_t base,
                              uint64_t size) {
    uint8_t data[16];
    put_be64(data, base);
    put_be64(data + 8, size);
    property(b, "reg", data, sizeof(data));
}

static size_t build_fixture_ex(uint8_t *out, size_t capacity,
                               int include_memory, int dynamic_reserved,
                               int bad_address_cells, uint32_t version,
                               int include_ranges, int disable_reserved_root,
                               int mismatch_reserved_cells,
                               int late_root_cells, int no_map_reserved) {
    struct builder b;
    memset(&b, 0, sizeof(b));

    const uint32_t reserve_offset = 40U;
    const uint32_t struct_offset = 72U;
    if (capacity < TEST_CAPACITY)
        return 0;

    put_be64(b.data + reserve_offset, 0x40300000ULL);
    put_be64(b.data + reserve_offset + 8U, 0x2000ULL);
    put_be64(b.data + reserve_offset + 16U, 0);
    put_be64(b.data + reserve_offset + 24U, 0);

    b.pos = struct_offset;
    b.struct_start = struct_offset;

    begin_node(&b, "");
    property_u32(&b, "#address-cells", bad_address_cells ? 3U : 2U);
    property_u32(&b, "#size-cells", 2U);

    if (include_memory) {
        begin_node(&b, "memory@40000000");
        property_string(&b, "device_type", "memory");
        uint8_t memory_reg[32];
        put_be64(memory_reg, 0x40000000ULL);
        put_be64(memory_reg + 8, 0x40000000ULL);
        put_be64(memory_reg + 16, 0x100000000ULL);
        put_be64(memory_reg + 24, 0x100000000ULL);
        property(&b, "reg", memory_reg, sizeof(memory_reg));
        end_node(&b);
    }

    if (late_root_cells)
        property_u32(&b, "#address-cells", 2U);

    begin_node(&b, "reserved-memory");
    if (disable_reserved_root)
        property_string(&b, "status", "disabled");
    property_u32(&b, "#address-cells", mismatch_reserved_cells ? 1U : 2U);
    property_u32(&b, "#size-cells", 2U);
    if (include_ranges)
        property(&b, "ranges", NULL, 0U);

    begin_node(&b, "secure@40100000");
    property_reg_pair(&b, 0x40100000ULL, 0x200000ULL);
    if (no_map_reserved)
        property(&b, "no-map", NULL, 0U);
    end_node(&b);

    begin_node(&b, "overlap@40300000");
    if (dynamic_reserved) {
        uint8_t size[8];
        put_be64(size, 0x10000ULL);
        property(&b, "size", size, sizeof(size));
    } else {
        property_reg_pair(&b, 0x40300000ULL, 0x1000ULL);
    }
    end_node(&b);
    end_node(&b);

    begin_node(&b, "cpus");
    property_u32(&b, "#address-cells", 1U);
    property_u32(&b, "#size-cells", 0U);

    begin_node(&b, "cpu@0");
    property_string(&b, "device_type", "cpu");
    property_string(&b, "status", "okay");
    end_node(&b);

    begin_node(&b, "cpu@1");
    property_string(&b, "device_type", "cpu");
    property_string(&b, "status", "disabled");
    end_node(&b);

    end_node(&b);
    end_node(&b);
    emit32(&b, TOK_END);

    uint32_t struct_size = (uint32_t)(b.pos - b.struct_start);
    uint32_t strings_offset = (uint32_t)b.pos;
    if (b.strings_size > capacity - b.pos)
        return 0;
    memcpy(b.data + b.pos, b.strings, b.strings_size);
    b.pos += b.strings_size;

    uint32_t total_size = (uint32_t)b.pos;
    put_be32(b.data, TEST_MAGIC);
    put_be32(b.data + 4U, total_size);
    put_be32(b.data + 8U, struct_offset);
    put_be32(b.data + 12U, strings_offset);
    put_be32(b.data + 16U, reserve_offset);
    put_be32(b.data + 20U, version);
    put_be32(b.data + 24U, 16U);
    put_be32(b.data + 28U, 0U);
    put_be32(b.data + 32U, (uint32_t)b.strings_size);
    if (version >= 17U)
        put_be32(b.data + 36U, struct_size);

    memcpy(out, b.data, total_size);
    return total_size;
}

static size_t build_fixture(uint8_t *out, size_t capacity,
                            int include_memory, int dynamic_reserved,
                            int bad_address_cells, uint32_t version) {
    return build_fixture_ex(out, capacity, include_memory, dynamic_reserved,
                            bad_address_cells, version, 1, 0, 0, 0, 0);
}

static int parse_fixture(uint8_t *blob, size_t length,
                         struct fdt_blob_view *view,
                         struct fdt_parser_region *regions,
                         size_t capacity, size_t *count) {
    int status = fdt_parser_validate(blob, length, view);
    if (status != FDT_PARSER_OK)
        return status;
    return fdt_parser_collect_regions(view, 0x50000000ULL,
                                      regions, capacity, count);
}

static void test_valid_and_regions(void) {
    uint8_t blob[TEST_CAPACITY];
    struct fdt_blob_view view;
    struct fdt_parser_region regions[32];
    size_t count = 0;
    size_t length = build_fixture(blob, sizeof(blob), 1, 0, 0, 17U);

    CHECK(length != 0, "fixture built");
    int status = parse_fixture(blob, length, &view, regions, 32U, &count);
    CHECK(status == FDT_PARSER_OK, "valid FDT accepted");
    CHECK(view.total_size == length, "totalsize retained");
    CHECK(fdt_parser_count_cpus(&view) == 1U, "disabled CPU excluded");
    CHECK(count == 4U, "two RAM plus two merged reserved descriptors");

    CHECK(regions[0].type == FDT_PARSER_REGION_USABLE &&
          regions[0].base == 0x40000000ULL &&
          regions[0].size == 0x40000000ULL,
          "first RAM region retained");
    CHECK(regions[1].type == FDT_PARSER_REGION_USABLE &&
          regions[1].base == 0x100000000ULL &&
          regions[1].size == 0x100000000ULL,
          "non-contiguous second RAM region retained");
    CHECK(regions[2].type == FDT_PARSER_REGION_RESERVED &&
          regions[2].base == 0x40100000ULL &&
          regions[2].size == 0x202000ULL,
          "adjacent and overlapping reserved ranges merged");
    CHECK(regions[3].type == FDT_PARSER_REGION_RESERVED &&
          regions[3].base == 0x50000000ULL &&
          regions[3].size == length,
          "DTB allocation reserved");
}

static void test_no_memory_preserves_reservations(void) {
    uint8_t blob[TEST_CAPACITY];
    struct fdt_blob_view view;
    struct fdt_parser_region regions[32];
    size_t count = 0;
    size_t length = build_fixture(blob, sizeof(blob), 0, 0, 0, 17U);
    CHECK(length != 0, "no-memory fixture built");
    CHECK(fdt_parser_validate(blob, length, &view) == FDT_PARSER_OK,
          "no-memory FDT structurally valid");
    int status = fdt_parser_collect_regions(&view, 0x50000000ULL,
                                             regions, 32U, &count);
    CHECK(status == FDT_PARSER_NO_MEMORY,
          "missing memory node has distinct status");
    CHECK(count == 2U,
          "merged reservations and DTB retained for fallback");
}

static void test_v16_compatibility(void) {
    uint8_t blob[TEST_CAPACITY];
    struct fdt_blob_view view;
    size_t length = build_fixture(blob, sizeof(blob), 1, 0, 0, 16U);
    CHECK(length != 0, "v16 fixture built");
    CHECK(fdt_parser_validate(blob, length, &view) == FDT_PARSER_OK,
          "v16 structure size derived from strings offset");
    CHECK(view.version == 16U, "v16 version retained");
}

static void test_dynamic_reserved_rejected(void) {
    uint8_t blob[TEST_CAPACITY];
    struct fdt_blob_view view;
    struct fdt_parser_region regions[32];
    size_t count = 0;
    size_t length = build_fixture(blob, sizeof(blob), 1, 1, 0, 17U);
    CHECK(length != 0, "dynamic-reserved fixture built");
    CHECK(fdt_parser_validate(blob, length, &view) == FDT_PARSER_OK,
          "dynamic-reserved tree structurally valid");
    int status = fdt_parser_collect_regions(&view, 0x50000000ULL,
                                             regions, 32U, &count);
    CHECK(status == FDT_PARSER_UNSUPPORTED,
          "dynamic reserved-memory request fails closed, never ignored");
    CHECK(count == 0U, "unsupported reservation emits no partial map");
}

static void test_reserved_memory_contract(void) {
    uint8_t blob[TEST_CAPACITY];
    struct fdt_blob_view view;
    struct fdt_parser_region regions[32];
    size_t count = 0;

    size_t length = build_fixture_ex(blob, sizeof(blob), 1, 0, 0, 17U,
                                     0, 0, 0, 0, 0);
    CHECK(length != 0, "missing-ranges fixture built");
    CHECK(fdt_parser_validate(blob, length, &view) == FDT_PARSER_OK,
          "missing-ranges fixture structurally valid");
    CHECK(fdt_parser_collect_regions(&view, 0x50000000ULL,
                                     regions, 32U, &count) ==
              FDT_PARSER_INVALID,
          "reserved-memory without required empty ranges rejected");

    length = build_fixture_ex(blob, sizeof(blob), 1, 0, 0, 17U,
                              1, 0, 1, 0, 0);
    CHECK(length != 0, "mismatched-cell fixture built");
    CHECK(fdt_parser_validate(blob, length, &view) == FDT_PARSER_OK,
          "mismatched-cell fixture structurally valid");
    CHECK(fdt_parser_collect_regions(&view, 0x50000000ULL,
                                     regions, 32U, &count) ==
              FDT_PARSER_INVALID,
          "reserved-memory cell widths must match root");

    length = build_fixture_ex(blob, sizeof(blob), 1, 0, 0, 17U,
                              1, 1, 0, 0, 0);
    CHECK(length != 0, "disabled-reserved-root fixture built");
    CHECK(fdt_parser_validate(blob, length, &view) == FDT_PARSER_OK,
          "disabled-reserved-root fixture structurally valid");
    CHECK(fdt_parser_collect_regions(&view, 0x50000000ULL,
                                     regions, 32U, &count) ==
              FDT_PARSER_OK,
          "disabled reserved-memory subtree ignored");
    CHECK(count == 4U &&
          regions[2].type == FDT_PARSER_REGION_RESERVED &&
          regions[2].base == 0x40300000ULL &&
          regions[2].size == 0x2000ULL,
          "disabled reserved-memory parent suppresses child reservations");

    length = build_fixture_ex(blob, sizeof(blob), 1, 0, 0, 17U,
                              1, 0, 0, 1, 0);
    CHECK(length != 0, "late-root-property fixture built");
    CHECK(fdt_parser_validate(blob, length, &view) == FDT_PARSER_OK,
          "late-root-property fixture structurally valid");
    CHECK(fdt_parser_collect_regions(&view, 0x50000000ULL,
                                     regions, 32U, &count) ==
              FDT_PARSER_INVALID,
          "properties after child nodes rejected");

    length = build_fixture_ex(blob, sizeof(blob), 1, 0, 0, 17U,
                              1, 0, 0, 0, 1);
    CHECK(length != 0, "no-map reserved fixture built");
    CHECK(fdt_parser_validate(blob, length, &view) == FDT_PARSER_OK,
          "no-map reserved fixture structurally valid");
    CHECK(fdt_parser_collect_regions(&view, 0x50000000ULL,
                                     regions, 32U, &count) ==
              FDT_PARSER_UNSUPPORTED,
          "no-map reservation rejected until VMM can exclude it");
}

static void test_malformed_headers(void) {
    uint8_t blob[TEST_CAPACITY];
    uint8_t copy[TEST_CAPACITY];
    struct fdt_blob_view view;
    size_t length = build_fixture(blob, sizeof(blob), 1, 0, 0, 17U);

    CHECK(fdt_parser_validate(blob, length - 1U, &view) ==
              FDT_PARSER_INVALID,
          "truncated available span rejected");

    memcpy(copy, blob, length);
    copy[0] ^= 0xffU;
    CHECK(fdt_parser_validate(copy, length, &view) == FDT_PARSER_INVALID,
          "wrong magic rejected");

    memcpy(copy, blob, length);
    put_be32(copy + 4U, (uint32_t)length + 1U);
    CHECK(fdt_parser_validate(copy, length, &view) == FDT_PARSER_INVALID,
          "totalsize larger than available rejected");

    memcpy(copy, blob, length);
    put_be32(copy + 8U, 0xfffffff0U);
    CHECK(fdt_parser_validate(copy, length, &view) == FDT_PARSER_INVALID,
          "structure offset overflow rejected");

    memcpy(copy, blob, length);
    put_be32(copy + 12U, 0xfffffff0U);
    CHECK(fdt_parser_validate(copy, length, &view) == FDT_PARSER_INVALID,
          "strings offset overflow rejected");

    memcpy(copy, blob, length);
    put_be32(copy + 16U, 0xfffffff8U);
    CHECK(fdt_parser_validate(copy, length, &view) == FDT_PARSER_INVALID,
          "reservation offset overflow rejected");

    memcpy(copy, blob, length);
    put_be32(copy + 20U, 15U);
    CHECK(fdt_parser_validate(copy, length, &view) == FDT_PARSER_INVALID,
          "unsupported old version rejected");

    memcpy(copy, blob, length);
    put_be32(copy + 24U, 18U);
    CHECK(fdt_parser_validate(copy, length, &view) == FDT_PARSER_INVALID,
          "incompatible future version rejected");
}

static void test_malformed_structure_and_reserve_map(void) {
    uint8_t blob[TEST_CAPACITY];
    uint8_t copy[TEST_CAPACITY];
    struct fdt_blob_view view;
    size_t length = build_fixture(blob, sizeof(blob), 1, 0, 0, 17U);

    memcpy(copy, blob, length);
    uint32_t reserve_offset = get_be32(copy + 16U);
    put_be64(copy + reserve_offset + 16U, 0x1234ULL);
    put_be64(copy + reserve_offset + 24U, 0x1000ULL);
    memset(copy + reserve_offset + 32U, 0xff, length - reserve_offset - 32U);
    CHECK(fdt_parser_validate(copy, length, &view) == FDT_PARSER_INVALID,
          "missing reservation terminator rejected");

    memcpy(copy, blob, length);
    uint32_t struct_offset = get_be32(copy + 8U);
    uint32_t struct_size = get_be32(copy + 36U);
    put_be32(copy + struct_offset + struct_size - 4U, TOK_NOP);
    CHECK(fdt_parser_validate(copy, length, &view) == FDT_PARSER_INVALID,
          "missing FDT_END rejected");

    memcpy(copy, blob, length);
    put_be32(copy + 8U, get_be32(copy + 12U));
    CHECK(fdt_parser_validate(copy, length, &view) == FDT_PARSER_INVALID,
          "overlapping structure and strings blocks rejected");

    memcpy(copy, blob, length);
    copy[struct_offset + 4U] = 'x';
    CHECK(fdt_parser_validate(copy, length, &view) == FDT_PARSER_INVALID,
          "unterminated node name rejected");

    memcpy(copy, blob, length);
    put_be32(copy + struct_offset + 16U, 0xfffffff0U);
    CHECK(fdt_parser_validate(copy, length, &view) == FDT_PARSER_INVALID,
          "property name offset outside strings rejected");
}

static void test_cell_width_and_capacity(void) {
    uint8_t blob[TEST_CAPACITY];
    struct fdt_blob_view view;
    struct fdt_parser_region regions[2];
    size_t count = 0;
    size_t length = build_fixture(blob, sizeof(blob), 1, 0, 1, 17U);
    CHECK(length != 0, "bad-cell fixture built");
    CHECK(fdt_parser_validate(blob, length, &view) == FDT_PARSER_OK,
          "bad cell width is a semantic error");
    CHECK(fdt_parser_collect_regions(&view, 0x50000000ULL,
                                     regions, 2U, &count) ==
              FDT_PARSER_INVALID,
          "oversized address cells rejected");

    length = build_fixture(blob, sizeof(blob), 1, 0, 0, 17U);
    CHECK(fdt_parser_validate(blob, length, &view) == FDT_PARSER_OK,
          "capacity fixture valid");
    CHECK(fdt_parser_collect_regions(&view, 0x50000000ULL,
                                     regions, 2U, &count) ==
              FDT_PARSER_NO_SPACE,
          "insufficient output capacity rejected");
}

static void test_real_dtb(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "FAIL: cannot open required QEMU DTB %s\n", path);
        CHECK(0, "required QEMU DTB opens");
        return;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        CHECK(0, "seek real DTB");
        return;
    }
    long end = ftell(file);
    if (end <= 0 || (unsigned long)end > FDT_PARSER_MAX_TOTAL_SIZE) {
        fclose(file);
        CHECK(0, "real DTB size within parser bound");
        return;
    }
    rewind(file);

    size_t length = (size_t)end;
    uint8_t *blob = (uint8_t *)malloc(length);
    if (!blob) {
        fclose(file);
        CHECK(0, "allocate real DTB");
        return;
    }
    size_t read_count = fread(blob, 1U, length, file);
    fclose(file);
    CHECK(read_count == length, "read complete real DTB");

    struct fdt_blob_view view;
    struct fdt_parser_region regions[768];
    size_t count = 0;
    int status = fdt_parser_validate(blob, length, &view);
    CHECK(status == FDT_PARSER_OK, "QEMU-generated DTB validates");
    if (status == FDT_PARSER_OK) {
        status = fdt_parser_collect_regions(&view, 0x170000000ULL,
                                            regions, 768U, &count);
        CHECK(status == FDT_PARSER_OK, "QEMU DTB memory regions parse");
        CHECK(fdt_parser_count_cpus(&view) == 4U,
              "QEMU DTB exposes four enabled CPUs");
        CHECK(count >= 2U, "QEMU DTB yields RAM and DTB reservation");
        if (count >= 2U) {
            CHECK(regions[0].base == 0x40000000ULL,
                  "QEMU RAM starts at physical memory base");
            CHECK(regions[0].size == 0x140000000ULL,
                  "QEMU DTB reports five GiB RAM, not physical span from zero");
        }
        if (status == FDT_PARSER_OK)
            printf("QEMU DTB: PASS (size=%zu bytes, regions=%zu, CPUs=%u)\n",
                   length, count, fdt_parser_count_cpus(&view));
    }
    free(blob);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <qemu-virt.dtb>\n", argv[0]);
        return 2;
    }

    test_valid_and_regions();
    test_no_memory_preserves_reservations();
    test_v16_compatibility();
    test_dynamic_reserved_rejected();
    test_reserved_memory_contract();
    test_malformed_headers();
    test_malformed_structure_and_reserve_map();
    test_cell_width_and_capacity();
    test_real_dtb(argv[1]);

    if (failures != 0) {
        fprintf(stderr, "%u/%u FDT parser checks failed\n", failures, checks);
        return 1;
    }
    printf("FDT parser tests: PASS (%u checks)\n", checks);
    return 0;
}
