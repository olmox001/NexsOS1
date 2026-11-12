#ifndef OS1_DRIVERS_INPUT_H
#define OS1_DRIVERS_INPUT_H

#include <kernel/types.h>
#include <os1_input.h>

enum os1_input_provider {
  INPUT_PROVIDER_UNKNOWN = 0,
  INPUT_PROVIDER_VIRTIO = 1,
  INPUT_PROVIDER_PS2 = 2,
  INPUT_PROVIDER_USB_HID = 3,
  INPUT_PROVIDER_LEGACY = 4
};

enum os1_input_device_id {
  INPUT_DEVICE_VIRTIO_BASE = 0x10,
  INPUT_DEVICE_PS2_KEYBOARD = 0x21,
  INPUT_DEVICE_PS2_MOUSE = 0x22,
  INPUT_DEVICE_USB_HID_BASE = 0x30
};

void OS1driver_input_core_init(void);
void OS1driver_input_providers_init(void);
void OS1driver_input_publish(uint8_t provider, uint8_t device_id,
                             uint16_t type, uint16_t code, int32_t value);
long OS1libkernel_input_read_raw(struct os1_raw_input_event_v1 *event,
                                 unsigned role, unsigned mode);
int OS1libkernel_input_role_valid(unsigned role);
void OS1libkernel_input_trace_dump(void);

#endif /* OS1_DRIVERS_INPUT_H */
