#ifndef _STDLIB_H
#define _STDLIB_H

#include <os1.h>
#include <sys/stat.h>

void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);
void exit(int status);
long strtol(const char *nptr, char **endptr, int base);
int stat(const char *path, struct stat *buf);
int mkdir(const char *path, mode_t mode);
double atof(const char *nptr);
int abs(int j);
char *getenv(const char *name);
int system(const char *command);

#endif
