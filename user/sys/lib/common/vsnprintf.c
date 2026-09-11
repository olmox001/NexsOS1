/*
 * lib/common/vsnprintf.c – Formatted output (vsnprintf/vsscanf)
 * Condiviso tra kernel e userland (risolve USR-LIB-01).
 * Nessuna dipendenza da header del kernel.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* Prototipi privati */
static int print_num(char *buf, size_t size, uint64_t num, int base,
                     int width, int precision, int flags);

/* Flag per print_num */
#define FLAG_ZEROPAD   0x01
#define FLAG_LEFT      0x02
#define FLAG_PLUS      0x04
#define FLAG_SPACE     0x08
#define FLAG_SPECIAL   0x10
#define FLAG_UPPERCASE 0x20
#define FLAG_SIGN      0x40

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    int written = 0, needed = 0;
    int width, precision, flags;
    uint64_t num;
    const char *s;
    if (size == 0 && buf != NULL) return 0;

    while (*fmt) {
        if (*fmt != '%') {
            if (size > 0 && written < (int)size - 1) buf[written++] = *fmt;
            needed++;
            fmt++;
            continue;
        }
        fmt++; /* skip '%' */

        flags = 0;
        while (1) {
            if (*fmt == '-')      flags |= FLAG_LEFT;
            else if (*fmt == '+') flags |= FLAG_PLUS;
            else if (*fmt == ' ') flags |= FLAG_SPACE;
            else if (*fmt == '#') flags |= FLAG_SPECIAL;
            else if (*fmt == '0') flags |= FLAG_ZEROPAD;
            else break;
            fmt++;
        }

        width = 0;
        if (*fmt == '*') {
            fmt++;
            width = va_arg(args, int);
            if (width < 0) { flags |= FLAG_LEFT; width = -width; }
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        precision = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                fmt++;
                precision = va_arg(args, int);
                if (precision < 0) precision = -1;
            } else {
                precision = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') { is_long = 2; fmt++; }
        } else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') {
            is_long = (sizeof(size_t) == 8) ? 2 : 1;
            fmt++;
        }

        switch (*fmt) {
            case 'c': {
                if (size > 0 && written < (int)size - 1)
                    buf[written++] = (char)va_arg(args, int);
                needed++;
                break;
            }
            case 's': {
                s = va_arg(args, const char *);
                if (!s) s = "(null)";
                int len = 0;
                while (s[len]) {
                    if (precision >= 0 && len >= precision) break;
                    len++;
                }
                if (!(flags & FLAG_LEFT)) {
                    while (len < width) {
                        if (size > 0 && written < (int)size - 1) buf[written++] = ' ';
                        needed++;
                        width--;
                    }
                }
                for (int j = 0; j < len; j++) {
                    if (size > 0 && written < (int)size - 1) buf[written++] = s[j];
                    needed++;
                }
                while (len < width) {
                    if (size > 0 && written < (int)size - 1) buf[written++] = ' ';
                    needed++;
                    width--;
                }
                break;
            }
            case 'd':
            case 'i': {
                if (is_long == 2)      num = va_arg(args, int64_t);
                else if (is_long == 1) num = va_arg(args, long);
                else                  num = va_arg(args, int);
                if ((int64_t)num < 0) {
                    flags |= FLAG_SIGN;
                    num = -(int64_t)num;
                }
                int n_added = print_num(
                    size > 0 ? buf + written : NULL,
                    size > (size_t)written ? size - written : 0,
                    num, 10, width, precision, flags);
                needed += n_added;
                if (size > 0 && written < (int)size - 1) {
                    int add = size - 1 - written;
                    written += n_added < add ? n_added : add;
                }
                break;
            }
            case 'u':
            case 'x':
            case 'X': {
                if (is_long == 2)      num = va_arg(args, uint64_t);
                else if (is_long == 1) num = va_arg(args, unsigned long);
                else                  num = va_arg(args, unsigned int);
                if (*fmt == 'X') flags |= FLAG_UPPERCASE;
                int base = (*fmt == 'u') ? 10 : 16;
                int n_added = print_num(
                    size > 0 ? buf + written : NULL,
                    size > (size_t)written ? size - written : 0,
                    num, base, width, precision, flags);
                needed += n_added;
                if (size > 0 && written < (int)size - 1) {
                    int add = size - 1 - written;
                    written += n_added < add ? n_added : add;
                }
                break;
            }
            case 'p': {
                num = (uint64_t)va_arg(args, void *);
                char pb[32];
                int pl = 0;
                pb[pl++] = '0';
                pb[pl++] = 'x';
                pl += print_num(pb + pl, sizeof(pb) - pl, num, 16, 16, -1, FLAG_ZEROPAD);
                int pad = width - pl;
                if (!(flags & FLAG_LEFT)) {
                    for (int k = 0; k < pad; k++) {
                        if (size > 0 && written < (int)size - 1) buf[written++] = ' ';
                        needed++;
                    }
                    for (int k = 0; k < pl; k++) {
                        if (size > 0 && written < (int)size - 1) buf[written++] = pb[k];
                        needed++;
                    }
                } else {
                    for (int k = 0; k < pl; k++) {
                        if (size > 0 && written < (int)size - 1) buf[written++] = pb[k];
                        needed++;
                    }
                    for (int k = 0; k < pad; k++) {
                        if (size > 0 && written < (int)size - 1) buf[written++] = ' ';
                        needed++;
                    }
                }
                break;
            }
            case '%': {
                if (size > 0 && written < (int)size - 1) buf[written++] = '%';
                needed++;
                break;
            }
            default: {
                if (size > 0 && written < (int)size - 1) buf[written++] = '%';
                needed++;
                if (size > 0 && written < (int)size - 1) buf[written++] = *fmt;
                needed++;
                break;
            }
        }
        fmt++;
    }

    if (size > 0) buf[written] = '\0';
    return needed;
}

static int print_num(char *buf, size_t size, uint64_t num, int base,
                     int width, int precision, int flags) {
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = (flags & FLAG_UPPERCASE) ? digits_upper : digits_lower;
    char tmp[66];
    int i = 0, written = 0, needed = 0;

    if (num == 0) {
        tmp[i++] = '0';
    } else {
        while (num != 0) {
            tmp[i++] = digits[num % base];
            num /= base;
        }
    }

    while (i < precision && i < 64) tmp[i++] = '0';

    width -= i;
    if (flags & (FLAG_SIGN | FLAG_PLUS | FLAG_SPACE)) width--;

    if (!(flags & (FLAG_ZEROPAD | FLAG_LEFT))) {
        while (width > 0) {
            if (written < (int)size - 1) buf[written++] = ' ';
            needed++;
            width--;
        }
    }

    if (flags & FLAG_SIGN) {
        if (written < (int)size - 1) buf[written++] = '-';
        needed++;
    } else if (flags & FLAG_PLUS) {
        if (written < (int)size - 1) buf[written++] = '+';
        needed++;
    } else if (flags & FLAG_SPACE) {
        if (written < (int)size - 1) buf[written++] = ' ';
        needed++;
    }

    if (flags & FLAG_ZEROPAD) {
        while (width > 0) {
            if (written < (int)size - 1) buf[written++] = '0';
            needed++;
            width--;
        }
    }

    while (i > 0) {
        if (written < (int)size - 1) buf[written++] = tmp[--i];
        else i--;
        needed++;
    }

    if (flags & FLAG_LEFT) {
        while (width > 0) {
            if (written < (int)size - 1) buf[written++] = ' ';
            needed++;
            width--;
        }
    }

    return needed;
}


/*
 * vsscanf - simplified format scanner supporting %d, %x/%X, %s with widths.
 *
 * Supports:
 *   %d  - signed decimal integer -> int *
 *   %x, %X - unsigned hex integer -> unsigned int *; skips optional 0x prefix
 *   %s  - whitespace-delimited string; width limits chars consumed
 *
 * Whitespace in the format string matches zero or more whitespace chars in
 * the input.  Literal format characters must match exactly (returns early on
 * mismatch).  Returns the count of successfully assigned conversions.
 *
 * Missing specifiers: %f, %c, %ld, %u, %p, etc.  Callers using unsupported
 * format specifiers will silently skip the conversion.
 */
/* ==========================================================================
 * MODULE 5 cont'd — Formatting: the scanf family
 * ========================================================================== */
int vsscanf(const char *inp, const char *fmt0, va_list ap) {
  int nassigned = 0;
  const unsigned char *fmt = (const unsigned char *)fmt0;
  const char *p_inp = inp;

  while (*fmt) {
    if (isspace(*fmt)) {
      while (isspace(*p_inp))
        p_inp++;
      fmt++;
      continue;
    }
    if (*fmt != '%') {
      if (*p_inp != *fmt)
        return nassigned;
      p_inp++;
      fmt++;
      continue;
    }
    fmt++; /* skip % */
    /* Parse optional field width */
    int width = 0;
    while (isdigit(*fmt)) {
      width = width * 10 + (*fmt - '0');
      fmt++;
    }

    char c = *fmt++;
    if (c == 'd') {
      while (isspace(*p_inp))
        p_inp++;
      int *res = va_arg(ap, int *);
      *res = atoi(p_inp);
      nassigned++;
      while (isdigit(*p_inp) || *p_inp == '-')
        p_inp++;
    } else if (c == 'x' || c == 'X') {
      while (isspace(*p_inp))
        p_inp++;
      unsigned int *res = va_arg(ap, unsigned int *);
      unsigned int val = 0;
      if (p_inp[0] == '0' && (p_inp[1] == 'x' || p_inp[1] == 'X'))
        p_inp += 2;
      while (isxdigit(*p_inp)) {
        char dc = *p_inp++;
        if (isdigit(dc))
          val = (val << 4) | (dc - '0');
        else
          val = (val << 4) | (tolower(dc) - 'a' + 10);
      }
      *res = val;
      nassigned++;
    } else if (c == 's') {
      while (isspace(*p_inp))
        p_inp++;
      char *res = va_arg(ap, char *);
      /* R2/documented hazard, NOT a bug specific to this file: a bare `%s`
       * (width == 0, i.e. unspecified) has NO destination bound to check
       * against here — this is the real POSIX scanf("%s", ...) contract
       * (as unsafe as gets() without an explicit width, by the C standard's
       * own design, same reason noted for sprintf() above), so `%Ns` is the
       * caller's actual safety mechanism, not something this function can
       * retrofit without silently changing what %s means. */
      while (*p_inp && !isspace(*p_inp)) {
        *res++ = *p_inp++;
        if (width > 0 && --width == 0)
          break;
      }
      *res = '\0';
      nassigned++;
    }
    /* NOTE(USR-LIB-04): Other format specifiers (%f, %c, %ld, %u, etc.) are
     * silently skipped; the corresponding va_arg is NOT consumed, which may
     * desync the va_list for subsequent conversions. */
  }
  return nassigned;
}