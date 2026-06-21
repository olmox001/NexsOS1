/*
 * kernel/lib/utf8.c
 * UTF-8 Decoder
 *
 * Purpose:
 *   Decodes a single UTF-8 character sequence into its Unicode code point.
 *   Used by the graphics / font rendering layer to iterate over multi-byte
 *   text strings.
 *
 * Role:
 *   Called from the font renderer (kernel/graphics/font.c) to step through
 *   UTF-8 encoded strings one code point at a time.
 *
 * Supported encodings:
 *   1-byte  U+0000 .. U+007F     (c < 0x80)
 *   2-byte  U+0080 .. U+07FF     ((c & 0xE0) == 0xC0)
 *   3-byte  U+0800 .. U+FFFF     ((c & 0xF0) == 0xE0)
 *   4-byte  U+10000 .. U+10FFFF  ((c & 0xF8) == 0xF0)
 *
 * Invariants:
 *   - Continuation bytes in multi-byte sequences are validated against the
 *     0x80 pattern (high bit set, bit 6 clear) on all paths.
 *   - Returns 0 (error) for invalid lead bytes or bad continuation bytes.
 *
 * Known issues:
 *   LIB-UTF8-01  (W2 SECURITY)  Continuation bytes s[1], s[2], s[3] are read
 *                without knowing the string length.  If 's' points to a buffer
 *                whose remaining content is shorter than the sequence claims
 *                (e.g. a partial 3-byte sequence at end of a non-NUL-terminated
 *                buffer), up to 3 bytes beyond the valid data are read.
 *   LIB-UTF8-02  (W1 MISSING)   Overlong encodings (e.g. U+0000 as 0xC0 0x80),
 *                surrogate pairs (U+D800..U+DFFF), and values > U+10FFFF are
 *                accepted and decoded rather than rejected.
 */
#include <kernel/types.h>
#include <kernel/graphics.h>

/*
 * utf8_decode - decode one UTF-8 code point from the string starting at 's'.
 *
 * Examines the lead byte s[0] to determine the sequence length, validates
 * continuation bytes, assembles the code point, and returns the number of
 * bytes consumed.
 *
 * NOTE(LIB-UTF8-01): reads s[1..3] without a length bound; the caller must
 *   ensure that 's' points to a NUL-terminated string with at least 4 readable
 *   bytes beyond s[0] if the text may contain multi-byte sequences.
 * NOTE(LIB-UTF8-02): does not reject overlong encodings, surrogates, or
 *   code points above U+10FFFF.
 *
 * Params:
 *   s    - pointer to the start of a UTF-8 sequence; must not be NULL.
 *   code - output pointer; receives the decoded Unicode code point on success.
 *          Must not be NULL.
 * Returns: number of bytes consumed (1–4) on success; 0 on error (NULL args,
 *          invalid lead byte, or bad continuation byte).
 * Locking: none (stateless).
 */
int utf8_decode(const char *s, size_t len, uint32_t *code) {
  if (!s || !code || len == 0) return 0;
  unsigned char c = (unsigned char)s[0];

  if (c < 0x80) {
    /* 1-byte ASCII sequence: U+0000..U+007F */
    *code = c;
    return 1;
  } else if ((c & 0xE0) == 0xC0) {
    /* 2-byte sequence: U+0080..U+07FF */
    if (len < 2 || (s[1] & 0xC0) != 0x80) return 0;
    *code = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
    if (*code < 0x80) return 0;
    return 2;
  } else if ((c & 0xF0) == 0xE0) {
    /* 3-byte sequence: U+0800..U+FFFF */
    if (len < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
    *code = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
    if (*code < 0x800) return 0;
    if (*code >= 0xD800 && *code <= 0xDFFF) return 0;
    return 3;
  } else if ((c & 0xF8) == 0xF0) {
    /* 4-byte sequence: U+10000..U+10FFFF */
    if (len < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
    *code = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
            ((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
    if (*code < 0x10000) return 0;
    if (*code > 0x10FFFF) return 0;
    return 4;
  }
  /* Invalid lead byte (e.g. continuation byte used as lead, or 0xFF/0xFE) */
  return 0;
}
