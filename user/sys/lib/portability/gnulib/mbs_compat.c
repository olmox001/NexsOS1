#include <stddef.h>
#include <string.h>

/* Minimal multibyte-string compatibility shims needed by Coreutils on NexsOS1.
 * These are intentionally small and conservative; they satisfy the GNU-like
 * symbols referenced by expr without modifying the upstream second-repository
 * sources.
 */

size_t mbslen(const char *string)
{
  size_t count = 0;
  if (!string)
    return 0;

  while (*string) {
    unsigned char ch = (unsigned char)*string;
    if ((ch & 0x80) == 0) {
      string++;
      count++;
      continue;
    }

    /* Count UTF-8 codepoints conservatively: skip one full codepoint. */
    if ((ch & 0xE0) == 0xC0) {
      string += 2;
    } else if ((ch & 0xF0) == 0xE0) {
      string += 3;
    } else if ((ch & 0xF8) == 0xF0) {
      string += 4;
    } else {
      string++;
    }
    count++;
  }

  return count;
}

char *mbschr(const char *string, int c)
{
  if (!string)
    return NULL;

  while (*string) {
    unsigned char ch = (unsigned char)*string;
    if ((ch & 0x80) == 0) {
      if ((unsigned char)c == ch)
        return (char *)string;
      string++;
      continue;
    }

    /* ASCII-compatible single-byte search only; UTF-8 codepoints are treated
     * as opaque for the current NexsOS1 bootstrap layer. */
    if ((ch & 0xE0) == 0xC0) {
      string += 2;
    } else if ((ch & 0xF0) == 0xE0) {
      string += 3;
    } else if ((ch & 0xF8) == 0xF0) {
      string += 4;
    } else {
      string++;
    }
  }

  return NULL;
}
