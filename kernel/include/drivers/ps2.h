#ifndef OS1_DRIVERS_PS2_H
#define OS1_DRIVERS_PS2_H

#include <kernel/types.h>

struct ps2_keyboard_decoder {
  uint8_t extended;
  uint8_t e1_bytes_remaining;
};

struct ps2_key_event {
  uint16_t code;
  int32_t value;
};

void ps2_keyboard_decoder_init(struct ps2_keyboard_decoder *decoder);
int ps2_keyboard_decode_set1(struct ps2_keyboard_decoder *decoder,
                             uint8_t byte, struct ps2_key_event *event);
void ps2_init(void);

#endif /* OS1_DRIVERS_PS2_H */
