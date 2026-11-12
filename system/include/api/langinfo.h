/*
 * Minimal POSIX langinfo compatibility for NexsOS1.
 * This provides the symbols and nl_item constants that Gnulib/Coreutils expect
 * even though the freestanding userland libc does not yet implement a full
 * locale database.
 */

#ifndef _LANGINFO_H
#define _LANGINFO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int nl_item;

/* nl_langinfo items of the LC_CTYPE category */
#define CODESET     10000
#define RADIXCHAR   10001
#define DECIMAL_POINT RADIXCHAR
#define THOUSEP     10002
#define THOUSANDS_SEP THOUSEP
#define GROUPING    10114

/* nl_langinfo items of the LC_TIME category */
#define D_T_FMT     10003
#define D_FMT       10004
#define T_FMT       10005
#define T_FMT_AMPM  10006
#define AM_STR      10007
#define PM_STR      10008
#define DAY_1       10009
#define DAY_2       (DAY_1 + 1)
#define DAY_3       (DAY_1 + 2)
#define DAY_4       (DAY_1 + 3)
#define DAY_5       (DAY_1 + 4)
#define DAY_6       (DAY_1 + 5)
#define DAY_7       (DAY_1 + 6)
#define ABDAY_1     10016
#define ABDAY_2     (ABDAY_1 + 1)
#define ABDAY_3     (ABDAY_1 + 2)
#define ABDAY_4     (ABDAY_1 + 3)
#define ABDAY_5     (ABDAY_1 + 4)
#define ABDAY_6     (ABDAY_1 + 5)
#define ABDAY_7     (ABDAY_1 + 6)
#define MON_1       10023
#define MON_2       (MON_1 + 1)
#define MON_3       (MON_1 + 2)
#define MON_4       (MON_1 + 3)
#define MON_5       (MON_1 + 4)
#define MON_6       (MON_1 + 5)
#define MON_7       (MON_1 + 6)
#define MON_8       (MON_1 + 7)
#define MON_9       (MON_1 + 8)
#define MON_10      (MON_1 + 9)
#define MON_11      (MON_1 + 10)
#define MON_12      (MON_1 + 11)
#define ALTMON_1    10200
#define ALTMON_2    (ALTMON_1 + 1)
#define ALTMON_3    (ALTMON_1 + 2)
#define ALTMON_4    (ALTMON_1 + 3)
#define ALTMON_5    (ALTMON_1 + 4)
#define ALTMON_6    (ALTMON_1 + 5)
#define ALTMON_7    (ALTMON_1 + 6)
#define ALTMON_8    (ALTMON_1 + 7)
#define ALTMON_9    (ALTMON_1 + 8)
#define ALTMON_10   (ALTMON_1 + 9)
#define ALTMON_11   (ALTMON_1 + 10)
#define ALTMON_12   (ALTMON_1 + 11)
#define ABMON_1     10035
#define ABMON_2     (ABMON_1 + 1)
#define ABMON_3     (ABMON_1 + 2)
#define ABMON_4     (ABMON_1 + 3)
#define ABMON_5     (ABMON_1 + 4)
#define ABMON_6     (ABMON_1 + 5)
#define ABMON_7     (ABMON_1 + 6)
#define ABMON_8     (ABMON_1 + 7)
#define ABMON_9     (ABMON_1 + 8)
#define ABMON_10    (ABMON_1 + 9)
#define ABMON_11    (ABMON_1 + 10)
#define ABMON_12    (ABMON_1 + 11)
#define ABALTMON_1  10220
#define ABALTMON_2  (ABALTMON_1 + 1)
#define ABALTMON_3  (ABALTMON_1 + 2)
#define ABALTMON_4  (ABALTMON_1 + 3)
#define ABALTMON_5  (ABALTMON_1 + 4)
#define ABALTMON_6  (ABALTMON_1 + 5)
#define ABALTMON_7  (ABALTMON_1 + 6)
#define ABALTMON_8  (ABALTMON_1 + 7)
#define ABALTMON_9  (ABALTMON_1 + 8)
#define ABALTMON_10 (ABALTMON_1 + 9)
#define ABALTMON_11 (ABALTMON_1 + 10)
#define ABALTMON_12 (ABALTMON_1 + 11)
#define ERA         10047
#define ERA_D_FMT   10048
#define ERA_D_T_FMT 10049
#define ERA_T_FMT   10050
#define ALT_DIGITS  10051

/* nl_langinfo items of the LC_MONETARY category */
#define CRNCYSTR    10052
#define CURRENCY_SYMBOL   CRNCYSTR
#define INT_CURR_SYMBOL   10100
#define MON_DECIMAL_POINT 10101
#define MON_THOUSANDS_SEP 10102
#define MON_GROUPING      10103
#define POSITIVE_SIGN     10104
#define NEGATIVE_SIGN     10105
#define FRAC_DIGITS       10106
#define INT_FRAC_DIGITS   10107
#define P_CS_PRECEDES     10108
#define N_CS_PRECEDES     10109
#define P_SEP_BY_SPACE    10110
#define N_SEP_BY_SPACE    10111
#define P_SIGN_POSN       10112
#define N_SIGN_POSN       10113

/* nl_langinfo items of the LC_MESSAGES category */
#define YESEXPR     10053
#define NOEXPR      10054

char *nl_langinfo(nl_item item);

#ifdef __cplusplus
}
#endif

#endif /* _LANGINFO_H */
