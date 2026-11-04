#ifndef _KERNEL_FDT_PARSER_H
#define _KERNEL_FDT_PARSER_H

#include <stddef.h>
#include <stdint.h>

#define FDT_PARSER_MAGIC 0xd00dfeedU
#define FDT_PARSER_MAX_TOTAL_SIZE (16U * 1024U * 1024U)
#define FDT_PARSER_MAX_DEPTH 64U
#define FDT_PARSER_MAX_MEMORY_REGIONS 128U
#define FDT_PARSER_MAX_RESERVED_REGIONS 512U
#define FDT_PARSER_MAX_RESERVE_ENTRIES 256U

enum fdt_parser_status {
    FDT_PARSER_OK = 0,
    FDT_PARSER_INVALID = -1,
    FDT_PARSER_NO_MEMORY = -2,
    FDT_PARSER_NO_SPACE = -3,
    FDT_PARSER_UNSUPPORTED = -4
};

enum fdt_parser_region_type {
    FDT_PARSER_REGION_USABLE = 1,
    FDT_PARSER_REGION_RESERVED = 2
};

struct fdt_blob_view {
    const uint8_t *data;
    uint32_t total_size;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t struct_offset;
    uint32_t struct_size;
    uint32_t strings_offset;
    uint32_t strings_size;
    uint32_t reserve_offset;
    uint32_t reserve_end;
};

struct fdt_parser_region {
    uint64_t base;
    uint64_t size;
    uint32_t type;
};

/* Validates the complete header, all block ranges, reservation terminator,
 * strings references and structure-token boundaries. */
int fdt_parser_validate(const void *blob, size_t available,
                        struct fdt_blob_view *view);

/* Collects memory nodes, the reservation map, /reserved-memory static reg
 * ranges, and the DTB allocation itself. Same-type overlaps are merged.
 * If no enabled memory node exists, reserved entries are still returned and
 * FDT_PARSER_NO_MEMORY is reported so the platform may use its legacy probe. */
int fdt_parser_collect_regions(const struct fdt_blob_view *view,
                               uint64_t blob_phys,
                               struct fdt_parser_region *regions,
                               size_t max_count, size_t *count);

/* Counts enabled CPU nodes. Returns zero for a malformed semantic tree. */
uint32_t fdt_parser_count_cpus(const struct fdt_blob_view *view);

#endif /* _KERNEL_FDT_PARSER_H */
