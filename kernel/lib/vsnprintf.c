#ifdef KERNEL
#include <kernel/printk.h>
#include <kernel/types.h>
#else
#include <os1.h>
#endif
#include <stdarg.h>

/* Number conversion flags */
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

    if (num == 0) {
        tmp[i++] = '0';
    } else {
        while (num != 0) {
            tmp[i++] = digits[num % base];
            num /= base;
        }
    }

    /* Handle precision: fill with '0' digits */
    while (i < precision && i < 64) {
        tmp[i++] = '0';
    }

    width -= i;

    /* Padding */
    if (!(flags & (FLAG_ZEROPAD | FLAG_LEFT))) {
        while (width-- > 0 && written < (int)size - 1) buf[written++] = ' ';
    }

    /* Sign/Prefix padding */
    if (flags & FLAG_SIGN) {
        if (written < (int)size - 1) buf[written++] = '-';
    } else if (flags & FLAG_PLUS) {
        if (written < (int)size - 1) buf[written++] = '+';
    } else if (flags & FLAG_SPACE) {
        if (written < (int)size - 1) buf[written++] = ' ';
    }

    if (flags & FLAG_ZEROPAD) {
        while (width-- > 0 && written < (int)size - 1) buf[written++] = '0';
    }

    /* Digits (reversed) */
    while (i > 0 && written < (int)size - 1) {
        buf[written++] = tmp[--i];
    }

    if (flags & FLAG_LEFT) {
        while (width-- > 0 && written < (int)size - 1) buf[written++] = ' ';
    }

    return written;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    int written = 0;
    int width, precision;
    int flags;
    uint64_t num;
    const char *s;

    if (size == 0) return 0;

    while (*fmt && written < (int)size - 1) {
        if (*fmt != '%') {
            buf[written++] = *fmt++;
            continue;
        }

        fmt++; /* Skip '%' */

        /* Parse flags */
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

        /* Width */
        width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Precision */
        precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Length modifiers */
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

        /* Conversion specifier */
        switch (*fmt) {
            case 'c':
                if (written < (int)size - 1) buf[written++] = (char)va_arg(args, int);
                break;

            case 's':
                s = va_arg(args, const char *);
                if (!s) s = "(null)";
                while (*s && written < (int)size - 1) buf[written++] = *s++;
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
                written += print_num(buf + written, size - written, num, 10, width, precision, flags);
                break;

            case 'u':
            case 'x':
            case 'X':
                if (is_long == 2)      num = va_arg(args, uint64_t);
                else if (is_long == 1) num = va_arg(args, unsigned long);
                else                  num = va_arg(args, unsigned int);
                
                if (*fmt == 'X') flags |= FLAG_UPPERCASE;
                written += print_num(buf + written, size - written, num, (*fmt == 'u' ? 10 : 16), width, precision, flags);
                break;

            case 'p':
                num = (uint64_t)va_arg(args, void *);
                if (written < (int)size - 2) {
                    buf[written++] = '0';
                    buf[written++] = 'x';
                }
                written += print_num(buf + written, size - written, num, 16, 16, 16, FLAG_ZEROPAD);
                break;

            case '%':
                if (written < (int)size - 1) buf[written++] = '%';
                break;

            default:
                if (written < (int)size - 1) buf[written++] = '%';
                if (written < (int)size - 1) buf[written++] = *fmt;
                break;
        }
        fmt++;
    }

    buf[written] = '\0';
    return written;
}
