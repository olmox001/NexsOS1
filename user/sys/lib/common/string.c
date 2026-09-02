/*
 * lib/common/string.c – Freestanding string/memory utilities
 * Condiviso tra kernel e userland (risolve USR-LIB-01).
 * Nessuna dipendenza da header del kernel.
 */
#include <stddef.h>
#include <stdint.h>

/* Prototipi per funzioni non standard (richiesti da -Wmissing-prototypes) */
size_t strnlen(const char *s, size_t maxlen);
size_t strlcpy(char *dest, const char *src, size_t size);
size_t strlcat(char *dest, const char *src, size_t size);
void bzero(void *s, size_t n);

size_t strlen(const char *s) {
    if (!s) return 0;
    const char *p = s;
    while (*p) p++;
    return p - s;
}

size_t strnlen(const char *s, size_t maxlen) {
    if (!s) return 0;
    const char *p = s;
    while (maxlen-- && *p) p++;
    return p - s;
}

int strcmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    if (!s1 || !s2) return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    while (n-- > 1 && *s1 && *s1 == *s2) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

char *strcpy(char *dest, const char *src) {
    if (!dest || !src) return dest;
    char *d = dest;
    while ((*d++ = *src++) != '\0') ;
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    if (!dest || !src || n == 0) return dest;
    char *d = dest;
    while (n > 0) {
        n--;
        if ((*d++ = *src++) == '\0') break;
    }
    while (n > 0) { n--; *d++ = '\0'; }
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
    if (!dest || !src) return dest;
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != '\0') ;
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    if (!dest || !src) return dest;
    char *d = dest;
    while (*d) d++;
    while (n-- && (*d++ = *src++) != '\0') ;
    if (n == (size_t)-1) *d = '\0';
    return dest;
}

size_t strlcat(char *dest, const char *src, size_t size) {
    size_t dlen = strnlen(dest, size);
    size_t slen = strlen(src);
    if (dlen < size) strlcpy(dest + dlen, src, size - dlen);
    return dlen + slen;
}

char *strchr(const char *s, int c) {
    if (!s) return NULL;
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    if (!s) return NULL;
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (c == '\0') ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!haystack || !needle) return NULL;
    size_t n = strlen(needle);
    if (n == 0) return (char *)haystack;
    while (*haystack) {
        if (memcmp(haystack, needle, n) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

void *memset(void *s, int c, size_t n) {
    if (!s) return s;
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    if (!dest || !src) return dest;
    unsigned char *d = dest;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    if (!dest || !src) return dest;
    unsigned char *d = dest;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    if (n == 0) return 0;
    if (!s1 || !s2) return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    const unsigned char *p1 = s1, *p2 = s2;
    while (n--) {
        if (*p1 != *p2) return (int)*p1 - (int)*p2;
        p1++; p2++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    if (!s) return NULL;
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}

void bzero(void *s, size_t n) {
    if (s) memset(s, 0, n);
}

int atoi(const char *s) {
    if (!s) return 0;
    int res = 0, sign = 1;
    while (*s == ' ') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        res = res * 10 + (*s - '0');
        s++;
    }
    return res * sign;
}

int strcasecmp(const char *s1, const char *s2) {
    if (!s1 || !s2) return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    while (*s1 && *s2) {
        unsigned char c1 = (unsigned char)*s1, c2 = (unsigned char)*s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 'a' - 'A';
        if (c2 >= 'A' && c2 <= 'Z') c2 += 'a' - 'A';
        if (c1 != c2) return (int)c1 - (int)c2;
        s1++; s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    if (n == 0) return 0;
    if (!s1 || !s2) return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    while (n-- && *s1 && *s2) {
        unsigned char c1 = (unsigned char)*s1, c2 = (unsigned char)*s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 'a' - 'A';
        if (c2 >= 'A' && c2 <= 'Z') c2 += 'a' - 'A';
        if (c1 != c2) return (int)c1 - (int)c2;
        s1++; s2++;
    }
    if (n == (size_t)-1) return 0; // n era 0? non dovrebbe accadere
    return (unsigned char)*s1 - (unsigned char)*s2;
}