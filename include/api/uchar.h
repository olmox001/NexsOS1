/*
 * include/api/uchar.h
 * Unicode character types for NexsOS1.
 */

#ifndef _UCHAR_H
#define _UCHAR_H

#include <stdint.h>
#include <stddef.h>

/* Forward declaration of wint_t to avoid circular dependency with wchar.h */
typedef unsigned int wint_t;

#ifndef __cplusplus
typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;
#endif

#ifndef _MBSTATE_T_DEFINED
#define _MBSTATE_T_DEFINED 1
typedef struct {
    int __count;
    unsigned int __value;
} mbstate_t;
#endif

/* Gnulib's C32 properties and mappings use function-pointer descriptors when
 * wide characters are not represented as UCS-4 values. This matches the ABI
 * expected by the generated regex and locale sources in the NexsOS port.
 */
#if defined(__STDC_ISO_10646__) && !defined(_GL_SMALL_WCHAR_T)
typedef unsigned long c32_type_test_t;
typedef unsigned long c32_mapping_t;
#else
typedef int (*c32_type_test_t)(wint_t wc);
typedef wint_t (*c32_mapping_t)(wint_t wc);
#endif

/* Prototypes for wide character utilities used by Coreutils */
wint_t btoc32(int c);
int c32isalnum(wint_t wc);
int c32isalpha(wint_t wc);
int c32isblank(wint_t wc);
int c32iscntrl(wint_t wc);
int c32isdigit(wint_t wc);
int c32isgraph(wint_t wc);
int c32islower(wint_t wc);
int c32isprint(wint_t wc);
int c32ispunct(wint_t wc);
int c32isspace(wint_t wc);
int c32isupper(wint_t wc);
int c32isxdigit(wint_t wc);
int c32width(char32_t wc);
wint_t c32tolower(wint_t wc);
wint_t c32toupper(wint_t wc);
size_t c32rtomb(char *s, char32_t wc, mbstate_t *ps);
size_t mbrtoc32(char32_t *pwc, const char *s, size_t n, mbstate_t *ps);
c32_type_test_t c32_get_type_test(const char *name);
int c32_apply_type_test(wint_t wc, c32_type_test_t property);
c32_mapping_t c32_get_mapping(const char *name);
wint_t c32_apply_mapping(wint_t wc, c32_mapping_t mapping);

#endif /* _UCHAR_H */
