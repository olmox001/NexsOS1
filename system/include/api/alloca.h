/*
 * include/api/alloca.h
 * Stack allocation macros and declarations for NexsOS1.
 */

#ifndef _ALLOCA_H
#define _ALLOCA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __GNUC__
#define alloca(size) __builtin_alloca(size)
#else
void *alloca(size_t size);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _ALLOCA_H */
