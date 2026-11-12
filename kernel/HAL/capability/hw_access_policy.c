#include "hw_access_policy.h"

enum preboot_hw_access_decision preboot_hw_access_policy(
    enum preboot_hw_domain domain,
    enum preboot_hw_operation operation)
{
    /* No actuator, sensor bus, MMIO, or controller register access in preboot. */
    switch (operation) {
    case PREBOOT_HW_OP_ENUMERATE:
    case PREBOOT_HW_OP_READ_IDENTITY:
        if ((domain == PREBOOT_HW_DOMAIN_CPU_IDENTITY) ||
            (domain == PREBOOT_HW_DOMAIN_PCI_IDENTITY)) {
            return PREBOOT_HW_ACCESS_ALLOW;
        }
        return PREBOOT_HW_ACCESS_DENY;
    case PREBOOT_HW_OP_READ_FIRMWARE_METADATA:
        if (domain == PREBOOT_HW_DOMAIN_FIRMWARE_METADATA) {
            return PREBOOT_HW_ACCESS_ALLOW;
        }
        return PREBOOT_HW_ACCESS_DENY;
    case PREBOOT_HW_OP_READ_SENSOR:
    case PREBOOT_HW_OP_WRITE_CONFIGURATION:
    case PREBOOT_HW_OP_SET_VOLTAGE:
    case PREBOOT_HW_OP_SET_VOLTAGE_OFFSET:
    case PREBOOT_HW_OP_SET_CURRENT_LIMIT:
    case PREBOOT_HW_OP_SET_POWER_LIMIT:
    case PREBOOT_HW_OP_SET_CLOCK:
    case PREBOOT_HW_OP_SET_FREQUENCY:
    case PREBOOT_HW_OP_SET_FAN_DUTY:
    case PREBOOT_HW_OP_SET_THERMAL_LIMIT:
    case PREBOOT_HW_OP_ENABLE_DISABLE:
    case PREBOOT_HW_OP_CLEAR_FAULT:
    case PREBOOT_HW_OP_UNLOCK_CONTROLLER:
    case PREBOOT_HW_OP_RESET:
    case PREBOOT_HW_OP_FIRMWARE_UPDATE:
    default:
        return PREBOOT_HW_ACCESS_DENY;
    }
}
