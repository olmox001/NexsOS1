/*
 * kernel/lib/vsnprintf.c
 * Bounded formatted string output (vsnprintf)
 *
 * Purpose:
 *   Implements vsnprintf() — the single formatted-output primitive used by
 *   printk(), snprintf(), and (via the userland build path) the os1 user library.
 *   All other formatted-output functions in the kernel are thin wrappers over
 *   this one.
 *
 * Role:
 *   This file is compiled in both kernel and userland modes:
 *     - KERNEL defined: pulls in kernel/printk.h and kernel/types.h.
 *     - KERNEL not defined: pulls in os1.h (userland).
 *   The same translation unit therefore serves both contexts.
 *
 * Supported specifiers:
 *   %c  %s  %d/%i  %u  %x/%X  %p  %%
 *   Length modifiers: l, ll, z.
 *   Flags: - (left-align), + (force sign), ' ' (space sign), # (alternate),
 *          0 (zero-pad).
 *   Width: decimal field width.
 *   Precision: decimal precision (for numeric digit count).
 *
 * Invariants:
 *   - Every write path guards `written < (int)size - 1` before writing.
 *   - buf[written] = '\0' is always written at exit (NUL termination).
 *   - No %n specifier is implemented (security: no arbitrary memory write).
 *
 * Known issues:
 *   LIB-VSNPRINTF-01  (W1 BUG)     Sign char emitted without decrementing width;
 *                                   %05d of -42 produces "-00042" (6 chars) not
 *                                   "-0042".  See print_num().
 *   LIB-VSNPRINTF-02  (W1 REFINE)  Returns chars written, not would-be length;
 *                                   callers cannot detect truncation. See vsnprintf.
 *   LIB-VSNPRINTF-03  (W0 MISSING) %o (octal) and %e/%f (float) are absent.
 *   LIB-VSNPRINTF-04  (W1 BAD-IMPL) %p hardcodes 16-digit width regardless of
 *                                   remaining buffer space.  See case 'p'.
 */
#ifdef KERNEL
#include <kernel/printk.h>
#include <kernel/types.h>
#else
#include <os1.h>
#endif
#include <stdarg.h>

/* Number conversion flags — bit-mask used by print_num() and the vsnprintf loop.
 * FLAG_ZEROPAD:   '0' flag: pad with '0' instead of ' ' when right-aligned.
 * FLAG_LEFT:      '-' flag: left-justify (overrides FLAG_ZEROPAD).
 * FLAG_PLUS:      '+' flag: always emit sign character even for non-negative.
 * FLAG_SPACE:     ' ' flag: prefix with a space for non-negative numbers.
 * FLAG_SPECIAL:   '#' flag: parsed but has no effect in the current implementation.
 * FLAG_UPPERCASE: uppercase hex digits (A-F); set for %X specifier.
 * FLAG_SIGN:      internal flag: set when the value is negative; causes '-' output.
 */
#define FLAG_ZEROPAD   0x01
#define FLAG_LEFT      0x02
#define FLAG_PLUS      0x04
#define FLAG_SPACE     0x08
#define FLAG_SPECIAL   0x10
#define FLAG_UPPERCASE 0x20
#define FLAG_SIGN      0x40

/*
 * print_num - format an unsigned integer into buf in the given base.
 *
 * Digits are accumulated in reverse into tmp[], then written forward.
 * Padding and sign/prefix characters are inserted between the width padding
 * and the digit string in the following order:
 *   [space padding] [sign or space] [zero padding] [digits] [left padding]
 *
 * Params:
 *   buf       - output buffer; write starts at buf[0].
 *   size      - remaining capacity of buf including NUL slot.
 *   num       - value to format (always unsigned at this point; sign already
 *               extracted by the caller and encoded in flags).
 *   base      - numeric base: 10 or 16.
 *   width     - minimum field width; 0 means no minimum.
 *   precision - minimum digit count (zero-pad with '0'); -1 means no minimum.
 *   flags     - bitwise OR of FLAG_* constants above.
 * Returns: number of characters written to buf (never writes NUL).
 * Locking: none (stateless aside from static digit tables).
 *
 * NOTE(LIB-VSNPRINTF-01): width is decremented by the digit count (i) before
 *   the sign character is emitted.  When FLAG_ZEROPAD is set, the sign is
 *   written first and then zero-padding fills width characters.  But width was
 *   already reduced by the digit count only — not by 1 for the sign char — so
 *   total output is (sign + width zeros + digits), which is one character more
 *   than the requested field width.  Example: print_num(..., -42, 10, 5, -1,
 *   FLAG_ZEROPAD|FLAG_SIGN) emits "-00042" (6 chars) instead of "-0042".
 */
static int print_num(char *buf, size_t size, uint64_t num, int base, int width, int precision, int flags) {
    static const char digits_lower[] = "0123456789abcdef";
    static const char digits_upper[] = "0123456789ABCDEF";
    const char *digits = (flags & FLAG_UPPERCASE) ? digits_upper : digits_lower;

    /* tmp[]: digits stored in reverse (least-significant first); up to 64 bits
     * in base 2 = 64 digits + possible overflow byte = 66 is a safe bound. */
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

    /* Precision: pad digit string with leading '0's to reach minimum digit count.
     * The extra '0' bytes go into tmp[] alongside the real digits. */
    while (i < precision && i < 64) {
        tmp[i++] = '0';
    }

    /* width now holds the net padding remaining after the digits are accounted for.
     * NOTE(LIB-VSNPRINTF-01): sign char (1 byte) is NOT subtracted from width here,
     * so FLAG_ZEROPAD output is one character wider than the requested field. */
    width -= i;

    /* Right-justify with space padding (only when neither zero-pad nor left-align) */
    if (!(flags & (FLAG_ZEROPAD | FLAG_LEFT))) {
        while (width-- > 0 && written < (int)size - 1) buf[written++] = ' ';
    }

    /* Sign / prefix character */
    if (flags & FLAG_SIGN) {
        if (written < (int)size - 1) buf[written++] = '-';
    } else if (flags & FLAG_PLUS) {
        if (written < (int)size - 1) buf[written++] = '+';
    } else if (flags & FLAG_SPACE) {
        if (written < (int)size - 1) buf[written++] = ' ';
    }

    /* Zero-pad between sign and digits when FLAG_ZEROPAD is active */
    if (flags & FLAG_ZEROPAD) {
        while (width-- > 0 && written < (int)size - 1) buf[written++] = '0';
    }

    /* Emit digits in correct (forward) order by reading tmp[] in reverse */
    while (i > 0 && written < (int)size - 1) {
        buf[written++] = tmp[--i];
    }

    /* Left-justify trailing space padding */
    if (flags & FLAG_LEFT) {
        while (width-- > 0 && written < (int)size - 1) buf[written++] = ' ';
    }

    return written;
}

/*
 * vsnprintf - format a string into buf using at most size bytes (including NUL).
 *
 * This is the kernel's bounded printf engine; all formatted output passes through
 * here.  The function is compiled for both kernel and userland (see file header).
 *
 * Params:
 *   buf  - destination buffer; must be at least 'size' bytes.
 *   size - total capacity of buf; if 0, returns 0 and writes nothing.
 *   fmt  - printf-style format string.
 *   args - va_list of format arguments; caller owns va_start/va_end.
 * Returns: number of characters written, NOT including the NUL terminator.
 *   NOTE(LIB-VSNPRINTF-02): returns chars written (< size), NOT the total chars
 *   needed if the buffer were unbounded.  Truncation is not detectable from the
 *   return value alone; this is not POSIX-conformant vsnprintf behaviour.
 * Locking: none (no global state modified).
 * Side effects: writes to buf; always NUL-terminates if size > 0.
 *
 * Format string parsing loop:
 *   For each '%' the loop parses:
 *     1. Zero or more flag characters (-, +, space, #, 0).
 *     2. Optional decimal field width.
 *     3. Optional '.' followed by decimal precision.
 *     4. Optional length modifier (l, ll, z).
 *     5. Conversion specifier character.
 *
 * NOTE(LIB-VSNPRINTF-03): %o (octal) is not implemented; %e/%f are absent
 *   (acceptable since the kernel does not use the FPU).
 */
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

        /* Parse flags: accumulate all flag characters before width/precision */
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

        /* Field width: decimal digits after flags, before '.' or specifier */
        width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Precision: optional '.' followed by decimal digits.
         * -1 means no precision was specified. */
        precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Length modifiers:
         *   l  → long (is_long=1)
         *   ll → long long / int64_t (is_long=2)
         *   z  → size_t (treated as 32- or 64-bit per sizeof) */
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
                /* char is promoted to int through va_arg */
                if (written < (int)size - 1) buf[written++] = (char)va_arg(args, int);
                break;

            case 's':
                s = va_arg(args, const char *);
                if (!s) s = "(null)";
                /* NOTE: width and precision are not applied to %s in this implementation */
                while (*s && written < (int)size - 1) buf[written++] = *s++;
                break;

            case 'd':
            case 'i':
                /* Signed integer: extract sign bit, negate, then format unsigned */
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
                /* Unsigned integer or hex: fetch without sign extension */
                if (is_long == 2)      num = va_arg(args, uint64_t);
                else if (is_long == 1) num = va_arg(args, unsigned long);
                else                  num = va_arg(args, unsigned int);

                if (*fmt == 'X') flags |= FLAG_UPPERCASE;
                written += print_num(buf + written, size - written, num, (*fmt == 'u' ? 10 : 16), width, precision, flags);
                break;

            case 'p':
                /* Pointer: emit "0x" prefix then 16 zero-padded hex digits.
                 * NOTE(LIB-VSNPRINTF-04): width is hardcoded to 16; the guard
                 *   `written < (int)size - 2` prevents writing "0x" near a full
                 *   buffer, but print_num still runs with near-zero remaining
                 *   capacity.  The 0x prefix may be omitted while digits still
                 *   appear, corrupting the pointer representation. */
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
                /* Unknown specifier: pass through literally as "%<char>" */
                if (written < (int)size - 1) buf[written++] = '%';
                if (written < (int)size - 1) buf[written++] = *fmt;
                break;
        }
        fmt++;
    }

    /* Always NUL-terminate; buf[written] is guaranteed in-bounds because
     * the loop condition is `written < (int)size - 1`. */
    buf[written] = '\0';
    return written;
}
