#ifndef _STDLIB_H
#define _STDLIB_H

#include <os1.h>
#include <sys/stat.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);
void exit(int status);
void _Exit(int status);
void abort(void);
long strtol(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
int atoi(const char *nptr);
long atol(const char *nptr);
long long atoll(const char *nptr);
int stat(const char *path, struct stat *buf);
int mkdir(const char *path, mode_t mode);
double atof(const char *nptr);
int abs(int j);
long labs(long j);
char *getenv(const char *name);
int system(const char *command);
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

#endif
