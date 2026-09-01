#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <posix_types.h>

/* Device major/minor types (POSIX <sys/types.h>) */
typedef unsigned int major_t;
typedef unsigned int minor_t;

/* 64-bit offset type (alias for off_t on 64-bit systems) */
#define off64_t off_t
#define loff_t  off_t

#endif
