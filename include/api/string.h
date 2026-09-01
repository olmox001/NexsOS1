#ifndef _STRING_H
#define _STRING_H

#include <os1.h>

size_t strlen(const char *s);
bool str_endswith(const char *string, const char *suffix);
char *strcpy(char *dest, const char *src) __attribute__((deprecated("use strlcpy/strlcat")));
char *stpcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);

int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);
int strcoll(const char *s1, const char *s2);
size_t strxfrm(char *dest, const char *src, size_t n);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
char *strchr(const char *s, int c);

char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strcat(char *dest, const char *src) __attribute__((deprecated("use strlcpy/strlcat")));
char *strncat(char *dest, const char *src, size_t n);
char *strtok(char *str, const char *delim);
char *strtok_r(char *str, const char *delim, char **saveptr);
char *strerror(int errnum);
void *memset(void *s, int c, size_t n);
void *memset_explicit(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memchr(const void *s, int c, size_t n);
void *memrchr(const void *s, int c, size_t n);
void *rawmemchr(const void *s, int c);
void *mempcpy(void *dest, const void *src, size_t n);
void *memmem(const void *haystack, size_t haystack_len, const void *needle, size_t needle_len);
void *memchr2(const void *s, int c1, int c2, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);


#endif
