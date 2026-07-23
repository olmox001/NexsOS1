/*
 * user/sys/lib/libc_vsnprintf.c
 * Userland bounded formatted string output (vsnprintf)
 *
 * Purpose:
 *   Standalone userland implementation of vsnprintf(), the single formatted-
 *   output primitive backing printf/snprintf/sprintf/vfprintf and (via Lua's
 *   LUAI_NUMFFORMAT) string.format.
 *
 * History (NOTE USR-LIB-01, fixed):
 *   user/sys/lib/lib.c used to `#include "../../kernel/lib/vsnprintf.c"`
 *   directly. This file is the userland-owned replacement: floating-point
 *   conversion (%f/%e/%g), which the kernel build excludes entirely (no FPU
 *   use in the kernel image), is unconditionally present here since userland
 *   always wants it. kernel/lib/vsnprintf.c is untouched and keeps serving
 *   the kernel's integer-only printk() path exactly as before.
 *
 * Supported specifiers:
 *   %c %s %d/%i %u %x/%X %p %f/%F %e/%E %g/%G %%
 *   Length modifiers: l, ll, z.
 *   Flags: - (left-align), + (force sign), ' ' (space sign), # (alternate),
 *          0 (zero-pad).
 *   Width: decimal, or '*' from an int argument (C99); negative '*' means
 *          left-justify.
 *   Precision: '.' + decimal, or '.*' from an int argument; negative '*'
 *          precision means "no precision".
 *
 * Known issues (inherited from the original shared implementation):
 *   LIB-VSNPRINTF-01  (W1 BUG)  Sign char emitted without decrementing width;
 *                     %05d of -42 produces "-00042" (6 chars) not "-0042".
 *   LIB-VSNPRINTF-02  (W1 REFINE) Returns chars written, not would-be length;
 *                     callers cannot detect truncation.
 *   LIB-VSNPRINTF-03  (W0 MISSING) %o (octal) is absent.
 *   LIB-VSNPRINTF-04  (W1 BAD-IMPL) %p width handling: see case 'p' below.
 */
#include <os1.h>
#include <stdarg.h>

#define FLAG_ZEROPAD   0x01
#define FLAG_LEFT      0x02
#define FLAG_PLUS      0x04
#define FLAG_SPACE     0x08
#define FLAG_SPECIAL   0x10
#define FLAG_UPPERCASE 0x20
#define FLAG_SIGN      0x40

static int print_num(char *buf, size_t size, uint64_t num, int base, int width, int precision, int flags) {
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = (flags & FLAG_UPPERCASE) ? digits_upper : digits_lower;

    char tmp[66];
    int i = 0;
    int written = 0;
    int needed = 0;

    if (num == 0) {
        tmp[i++] = '0';
    } else {
        while (num != 0) {
            tmp[i++] = digits[num % base];
            num /= base;
        }
    }

    while (i < precision && i < 64) {
        tmp[i++] = '0';
    }

    width -= i;
    if (flags & (FLAG_SIGN | FLAG_PLUS | FLAG_SPACE)) {
        width--;
    }

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
 * Floating-point conversion (%f/%F, %e/%E, %g/%G).
 * Digits come from double arithmetic only (no libm dependency): ~15-16
 * significant decimal digits are reliable, which covers Lua's 14-digit %g.
 */
static void fp_gen(double value, int ndigits, char *digs, int *decexp) {
    if (ndigits < 1) ndigits = 1;
    if (ndigits > 17) ndigits = 17;

    int e = 0;
    double v = value;
    while (v >= 10.0) { v /= 10.0; e++; }
    while (v < 1.0)   { v *= 10.0; e--; }

    int d[19];
    for (int i = 0; i <= ndigits; i++) {
        int digit = (int)v;
        if (digit < 0) digit = 0; else if (digit > 9) digit = 9;
        d[i] = digit;
        v = (v - (double)digit) * 10.0;
    }

    int carry = (d[ndigits] >= 5) ? 1 : 0;
    for (int i = ndigits - 1; i >= 0 && carry; i--) {
        d[i] += 1;
        if (d[i] == 10) { d[i] = 0; carry = 1; } else carry = 0;
    }
    if (carry) {
        for (int i = ndigits - 1; i > 0; i--) d[i] = d[i - 1];
        d[0] = 1;
        e++;
    }
    for (int i = 0; i < ndigits; i++) digs[i] = (char)('0' + d[i]);
    *decexp = e;
}

static int fp_exp(char *out, int n, int decexp, int upper) {
    out[n++] = upper ? 'E' : 'e';
    int ex = decexp;
    if (ex < 0) { out[n++] = '-'; ex = -ex; } else out[n++] = '+';
    char eb[8];
    int en = 0;
    if (ex == 0) eb[en++] = '0';
    while (ex > 0) { eb[en++] = (char)('0' + ex % 10); ex /= 10; }
    while (en < 2) eb[en++] = '0';
    while (en > 0) out[n++] = eb[--en];
    return n;
}

static int fp_format(char *out, double value, char spec, int prec, int flags,
                     int upper) {
    int n = 0;
    union { double d; uint64_t u; } bp;
    bp.d = value;
    int neg = (int)(bp.u >> 63);
    uint64_t expo = (bp.u >> 52) & 0x7ff;
    uint64_t mant = bp.u & 0xfffffffffffffULL;

    char sgn = neg ? '-' : (flags & FLAG_PLUS) ? '+'
                          : (flags & FLAG_SPACE) ? ' ' : 0;

    if (expo == 0x7ff) {
        if (mant) {
            const char *w = upper ? "NAN" : "nan";
            while (*w) out[n++] = *w++;
        } else {
            if (sgn) out[n++] = sgn;
            const char *w = upper ? "INF" : "inf";
            while (*w) out[n++] = *w++;
        }
        out[n] = '\0';
        return n;
    }

    if (neg) value = -value;
    if (prec < 0) prec = 6;
    if (sgn) out[n++] = sgn;

    char digs[20];
    int decexp;

    if (spec == 'e') {
        int ndig = prec + 1;
        if (value == 0.0) { for (int i = 0; i < 20; i++) digs[i] = '0'; decexp = 0; }
        else fp_gen(value, ndig, digs, &decexp);
        out[n++] = digs[0];
        if (prec > 0 || (flags & FLAG_SPECIAL)) out[n++] = '.';
        for (int i = 1; i <= prec; i++) out[n++] = (i < 20) ? digs[i] : '0';
        n = fp_exp(out, n, decexp, upper);
    } else if (spec == 'g') {
        int P = prec ? prec : 1;
        if (value == 0.0) { for (int i = 0; i < 20; i++) digs[i] = '0'; decexp = 0; }
        else fp_gen(value, P, digs, &decexp);
        int siglen = P;
        if (!(flags & FLAG_SPECIAL))
            while (siglen > 1 && digs[siglen - 1] == '0') siglen--;

        if (decexp < -4 || decexp >= P) {
            out[n++] = digs[0];
            if (siglen > 1 || (flags & FLAG_SPECIAL)) out[n++] = '.';
            for (int i = 1; i < siglen; i++) out[n++] = digs[i];
            n = fp_exp(out, n, decexp, upper);
        } else {
            int di = 0;
            if (decexp < 0) out[n++] = '0';
            else for (int p = decexp; p >= 0; p--)
                     out[n++] = (di < siglen) ? digs[di++] : '0';
            char frac[48];
            int fn = 0;
            if (decexp < 0) {
                for (int p = -1; p > decexp; p--) frac[fn++] = '0';
                for (int i = 0; i < siglen; i++) frac[fn++] = digs[i];
            } else {
                for (int i = decexp + 1; i < siglen; i++) frac[fn++] = digs[i];
            }
            if (fn > 0 || (flags & FLAG_SPECIAL)) {
                out[n++] = '.';
                for (int i = 0; i < fn; i++) out[n++] = frac[i];
            }
        }
    } else { /* 'f' */
        if (value == 0.0) { digs[0] = '0'; decexp = 0;
            for (int i = 1; i < 20; i++) digs[i] = '0'; }
        else {
            fp_gen(value, 1, digs, &decexp);
            int ndig = decexp + 1 + prec;
            if (ndig < 1) ndig = 1;
            if (ndig > 17) ndig = 17;
            fp_gen(value, ndig, digs, &decexp);
        }
        int ndig = decexp + 1 + prec;
        if (ndig < 1) ndig = 1;
        if (ndig > 17) ndig = 17;
        int di = 0;
        if (decexp < 0) out[n++] = '0';
        else for (int p = decexp; p >= 0; p--)
                 out[n++] = (di < ndig) ? digs[di++] : '0';
        if (prec > 0 || (flags & FLAG_SPECIAL)) out[n++] = '.';
        for (int p = -1; p >= -prec; p--) {
            int idx = decexp - p;
            out[n++] = (idx >= 0 && idx < ndig) ? digs[idx] : '0';
        }
    }
    out[n] = '\0';
    return n;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    int written = 0;
    int needed = 0;
    int width, precision;
    int flags;
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

        fmt++;

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
            if (width < 0) {
                flags |= FLAG_LEFT;
                width = -width;
            }
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
                if (precision < 0)
                    precision = -1;
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
            if (*fmt == 'l') {
                is_long = 2;
                fmt++;
            }
        } else if (*fmt == 'z') {
            is_long = (sizeof(size_t) == 8) ? 2 : 1;
            fmt++;
        }

        switch (*fmt) {
            case 'c':
                if (size > 0 && written < (int)size - 1) buf[written++] = (char)va_arg(args, int);
                needed++;
                break;

            case 's':
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

            case 'd':
            case 'i':
                if (is_long == 2)      num = va_arg(args, int64_t);
                else if (is_long == 1) num = va_arg(args, long);
                else                  num = va_arg(args, int);

                if ((int64_t)num < 0) {
                    flags |= FLAG_SIGN;
                    num = -(int64_t)num;
                }
                needed += print_num(size > 0 ? buf + written : NULL, size > (size_t)written ? size - written : 0, num, 10, width, precision, flags);
                if (size > 0 && written < (int)size - 1) {
                    int add = size - 1 - written;
                    int added = print_num(buf + written, size - written, num, 10, width, precision, flags);
                    written += added < add ? added : add;
                } else {
                    written += 0;
                }
                break;

            case 'u':
            case 'x':
            case 'X':
                if (is_long == 2)      num = va_arg(args, uint64_t);
                else if (is_long == 1) num = va_arg(args, unsigned long);
                else                  num = va_arg(args, unsigned int);

                if (*fmt == 'X') flags |= FLAG_UPPERCASE;

                int n_added = print_num(size > 0 ? buf + written : NULL, size > (size_t)written ? size - written : 0, num, (*fmt == 'u' ? 10 : 16), width, precision, flags);
                needed += n_added;
                if (size > 0 && written < (int)size - 1) {
                    int add = size - 1 - written;
                    written += n_added < add ? n_added : add;
                }
                break;

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

            case '%':
                if (size > 0 && written < (int)size - 1) buf[written++] = '%';
                needed++;
                break;

            case 'f': case 'F':
            case 'e': case 'E':
            case 'g': case 'G': {
                double dv = va_arg(args, double);
                char nb[512];
                int up = (*fmt >= 'A' && *fmt <= 'Z');
                char lspec = up ? (char)(*fmt + 32) : *fmt;
                int ln = fp_format(nb, dv, lspec, precision, flags, up);
                int pad = width - ln;
                if (!(flags & FLAG_LEFT)) {
                    if (flags & FLAG_ZEROPAD) {
                        int sk = (nb[0] == '-' || nb[0] == '+' || nb[0] == ' ') ? 1 : 0;
                        for (int k = 0; k < sk; k++) {
                            if (size > 0 && written < (int)size - 1) buf[written++] = nb[k];
                            needed++;
                        }
                        for (int k = 0; k < pad; k++) {
                            if (size > 0 && written < (int)size - 1) buf[written++] = '0';
                            needed++;
                        }
                        for (int k = sk; k < ln; k++) {
                            if (size > 0 && written < (int)size - 1) buf[written++] = nb[k];
                            needed++;
                        }
                    } else {
                        for (int k = 0; k < pad; k++) {
                            if (size > 0 && written < (int)size - 1) buf[written++] = ' ';
                            needed++;
                        }
                        for (int k = 0; k < ln; k++) {
                            if (size > 0 && written < (int)size - 1) buf[written++] = nb[k];
                            needed++;
                        }
                    }
                } else {
                    for (int k = 0; k < ln; k++) {
                        if (size > 0 && written < (int)size - 1) buf[written++] = nb[k];
                        needed++;
                    }
                    for (int k = 0; k < pad; k++) {
                        if (size > 0 && written < (int)size - 1) buf[written++] = ' ';
                        needed++;
                    }
                }
                break;
            }

            default:
                if (size > 0 && written < (int)size - 1) buf[written++] = '%';
                needed++;
                if (size > 0 && written < (int)size - 1) buf[written++] = *fmt;
                needed++;
                break;
        }
        fmt++;
    }

    if (size > 0) buf[written] = '\0';
    return needed;
}
