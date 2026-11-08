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

#if defined __GNUC__ && __GNUC__ >= 2 && !defined _GNULIB_OS1_GLUE_IMPL
extern void __error_noreturn(int status, int errnum, const char *format, ...)
    __asm__("error") __attribute__((__format__(__printf__, 3, 4), __noreturn__));

extern void __error_at_line_noreturn(int status, int errnum, const char *filename,
                                     unsigned int linenumber, const char *format, ...)
    __asm__("error_at_line") __attribute__((__format__(__printf__, 5, 6), __noreturn__));

# if defined __OPTIMIZE__ && defined __va_arg_pack
extern void __error_alias(int status, int errnum, const char *format, ...)
    __asm__("error") __attribute__((__format__(__printf__, 3, 4)));

extern void __error_at_line_alias(int status, int errnum, const char *filename,
                                  unsigned int linenumber, const char *format, ...)
    __asm__("error_at_line") __attribute__((__format__(__printf__, 5, 6)));

static __inline__ __attribute__((__always_inline__)) void
error(int status, int errnum, const char *format, ...)
{
  if (__builtin_constant_p(status) && status != 0)
    __error_noreturn(status, errnum, format, __va_arg_pack());
  else
    __error_alias(status, errnum, format, __va_arg_pack());
}

static __inline__ __attribute__((__always_inline__)) void
error_at_line(int status, int errnum, const char *filename,
              unsigned int linenumber, const char *format, ...)
{
  if (__builtin_constant_p(status) && status != 0)
    __error_at_line_noreturn(status, errnum, filename, linenumber, format, __va_arg_pack());
  else
    __error_at_line_alias(status, errnum, filename, linenumber, format, __va_arg_pack());
}
# endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* _ERROR_H */
