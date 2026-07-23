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
/* Environment (Phase 17).  Backed by the KERNEL's per-process block, reached
 * through the `sys.proc.<pid>.env.*` registry seam, with `sys.env.*` as the
 * machine-wide defaults underneath.  See the block comment in lib.c for the
 * layering and the one documented deviation (getenv's returned pointer is
 * valid for a bounded number of further getenv calls, not until setenv). */
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *string);
int clearenv(void);
/* env_names - NexsOS extension replacing POSIX `environ`: fills names[] with
 * this process's own variable NAMES (values via getenv) and returns the count.
 * `environ` is deliberately absent — the environment is not a userland array
 * here, and exposing one would mean publishing a snapshot that silently goes
 * stale. */
int env_names(char *names[], int max);
int system(const char *command);
/* Quote-aware command-line tokenizer (NexsOS extension): splits s into argv[]
 * on whitespace, keeping '...'/"..." spans as single tokens with the quotes
 * stripped.  Modifies s in place; returns argc.  See lib.c. */
int cmdline_split(char *s, char **argv, int max);
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));

#endif
