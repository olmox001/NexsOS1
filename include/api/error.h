/*
 * include/api/error.h
 * GNU-compatible error reporting functions for NexsOS1.
 */

#ifndef _ERROR_H
#define _ERROR_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned int error_message_count;
extern int error_one_per_line;
extern void (*error_print_progname)(void);

void error(int status, int errnum, const char *format, ...)
    __attribute__((__format__(__printf__, 3, 4)));

void error_at_line(int status, int errnum, const char *filename,
                   unsigned int linenumber, const char *format, ...)
    __attribute__((__format__(__printf__, 5, 6)));

void verror(int status, int errnum, const char *format, va_list args)
    __attribute__((__format__(__printf__, 3, 0)));

void verror_at_line(int status, int errnum, const char *filename,
                    unsigned int linenumber, const char *format, va_list args)
    __attribute__((__format__(__printf__, 5, 0)));

#ifdef __cplusplus
}
#endif

#endif /* _ERROR_H */
