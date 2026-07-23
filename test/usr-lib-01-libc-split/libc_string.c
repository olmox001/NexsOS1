/*
 * user/sys/lib/libc_string.c
 * Userland string/memory primitives
 *
 * Purpose:
 *   Standalone userland implementation of the functions declared in
 *   include/api/string.h.  This file has NO dependency on kernel/lib/string.c
 *   or any kernel header — it is compiled as its own object (see Makefile:
 *   USER_LIB_O) and linked into every user ELF alongside lib.o, exactly like
 *   any other userland translation unit.
 *
 * History (NOTE USR-LIB-01, fixed):
 *   user/sys/lib/lib.c used to `#include "../../kernel/lib/string.c"`
 *   directly — a source-level #include of a kernel-tree file, with no
 *   compile-time boundary between kernel and userland.  This file replaces
 *   that: it is a userland-owned copy under user/, kept in sync with
 *   string.h by hand rather than by sharing a translation unit with the
 *   kernel.  kernel/lib/string.c is untouched and continues to serve the
 *   kernel image exactly as before.
 *
 * Invariants:
 *   - Every function that accepts a pointer checks it for NULL before
 *     dereferencing; callers may pass NULL and get a defined result.
 *   - No dynamic allocation; no global state.
 *   - Byte-at-a-time; no SIMD.
 */
#include <os1.h>

size_t strlen(const char *s) {
  if (!s)
    return 0;
  const char *p = s;
  while (*p)
    p++;
  return p - s;
}

size_t strnlen(const char *s, size_t maxlen) {
  if (!s)
    return 0;
  const char *p = s;
  while (maxlen-- && *p)
    p++;
  return p - s;
}

int strcmp(const char *s1, const char *s2) {
  if (!s1 || !s2)
    return (s1 == s2) ? 0 : (s1 ? 1 : -1);
  while (*s1 && *s1 == *s2) {
    s1++;
    s2++;
  }
  return (unsigned char)*s1 - (unsigned char)*s2;
}

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

char *strcpy(char *dest, const char *src) {
  if (!dest || !src)
    return dest;
  char *d = dest;
  while ((*d++ = *src++) != '\0')
    ;
  return dest;
}

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

size_t strlcpy(char *dest, const char *src, size_t size) {
  size_t len = strlen(src);
  if (size > 0) {
    size_t n = (len >= size) ? size - 1 : len;
    memcpy(dest, src, n);
    dest[n] = '\0';
  }
  return len;
}

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

size_t strlcat(char *dest, const char *src, size_t size) {
  size_t dlen = strnlen(dest, size);
  size_t slen = strlen(src);
  if (dlen < size) {
    strlcpy(dest + dlen, src, size - dlen);
  }
  return dlen + slen;
}

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

void *memset(void *s, int c, size_t n) {
  if (!s)
    return s;
  unsigned char *p = s;
  while (n--)
    *p++ = (unsigned char)c;
  return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
  if (!dest || !src)
    return dest;
  unsigned char *d = dest;
  const unsigned char *s = src;
  while (n--)
    *d++ = *s++;
  return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
  if (!dest || !src)
    return dest;
  unsigned char *d = dest;
  const unsigned char *s = src;

  if (d < s) {
    while (n--)
      *d++ = *s++;
  } else {
    d += n;
    s += n;
    while (n--)
      *--d = *--s;
  }
  return dest;
}

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

void bzero(void *s, size_t n) {
  if (s)
    memset(s, 0, n);
}

static int to_lower(int c) {
  if (c >= 'A' && c <= 'Z')
    return c + ('a' - 'A');
  return c;
}

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
