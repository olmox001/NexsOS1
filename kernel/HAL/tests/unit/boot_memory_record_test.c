#include <common/boot_memory_record.h>
#include <common/hcd_endian.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MUTATION_ITERATIONS UINT64_C(100000)

static void valid_record(uint8_t record[NXHAL_BOOT_MEMORY_WIRE_RECORD_SIZE]) {
  memset(record, 0, NXHAL_BOOT_MEMORY_WIRE_RECORD_SIZE);
  hcd_store_le64(record, 0U, UINT64_C(0x40000000));
  hcd_store_le64(record, 8U, UINT64_C(0x200000));
  hcd_store_le32(record, 16U, (uint32_t)NXHAL_BOOT_MEMORY_USABLE);
  hcd_store_le32(record, 20U, UINT32_C(0x7));
  hcd_store_le32(record, 24U, UINT32_C(0xffffffff));
  hcd_store_le32(record, 28U, UINT32_C(0xffffffff));
}

static uint64_t next_random(uint64_t *state) {
  *state ^= *state >> 12U;
  *state ^= *state << 25U;
  *state ^= *state >> 27U;
  return *state * UINT64_C(2685821657736338717);
}

static int test_valid(void) {
  uint8_t record[NXHAL_BOOT_MEMORY_WIRE_RECORD_SIZE];
  struct nxhal_boot_memory_region out;

  valid_record(record);
  if (nxhal_boot_memory_decode_hcd_record(record, &out) !=
      NXHAL_BOOT_MEMORY_OK)
    return 1;
  if (out.base != UINT64_C(0x40000000) ||
      out.length != UINT64_C(0x200000) ||
      out.type != (uint32_t)NXHAL_BOOT_MEMORY_USABLE ||
      out.attributes != UINT32_C(0x7))
    return 2;
  return 0;
}

static int test_rejections(void) {
  uint8_t record[NXHAL_BOOT_MEMORY_WIRE_RECORD_SIZE];
  struct nxhal_boot_memory_region out;

  valid_record(record);
  hcd_store_le64(record, 8U, 0U);
  if (nxhal_boot_memory_decode_hcd_record(record, &out) ==
      NXHAL_BOOT_MEMORY_OK)
    return 1;
  valid_record(record);
  hcd_store_le32(record, 16U, UINT32_C(11));
  if (nxhal_boot_memory_decode_hcd_record(record, &out) ==
      NXHAL_BOOT_MEMORY_OK)
    return 2;
  valid_record(record);
  hcd_store_le64(record, 0U, UINT64_MAX - UINT64_C(0xfff));
  hcd_store_le64(record, 8U, UINT64_C(0x2000));
  if (nxhal_boot_memory_decode_hcd_record(record, &out) ==
      NXHAL_BOOT_MEMORY_OK)
    return 3;
  valid_record(record);
  hcd_store_le64(record, 32U, UINT64_C(1));
  if (nxhal_boot_memory_decode_hcd_record(record, &out) ==
      NXHAL_BOOT_MEMORY_OK)
    return 4;
  if (nxhal_boot_memory_decode_hcd_record(NULL, &out) !=
      NXHAL_BOOT_MEMORY_E_INVALID)
    return 5;
  return nxhal_boot_memory_decode_hcd_record(record, NULL) ==
                 NXHAL_BOOT_MEMORY_E_INVALID
             ? 0
             : 6;
}

static void mutate(uint8_t *record, uint64_t *state) {
  uint64_t count = (next_random(state) % UINT64_C(4)) + 1U;

  for (uint64_t index = 0U; index < count; ++index) {
    size_t offset = (size_t)(
        next_random(state) % NXHAL_BOOT_MEMORY_WIRE_RECORD_SIZE);
    uint8_t delta = (uint8_t)next_random(state);
    record[offset] ^= delta == 0U ? UINT8_C(1) : delta;
  }
}

static int test_mutations(void) {
  uint8_t record[NXHAL_BOOT_MEMORY_WIRE_RECORD_SIZE];
  struct nxhal_boot_memory_region out;
  uint64_t state = UINT64_C(0x4e5848414c4d4150);
  uint64_t accepted = 0U;

  for (uint64_t iteration = 0U; iteration < MUTATION_ITERATIONS; ++iteration) {
    valid_record(record);
    if ((iteration & UINT64_C(3)) != 0U)
      mutate(record, &state);
    if (nxhal_boot_memory_decode_hcd_record(record, &out) ==
        NXHAL_BOOT_MEMORY_OK) {
      ++accepted;
      if (out.length == 0U || out.base > UINT64_MAX - out.length ||
          out.type < (uint32_t)NXHAL_BOOT_MEMORY_USABLE ||
          out.type > (uint32_t)NXHAL_BOOT_MEMORY_UNACCEPTED)
        return 1;
    }
  }
  if (accepted == 0U)
    return 2;
  (void)printf("NX-HAL boot-memory mutation: %" PRIu64
               " PASS accepted=%" PRIu64 "\n",
               MUTATION_ITERATIONS, accepted);
  return 0;
}

int main(void) {
  if (test_valid() != 0)
    return 1;
  if (test_rejections() != 0)
    return 2;
  if (test_mutations() != 0)
    return 3;
  return 0;
}
