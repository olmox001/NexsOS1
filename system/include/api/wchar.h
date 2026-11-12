/*
 * include/api/wchar.h
 * Wide-character types and functions for NexsOS1.
 */

#ifndef _WCHAR_H
#define _WCHAR_H

#include <stddef.h>
#include <stdint.h>
#include <uchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _WINT_T_DEFINED
#define _WINT_T_DEFINED 1
typedef unsigned int wint_t;
#endif

#ifndef WEOF
#define WEOF ((wint_t)(0xffffffffu))
#endif

size_t wcslen(const wchar_t *s);
wchar_t *wcscpy(wchar_t *dest, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n);
int wcscmp(const wchar_t *s1, const wchar_t *s2);
int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n);
wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n);
wint_t btowc(int c);
int wctob(wint_t c);
size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
int wcwidth(wchar_t wc);

#ifdef __cplusplus
}
#endif

#endif /* _WCHAR_H */
