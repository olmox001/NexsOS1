#ifndef OS1_INPUT_H
#define OS1_INPUT_H

#include <stdint.h>

#define OS1_INPUT_ABI_V1 1u
#define OS1_INPUT_ROLE_KEYBOARD 1u
#define OS1_INPUT_ROLE_MOUSE 2u

#define OS1_INPUT_READ_TRY 0u
#define OS1_INPUT_READ_WAIT 1u

struct os1_raw_input_event_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint64_t sequence;
  uint32_t producer_cpu;
  uint8_t provider;
  uint8_t device_id; /* stable within the current provider/device table */
  uint8_t reserved[2];
  uint16_t type;
  uint16_t code;
  int32_t value;
};

#endif
