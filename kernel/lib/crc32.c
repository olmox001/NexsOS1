/*
 * kernel/lib/crc32.c
 * Standard CRC32 Implementation
 *
 * Purpose:
 *   Provides crc32() — an ISO 3309 / ITU-T V.42 CRC-32 checksum function.
 *   Used by the kernel for data-integrity checks (VirtIO descriptors, GPT
 *   partition table header, and filesystem metadata).
 *
 * Role:
 *   Called from kernel/fs/gpt.c and any VirtIO driver that validates checksums.
 *   No callers outside the kernel; not exported to userland.
 *
 * Algorithm:
 *   Sarwate bit-by-bit (no pre-computed table).  Each byte is XOR'd into the
 *   CRC, then eight one-bit shifts are applied.  On each shift, if the output
 *   bit is 1 the generator polynomial 0xEDB88320 (bit-reversed form of the
 *   standard CRC-32 polynomial 0x04C11DB7) is XOR'd in.
 *
 *   The `-(crc & 1)` expression produces 0xFFFFFFFF when the low bit is 1
 *   (all ones mask, so the XOR applies the polynomial) and 0x00000000 when
 *   the low bit is 0 (no-op XOR).  This is a branchless conditional.
 *
 * Invariants:
 *   - Init value: 0xFFFFFFFF (pre-condition).
 *   - Final inversion: ~crc (post-condition).
 *   - Polynomial: 0xEDB88320 (reflected CRC-32).
 *   - Result is equivalent to standard CRC-32 table lookups.
 *
 * Known issues:
 *   None tracked for this file (see docs/review/analysis/07-lib-headers.md).
 *   The implementation is correct and produces standard CRC-32 values.
 *   It is O(8*n) rather than O(n) (table-driven) but adequate for kernel use.
 */
#include <kernel/string.h>
#include <kernel/types.h>

/*
 * crc32 - compute the ISO 3309 CRC-32 checksum of a byte buffer.
 *
 * Params:
 *   data    - pointer to input data; must be non-NULL if n_bytes > 0.
 *   n_bytes - number of bytes to process.
 * Returns: 32-bit CRC-32 checksum.
 * Locking: none (stateless).
 * Side effects: none.
 */
uint32_t crc32(const void *data, size_t n_bytes) {
  uint32_t crc = 0xFFFFFFFF;
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < n_bytes; i++) {
    crc ^= p[i];
    for (int j = 0; j < 8; j++) {
      /* Branchless polynomial step: XOR in 0xEDB88320 iff the output bit is 1.
       * -(crc & 1) is 0xFFFFFFFF when low bit is 1, 0x00000000 otherwise. */
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
  }
  return ~crc;
}
