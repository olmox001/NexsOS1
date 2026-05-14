#include <kernel/types.h>
#include <kernel/graphics.h>

/*
 * Decodes a UTF-8 sequence starting at 's'.
 * Returns the number of bytes consumed, and sets *code to the unicode codepoint.
 * Returns 0 on error.
 */
int utf8_decode(const char *s, uint32_t *code) {
  if (!s || !code) return 0;
  unsigned char c = (unsigned char)s[0];

  if (c < 0x80) {
    *code = c;
    return 1;
  } else if ((c & 0xE0) == 0xC0) {
    if ((s[1] & 0xC0) != 0x80) return 0;
    *code = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
    return 2;
  } else if ((c & 0xF0) == 0xE0) {
    if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
    *code = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
    return 3;
  } else if ((c & 0xF8) == 0xF0) {
    if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
    *code = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
            ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
    return 4;
  }
  return 0;
}
