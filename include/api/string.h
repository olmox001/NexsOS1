#ifndef _STRING_H
#define _STRING_H

#include <os1.h>

size_t strlen(const char *s);
char *strcpy(char *dest, const char *src) __attribute__((deprecated("use strlcpy/strlcat")));
char *strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);
char *strdup(const char *s);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strcat(char *dest, const char *src) __attribute__((deprecated("use strlcpy/strlcat")));
char *strncat(char *dest, const char *src, size_t n);
char *strtok(char *str, const char *delim);
char *strtok_r(char *str, const char *delim, char **saveptr);
char *strerror(int errnum);
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memchr(const void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
size_t strspn(const char *s, const char *accept);
char *strpbrk(const char *s, const char *accept);

#endif
