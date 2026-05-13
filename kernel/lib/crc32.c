/*
 * kernel/lib/crc32.c
 * Standard CRC32 Implementation
 */
#include <kernel/string.h>
#include <kernel/types.h>

uint32_t crc32(const void *data, size_t n_bytes) {
  uint32_t crc = 0xFFFFFFFF;
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < n_bytes; i++) {
    crc ^= p[i];
    for (int j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
  }
  return ~crc;
}
