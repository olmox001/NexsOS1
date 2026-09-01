/*
 * include/api/byteswap.h
 * Standard byte swap macros for NexsOS1.
 */

#ifndef _BYTESWAP_H
#define _BYTESWAP_H

#include <stdint.h>

#define bswap_16(x) __builtin_bswap16(x)
#define bswap_32(x) __builtin_bswap32(x)
#define bswap_64(x) __builtin_bswap64(x)

#endif /* _BYTESWAP_H */
