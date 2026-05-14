/*
 * kernel/lib/string.c
 * String manipulation functions
 */
#include <kernel/string.h>
#include <kernel/types.h>

/*
 * strlen - calculate string length
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
 * strnlen - calculate string length with limit
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
 * strcmp - compare two strings
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
 * strncmp - compare two strings with limit
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
 * strcpy - copy string
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
 * strncpy - copy string with limit
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
 * strlcpy - safer string copy
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
 * strcat - concatenate strings
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
 * strncat - concatenate strings with limit
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
 * strlcat - safer string concatenation
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
 * strchr - find character in string
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
 * strrchr - find last occurrence of character
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
 * strstr - find substring
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
 * memset - fill memory with value
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
 * memcpy - copy memory
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
 * memmove - copy memory (overlapping safe)
 */
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

/*
 * memcmp - compare memory
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
 * memchr - find byte in memory
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
 * bzero - zero memory (BSD compatibility)
 */
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
