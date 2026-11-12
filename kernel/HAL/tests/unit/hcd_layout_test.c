#include "common/hcd_wire.h"

int main(void)
{
    const struct hcd_header header = {0};
    const struct hcd_section_entry section = {0};
    const struct hcd_cpu_summary summary = {0};
    const struct hcd_cpu_record cpu = {0};

    if (header.magic != 0U) {
        return 1;
    }
    if (section.type != 0U) {
        return 2;
    }
    if (summary.cpu_count != 0U) {
        return 3;
    }
    if (cpu.logical_cpu_id != 0U) {
        return 4;
    }
    return 0;
}
