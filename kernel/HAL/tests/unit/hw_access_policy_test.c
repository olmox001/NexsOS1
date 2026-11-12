#include <stdint.h>

#include "capability/hw_access_policy.h"

static int expect(
    enum preboot_hw_domain domain,
    enum preboot_hw_operation operation,
    enum preboot_hw_access_decision expected)
{
    return preboot_hw_access_policy(domain, operation) == expected ? 0 : 1;
}

int main(void)
{
    static const enum preboot_hw_domain all_domains[] = {
        PREBOOT_HW_DOMAIN_CPU_IDENTITY,
        PREBOOT_HW_DOMAIN_PCI_IDENTITY,
        PREBOOT_HW_DOMAIN_FIRMWARE_METADATA,
        PREBOOT_HW_DOMAIN_MMIO,
        PREBOOT_HW_DOMAIN_SMBUS,
        PREBOOT_HW_DOMAIN_PMBUS,
        PREBOOT_HW_DOMAIN_VOLTAGE_REGULATOR,
        PREBOOT_HW_DOMAIN_THERMAL_CONTROLLER,
        PREBOOT_HW_DOMAIN_POWER_CONTROLLER
    };
    static const enum preboot_hw_operation all_operations[] = {
        PREBOOT_HW_OP_ENUMERATE,
        PREBOOT_HW_OP_READ_IDENTITY,
        PREBOOT_HW_OP_READ_FIRMWARE_METADATA,
        PREBOOT_HW_OP_READ_SENSOR,
        PREBOOT_HW_OP_WRITE_CONFIGURATION,
        PREBOOT_HW_OP_SET_VOLTAGE,
        PREBOOT_HW_OP_SET_VOLTAGE_OFFSET,
        PREBOOT_HW_OP_SET_CURRENT_LIMIT,
        PREBOOT_HW_OP_SET_POWER_LIMIT,
        PREBOOT_HW_OP_SET_CLOCK,
        PREBOOT_HW_OP_SET_FREQUENCY,
        PREBOOT_HW_OP_SET_FAN_DUTY,
        PREBOOT_HW_OP_SET_THERMAL_LIMIT,
        PREBOOT_HW_OP_ENABLE_DISABLE,
        PREBOOT_HW_OP_CLEAR_FAULT,
        PREBOOT_HW_OP_UNLOCK_CONTROLLER,
        PREBOOT_HW_OP_RESET,
        PREBOOT_HW_OP_FIRMWARE_UPDATE
    };
    static const struct {
        enum preboot_hw_domain domain;
        enum preboot_hw_operation operation;
    } allowed_pairs[] = {
        {PREBOOT_HW_DOMAIN_CPU_IDENTITY, PREBOOT_HW_OP_ENUMERATE},
        {PREBOOT_HW_DOMAIN_CPU_IDENTITY, PREBOOT_HW_OP_READ_IDENTITY},
        {PREBOOT_HW_DOMAIN_PCI_IDENTITY, PREBOOT_HW_OP_ENUMERATE},
        {PREBOOT_HW_DOMAIN_PCI_IDENTITY, PREBOOT_HW_OP_READ_IDENTITY},
        {PREBOOT_HW_DOMAIN_FIRMWARE_METADATA,
         PREBOOT_HW_OP_READ_FIRMWARE_METADATA}
    };
    uint32_t domain_index = 0U;
    uint32_t operation_index = 0U;
    uint32_t pair_index = 0U;
    enum preboot_hw_access_decision expected = PREBOOT_HW_ACCESS_DENY;

    for (domain_index = 0U;
         domain_index < (uint32_t)(sizeof(all_domains) /
                                   sizeof(all_domains[0]));
         ++domain_index) {
        for (operation_index = 0U;
             operation_index < (uint32_t)(sizeof(all_operations) /
                                          sizeof(all_operations[0]));
             ++operation_index) {
            expected = PREBOOT_HW_ACCESS_DENY;
            for (pair_index = 0U;
                 pair_index < (uint32_t)(sizeof(allowed_pairs) /
                                         sizeof(allowed_pairs[0]));
                 ++pair_index) {
                if ((all_domains[domain_index] ==
                     allowed_pairs[pair_index].domain) &&
                    (all_operations[operation_index] ==
                     allowed_pairs[pair_index].operation)) {
                    expected = PREBOOT_HW_ACCESS_ALLOW;
                    break;
                }
            }
            if (expect(all_domains[domain_index],
                       all_operations[operation_index], expected) != 0) {
                return 1;
            }
        }
    }
    if (preboot_hw_access_policy((enum preboot_hw_domain)0,
                                 PREBOOT_HW_OP_ENUMERATE) !=
        PREBOOT_HW_ACCESS_DENY) return 10;
    if (preboot_hw_access_policy(PREBOOT_HW_DOMAIN_VOLTAGE_REGULATOR,
                                 (enum preboot_hw_operation)0) !=
        PREBOOT_HW_ACCESS_DENY) return 11;
    if (preboot_hw_access_policy((enum preboot_hw_domain)UINT32_MAX,
                                 (enum preboot_hw_operation)UINT32_MAX) !=
        PREBOOT_HW_ACCESS_DENY) return 12;
    return 0;
}
