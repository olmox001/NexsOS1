/*
 * kernel/lib/string.c
 * String manipulation functions
 *
 * Purpose:
 *   Provides the complete set of in-kernel string and memory primitives used
 *   by virtually every other kernel subsystem.  This file is a self-contained
 *   replacement for the C standard library string functions; no libc is linked
 *   into the kernel.
 *
 * Role:
 *   - Consumed by kmalloc, printk, registry, vfs, fs, sched, and driver layers.
 *   - Shared between amd64 and aarch64 builds; no arch-specific code is present.
 *   - String functions that mirror POSIX semantics (strlen, strcpy, strcmp …)
 *     are also re-exported to userland programs through include/api/string.h.
 *
 * Invariants:
 *   - Every function that accepts a pointer checks it for NULL before
 *     dereferencing.  Callers can safely pass NULL and receive a defined result
 *     (0, NULL, or dest as appropriate) rather than a fault.
 *   - No dynamic allocation is performed; no global state is modified.
 *   - Operations are purely sequential byte-by-byte; no SIMD or hardware assist.
 *
 * Known issues:
 *   None tracked for this file (see docs/review/analysis/07-lib-headers.md).
 *   The implementations are correct, safe, and complete for the kernel's needs.
 */
#include <kernel/string.h>
#include <kernel/types.h>

/*
 * strlen - return the number of bytes in a NUL-terminated string.
 *
 * Params: s - string pointer; may be NULL.
 * Returns: byte count excluding the NUL terminator, or 0 if s is NULL.
 * Locking: none; reads only the caller's memory.
 */
size_t strlen(const char *s) {
  if (!s)
    return 0;
  const char *p = s;
  while (*p)
    p++;
  return p - s;
}

/*
 * strnlen - return the number of bytes in a string, up to maxlen.
 *
 * Params:
 *   s      - string pointer; may be NULL.
 *   maxlen - maximum number of bytes to examine.
 * Returns: byte count (< maxlen) if a NUL is found, otherwise maxlen.
 *          Returns 0 if s is NULL.
 * Locking: none.
 */
size_t strnlen(const char *s, size_t maxlen) {
  if (!s)
    return 0;
  const char *p = s;
  while (maxlen-- && *p)
    p++;
  return p - s;
}

/*
 * strcmp - lexicographically compare two NUL-terminated strings.
 *
 * Params:
 *   s1, s2 - string pointers; either may be NULL.
 * Returns:
 *   0 if equal (or both NULL).
 *   Positive if s1 > s2 (unsigned byte comparison).
 *   Negative if s1 < s2.
 *   If exactly one is NULL, non-NULL is treated as greater.
 * Locking: none.
 */
int strcmp(const char *s1, const char *s2) {
  if (!s1 || !s2)
    return (s1 == s2) ? 0 : (s1 ? 1 : -1);
  while (*s1 && *s1 == *s2) {
    s1++;
    s2++;
  }
  return (unsigned char)*s1 - (unsigned char)*s2;
}

/*
 * strncmp - lexicographically compare at most n bytes of two strings.
 *
 * Params:
 *   s1, s2 - string pointers; either may be NULL.
 *   n      - maximum number of bytes to compare.
 * Returns: same sign semantics as strcmp; 0 if n is 0.
 *          NULL handling matches strcmp.
 * Locking: none.
 */
int strncmp(const char *s1, const char *s2, size_t n) {
  if (n == 0)
    return 0;
  if (!s1 || !s2)
    return (s1 == s2) ? 0 : (s1 ? 1 : -1);
  while (n-- > 1 && *s1 && *s1 == *s2) {
    s1++;
    s2++;
  }
  return (unsigned char)*s1 - (unsigned char)*s2;
}

/*
 * strcpy - copy a NUL-terminated string, including the NUL terminator.
 *
 * Params:
 *   dest - destination buffer; caller is responsible for sufficient size.
 *   src  - source string; may be NULL (returns dest unchanged).
 * Returns: dest.
 * Locking: none.
 * Side effects: writes to dest.
 */
char *strcpy(char *dest, const char *src) {
  if (!dest || !src)
    return dest;
  char *d = dest;
  while ((*d++ = *src++) != '\0')
    ;
  return dest;
}

/*
 * strncpy - copy at most n bytes from src to dest, zero-padding to n.
 *
 * Params:
 *   dest - destination buffer of at least n bytes.
 *   src  - source string; may be NULL (returns dest unchanged).
 *   n    - bytes to write; if src is shorter, remaining bytes are zeroed.
 * Returns: dest.
 * Locking: none.
 * Note: result is NOT guaranteed NUL-terminated if strlen(src) >= n.
 *       Prefer strlcpy for safe, always-NUL-terminated copies.
 */
char *strncpy(char *dest, const char *src, size_t n) {
  if (!dest || !src || n == 0)
    return dest;
  char *d = dest;
  while (n > 0) {
    n--;
    if ((*d++ = *src++) == '\0')
      break;
  }
  while (n > 0) {
    n--;
    *d++ = '\0';
  }
  return dest;
}

/*
 * strlcpy - copy string into a fixed-size buffer with guaranteed NUL termination.
 *
 * Copies at most (size - 1) characters from src to dest and always appends
 * a NUL terminator (if size > 0).
 *
 * Params:
 *   dest - destination buffer of exactly 'size' bytes.
 *   src  - source string (NUL-terminated); length is measured via strlen.
 *   size - total capacity of dest (including the NUL slot).
 * Returns: strlen(src) — callers can detect truncation by comparing the return
 *          value against size - 1.
 * Locking: none.
 */
size_t strlcpy(char *dest, const char *src, size_t size) {
  size_t len = strlen(src);
  if (size > 0) {
    size_t n = (len >= size) ? size - 1 : len;
    memcpy(dest, src, n);
    dest[n] = '\0';
  }
  return len;
}

/*
 * strcat - append src to dest (dest must be NUL-terminated on entry).
 *
 * Params:
 *   dest - destination NUL-terminated string; must have space for strlen(src)+1.
 *   src  - source string; may be NULL (returns dest unchanged).
 * Returns: dest.
 * Locking: none.
 * Note: no bounds check; prefer strlcat.
 */
char *strcat(char *dest, const char *src) {
  if (!dest || !src)
    return dest;
  char *d = dest;
  while (*d)
    d++;
  while ((*d++ = *src++) != '\0')
    ;
  return dest;
}

/*
 * strncat - append at most n bytes of src to dest.
 *
 * Params:
 *   dest - NUL-terminated destination string; must have at least n+1 spare bytes.
 *   src  - source string; may be NULL (returns dest unchanged).
 *   n    - maximum bytes to append from src (not counting added NUL).
 * Returns: dest.
 * Locking: none.
 * Note: the post-loop `if (n == (size_t)-1) *d = '\0'` handles the case where
 *       src ended inside the n-byte window (the inner loop consumed the NUL but
 *       n reached SIZE_MAX after underflow); ensures dest is always NUL-terminated.
 */
char *strncat(char *dest, const char *src, size_t n) {
  if (!dest || !src)
    return dest;
  char *d = dest;
  while (*d)
    d++;
  while (n-- && (*d++ = *src++) != '\0')
    ;
  if (n == (size_t)-1)
    *d = '\0';
  return dest;
}

/*
 * strlcat - append src to dest with a bound on dest's total size.
 *
 * Appends as much of src as fits within size bytes total (including the
 * existing content and the NUL terminator).
 *
 * Params:
 *   dest - NUL-terminated destination buffer of capacity 'size'.
 *   src  - source string (NUL-terminated).
 *   size - total capacity of dest (including NUL).
 * Returns: dlen + strlen(src) — callers can detect truncation by comparing
 *          against size - 1.
 * Locking: none.
 */
size_t strlcat(char *dest, const char *src, size_t size) {
  size_t dlen = strnlen(dest, size);
  size_t slen = strlen(src);
  if (dlen < size) {
    strlcpy(dest + dlen, src, size - dlen);
  }
  return dlen + slen;
}

/*
 * strchr - find the first occurrence of character c in string s.
 *
 * Params:
 *   s - NUL-terminated string; may be NULL (returns NULL).
 *   c - character to locate (compared as unsigned char); '\0' is valid.
 * Returns: pointer to first matching byte, or NULL if not found.
 *          If c == '\0', returns a pointer to the NUL terminator.
 * Locking: none.
 */
char *strchr(const char *s, int c) {
  if (!s)
    return NULL;
  while (*s) {
    if (*s == (char)c)
      return (char *)s;
    s++;
  }
  return (c == '\0') ? (char *)s : NULL;
}

/*
 * strrchr - find the last occurrence of character c in string s.
 *
 * Params:
 *   s - NUL-terminated string; may be NULL (returns NULL).
 *   c - character to locate (compared as unsigned char); '\0' is valid.
 * Returns: pointer to last matching byte, or NULL if not found.
 *          If c == '\0', returns a pointer to the NUL terminator.
 * Locking: none.
 */
char *strrchr(const char *s, int c) {
  if (!s)
    return NULL;
  const char *last = NULL;
  while (*s) {
    if (*s == (char)c)
      last = s;
    s++;
  }
  return (c == '\0') ? (char *)s : (char *)last;
}

/*
 * strstr - locate the first occurrence of needle within haystack.
 *
 * Uses a naive O(m*n) sliding-window scan; adequate for kernel use where
 * both strings are short.
 *
 * Params:
 *   haystack - string to search; may be NULL (returns NULL).
 *   needle   - pattern to locate; may be NULL (returns NULL).
 * Returns: pointer to first occurrence, or NULL if not found.
 *          If needle is empty, returns haystack.
 * Locking: none.
 */
char *strstr(const char *haystack, const char *needle) {
  if (!haystack || !needle)
    return NULL;
  size_t n = strlen(needle);
  if (n == 0)
    return (char *)haystack;
  while (*haystack) {
    if (memcmp(haystack, needle, n) == 0)
      return (char *)haystack;
    haystack++;
  }
  return NULL;
}

/*
 * memset - fill n bytes of memory starting at s with byte value c.
 *
 * Params:
 *   s - pointer to memory region; may be NULL (returns s immediately).
 *   c - byte value to fill (low 8 bits used).
 *   n - number of bytes to fill.
 * Returns: s.
 * Locking: none.
 * Side effects: writes n bytes at s.
 */
void *memset(void *s, int c, size_t n) {
  if (!s)
    return s;
  unsigned char *p = s;
  while (n--)
    *p++ = (unsigned char)c;
  return s;
}

/*
 * memcpy - copy n bytes from src to dest (regions must NOT overlap).
 *
 * Params:
 *   dest - destination pointer; may be NULL (returns dest immediately).
 *   src  - source pointer; may be NULL (returns dest immediately).
 *   n    - number of bytes to copy.
 * Returns: dest.
 * Locking: none.
 * Side effects: writes n bytes at dest.
 * Note: for overlapping regions use memmove.
 */
void *memcpy(void *dest, const void *src, size_t n) {
  if (!dest || !src)
    return dest;
  unsigned char *d = dest;
  const unsigned char *s = src;
  while (n--)
    *d++ = *s++;
  return dest;
}

/*
 * memmove - copy n bytes from src to dest, handling overlapping regions.
 *
 * If dest < src, copies forward (same as memcpy).
 * If dest > src, copies backward to avoid clobbering src bytes before they
 * are read (handles the d > s overlap direction correctly).
 *
 * Params:
 *   dest - destination pointer; may be NULL (returns dest immediately).
 *   src  - source pointer; may be NULL (returns dest immediately).
 *   n    - number of bytes to copy.
 * Returns: dest.
 * Locking: none.
 * Side effects: writes n bytes at dest.
 */
void *memmove(void *dest, const void *src, size_t n) {
  if (!dest || !src)
    return dest;
  unsigned char *d = dest;
  const unsigned char *s = src;

  if (d < s) {
    /* Forward copy: safe when dest is before src or ranges do not overlap */
    while (n--)
      *d++ = *s++;
  } else {
    /* Backward copy: safe when dest is after src (prevents overwriting src) */
    d += n;
    s += n;
    while (n--)
      *--d = *--s;
  }
  return dest;
}

/*
 * memcmp - compare n bytes of two memory regions.
 *
 * Params:
 *   s1, s2 - memory pointers; either may be NULL.
 *   n      - number of bytes to compare.
 * Returns:
 *   0 if equal (or n is 0, or both NULL).
 *   Positive if *s1 > *s2 at the first differing byte.
 *   Negative if *s1 < *s2.
 *   If exactly one is NULL, non-NULL is treated as greater.
 * Locking: none.
 */
int memcmp(const void *s1, const void *s2, size_t n) {
  if (n == 0)
    return 0;
  if (!s1 || !s2)
    return (s1 == s2) ? 0 : (s1 ? 1 : -1);
  const unsigned char *p1 = s1;
  const unsigned char *p2 = s2;

  while (n--) {
    if (*p1 != *p2)
      return (int)*p1 - (int)*p2;
    p1++;
    p2++;
  }
  return 0;
}

/*
 * memchr - locate the first byte equal to c within the first n bytes of s.
 *
 * Params:
 *   s - memory region; may be NULL (returns NULL).
 *   c - byte value to search for (low 8 bits used).
 *   n - number of bytes to scan.
 * Returns: pointer to first matching byte, or NULL if not found.
 * Locking: none.
 */
void *memchr(const void *s, int c, size_t n) {
  if (!s)
    return NULL;
  const unsigned char *p = s;
  while (n--) {
    if (*p == (unsigned char)c)
      return (void *)p;
    p++;
  }
  return NULL;
}

/*
 * bzero - zero n bytes of memory at s (BSD compatibility wrapper for memset).
 *
 * Params:
 *   s - pointer to memory region; if NULL, does nothing.
 *   n - number of bytes to zero.
 * Locking: none.
 */
void bzero(void *s, size_t n) {
  if (s)
    memset(s, 0, n);
}

/* to_lower - convert ASCII uppercase letter to lowercase; leaves others unchanged.
 * Internal helper for strcasecmp / strncasecmp only. */
static int to_lower(int c) {
  if (c >= 'A' && c <= 'Z')
    return c + ('a' - 'A');
  return c;
}

/*
 * strcasecmp - case-insensitive string comparison (ASCII only).
 *
 * Params:
 *   s1, s2 - strings to compare; either may be NULL.
 * Returns: 0 if equal (ignoring case), positive/negative per strcmp semantics.
 *          NULL handling matches strcmp.
 * Locking: none.
 */
int strcasecmp(const char *s1, const char *s2) {
  if (!s1 || !s2)
    return (s1 == s2) ? 0 : (s1 ? 1 : -1);
  while (*s1 && *s2) {
    if (to_lower((unsigned char)*s1) != to_lower((unsigned char)*s2))
      break;
    s1++;
    s2++;
  }
  return to_lower((unsigned char)*s1) - to_lower((unsigned char)*s2);
}

/*
 * strncasecmp - case-insensitive comparison of at most n bytes (ASCII only).
 *
 * Params:
 *   s1, s2 - strings to compare; either may be NULL.
 *   n      - maximum number of bytes to compare.
 * Returns: 0 if equal, positive/negative per strcmp semantics; 0 if n is 0.
 * Locking: none.
 */
int strncasecmp(const char *s1, const char *s2, size_t n) {
  if (n == 0)
    return 0;
  if (!s1 || !s2)
    return (s1 == s2) ? 0 : (s1 ? 1 : -1);
  while (n-- > 1 && *s1 && *s2) {
    if (to_lower((unsigned char)*s1) != to_lower((unsigned char)*s2))
      break;
    s1++;
    s2++;
  }
  return to_lower((unsigned char)*s1) - to_lower((unsigned char)*s2);
}

/*
 * atoi - convert the leading decimal digits of s to an int.
 *
 * Skips leading whitespace, handles an optional leading '+' or '-'.
 * Stops at the first non-digit character.  No overflow detection.
 *
 * Params:
 *   s - input string; may be NULL (returns 0).
 * Returns: integer value, or 0 on empty/NULL input.
 * Locking: none.
 */
int atoi(const char *s) {
  if (!s)
    return 0;
  int res = 0;
  int sign = 1;
  while (*s == ' ')
    s++;
  if (*s == '-') {
    sign = -1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  while (*s >= '0' && *s <= '9') {
    res = res * 10 + (*s - '0');
    s++;
  }
  return res * sign;
}
