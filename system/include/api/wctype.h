/*
 * include/api/wctype.h
 * Minimal wide-character classification API for NexsOS1/Gnulib.
 */

#ifndef _WCTYPE_H
#define _WCTYPE_H

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WEOF
#define WEOF ((wint_t)(0xffffffffu))
#endif

typedef unsigned long wctype_t;
typedef unsigned long wctrans_t;

int iswalnum(wint_t wc);
int iswalpha(wint_t wc);
int iswblank(wint_t wc);
int iswcntrl(wint_t wc);
int iswdigit(wint_t wc);
int iswgraph(wint_t wc);
int iswlower(wint_t wc);
int iswprint(wint_t wc);
int iswpunct(wint_t wc);
int iswspace(wint_t wc);
int iswupper(wint_t wc);
int iswxdigit(wint_t wc);

wint_t towlower(wint_t wc);
wint_t towupper(wint_t wc);

wctype_t wctype(const char *property);
int iswctype(wint_t wc, wctype_t desc);

#ifdef __cplusplus
}
#endif

#endif /* _WCTYPE_H */
