#include "lua_portability.h"
#include <os1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Locale implementation */
char *setlocale(int category, const char *locale) {
  (void)category;
  (void)locale;
  return "C";
}

struct lconv *localeconv(void) {
  static struct lconv c_locale = {
    .decimal_point = ".",
    .thousands_sep = "",
    .grouping = "",
    .int_curr_symbol = "",
    .currency_symbol = "",
    .mon_decimal_point = "",
    .mon_thousands_sep = "",
    .mon_grouping = "",
    .positive_sign = "",
    .negative_sign = "",
    .int_frac_digits = 127,
    .frac_digits = 127,
    .p_cs_precedes = 127,
    .p_sep_by_space = 127,
    .n_cs_precedes = 127,
    .n_sep_by_space = 127,
    .p_sign_posn = 127,
    .n_sign_posn = 127
  };
  return &c_locale;
}

/* Time implementation */
time_t time(time_t *t) {
  time_t sec = (time_t)get_time();
  if (t) {
    *t = sec;
  }
  return sec;
}

struct tm *localtime(const time_t *timep) {
  static struct tm result;
  if (!timep) return NULL;
  
  time_t t = *timep;
  result.tm_sec = t % 60; t /= 60;
  result.tm_min = t % 60; t /= 60;
  result.tm_hour = t % 24; t /= 24;
  
  /* Epoch started Jan 1 1970, which was Thursday (wday = 4) */
  result.tm_wday = (t + 4) % 7;
  
  int year = 1970;
  while (1) {
    int days_in_year = 365;
    if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
      days_in_year = 366;
    }
    if (t < days_in_year) break;
    t -= days_in_year;
    year++;
  }
  result.tm_year = year - 1900;
  result.tm_yday = t;
  
  int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
    month_days[1] = 29;
  }
  
  int mon = 0;
  while (t >= month_days[mon]) {
    t -= month_days[mon];
    mon++;
  }
  result.tm_mon = mon;
  result.tm_mday = t + 1;
  result.tm_isdst = 0;
  return &result;
}

struct tm *gmtime(const time_t *timep) {
  return localtime(timep);
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
  size_t written = 0;
  const char *p = format;
  while (*p && written < max - 1) {
    if (*p == '%') {
      p++;
      if (!*p) break;
      int len = 0;
      switch (*p) {
        case 'Y': len = snprintf(s + written, max - written, "%d", tm->tm_year + 1900); break;
        case 'm': len = snprintf(s + written, max - written, "%02d", tm->tm_mon + 1); break;
        case 'd': len = snprintf(s + written, max - written, "%02d", tm->tm_mday); break;
        case 'H': len = snprintf(s + written, max - written, "%02d", tm->tm_hour); break;
        case 'M': len = snprintf(s + written, max - written, "%02d", tm->tm_min); break;
        case 'S': len = snprintf(s + written, max - written, "%02d", tm->tm_sec); break;
        default:
          s[written++] = '%';
          if (written < max - 1) s[written++] = *p;
          len = 0;
          break;
      }
      if (len > 0) {
        written += len;
      }
    } else {
      s[written++] = *p;
    }
    p++;
  }
  s[written] = '\0';
  return written;
}

time_t mktime(struct tm *tm) {
  int year = tm->tm_year + 1900;
  int mon = tm->tm_mon;
  int mday = tm->tm_mday;
  
  long days = 0;
  for (int y = 1970; y < year; y++) {
    days += ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 366 : 365;
  }
  int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
    month_days[1] = 29;
  }
  for (int m = 0; m < mon; m++) {
    days += month_days[m];
  }
  days += mday - 1;
  
  return days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

/* Double parsing */
double strtod(const char *nptr, char **endptr) {
  const char *p = nptr;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') {
    p++;
  }
  
  double sign = 1.0;
  if (*p == '-') {
    sign = -1.0;
    p++;
  } else if (*p == '+') {
    p++;
  }
  
  double val = 0.0;
  while (*p >= '0' && *p <= '9') {
    val = val * 10.0 + (*p - '0');
    p++;
  }
  
  if (*p == '.') {
    p++;
    double dec = 0.1;
    while (*p >= '0' && *p <= '9') {
      val += (*p - '0') * dec;
      dec *= 0.1;
      p++;
    }
  }
  
  if (*p == 'e' || *p == 'E') {
    p++;
    double exp_sign = 1.0;
    if (*p == '-') {
      exp_sign = -1.0;
      p++;
    } else if (*p == '+') {
      p++;
    }
    int exp_val = 0;
    while (*p >= '0' && *p <= '9') {
      exp_val = exp_val * 10 + (*p - '0');
      p++;
    }
    double factor = 1.0;
    double base = 10.0;
    int e = exp_val;
    while (e > 0) {
      if (e & 1) {
        factor *= base;
      }
      base *= base;
      e >>= 1;
    }
    if (exp_sign < 0) {
      val /= factor;
    } else {
      val *= factor;
    }
  }
  
  if (endptr) {
    *endptr = (char *)p;
  }

  return sign * val;
}

/* ---- Math functions used by Lua core and lmathlib ----
 * NexsOS1's <math.h> only ships fabs; everything else below is a
 * minimal implementation. Accuracy is "good enough" for Lua's numeric
 * mode (where Lua Number is double). For tight numeric work, replace
 * with libm-equivalents. */

static double ipow10(int n) {
  double r = 1.0;
  double b = 10.0;
  int neg = 0;
  if (n < 0) { neg = 1; n = -n; }
  while (n > 0) {
    if (n & 1) r *= b;
    b *= b;
    n >>= 1;
  }
  return neg ? 1.0 / r : r;
}

double ldexp(double x, int exp) {
  /* x * 2^exp implemented by repeated squaring on the exponent. */
  double r = x;
  if (exp == 0) return r;
  int neg = 0;
  unsigned int e = (unsigned int)exp;
  if ((int)e < 0) { neg = 1; e = (unsigned int)(-(int)e); }
  double base = 2.0;
  double factor = 1.0;
  while (e) {
    if (e & 1u) factor *= base;
    base *= base;
    e >>= 1;
  }
  return neg ? r / factor : r * factor;
}

double frexp(double x, int *exp) {
  if (x == 0.0) { if (exp) *exp = 0; return 0.0; }
  int e = 0;
  double y = x;
  double ax = x < 0 ? -x : x;
  if (ax >= 1.0) {
    while (ax >= 1.0) { ax *= 0.5; e++; }
  } else {
    while (ax < 0.5) { ax *= 2.0; e--; }
  }
  if (exp) *exp = e;
  return x < 0 ? -ax : ax;
}

double modf(double x, double *iptr) {
  double intpart = (double)((long long)x);
  /* adjust for negative non-integer values */
  if (x < 0.0 && x != intpart) intpart -= 1.0;
  if (iptr) *iptr = intpart;
  return x - intpart;
}

double floor(double x) {
  long long i = (long long)x;
  if (x < 0.0 && x != (double)i) i -= 1;
  return (double)i;
}

double ceil(double x) {
  long long i = (long long)x;
  if (x > 0.0 && x != (double)i) i += 1;
  return (double)i;
}

double fmod(double x, double y) {
  if (y == 0.0) return 0.0; /* unspecified; return 0 to avoid NaN */
  long long q = (long long)(x / y);
  /* truncate toward zero */
  double r = x - (double)q * y;
  if ((r != 0.0) && ((r < 0.0) != (y < 0.0))) r += y;
  return r;
}

double pow(double x, double y) {
  if (y == 0.0) return 1.0;
  if (x == 0.0) return 0.0;
  if (y == (double)(long long)y) {
    /* integer exponent fast path */
    int neg = 0;
    long long n = (long long)y;
    if (n < 0) { neg = 1; n = -n; }
    double r = 1.0;
    double b = x;
    while (n) {
      if (n & 1) r *= b;
      b *= b;
      n >>= 1;
    }
    return neg ? 1.0 / r : r;
  }
  /* exp(y * log(x)) using series for log/exp would be heavy;
   * use identity pow(x,y) = 10^(y * log10(x)) with simple log10. */
  /* log10 via frexp + table-less series: bit weak, but functional. */
  if (x < 0.0) return 0.0 / 0.0; /* NaN */
  double log10x;
  {
    int e;
    double m = frexp(x, &e);
    /* m in [0.5, 1) */
    log10x = (double)e * 0.30102999566398119521;
    /* add log10(m) ~ log10(1 - (1-m)) using a few terms */
    double u = 1.0 - m;
    /* log10(1-u) = ln(1-u)/ln(10) — keep coarse */
    double s = 0.0;
    double term = -u;
    for (int k = 1; k < 16; k++) {
      s += term / (double)k;
      term *= -u;
    }
    log10x += s * 0.43429448190325182765;
  }
  double p = y * log10x;
  /* 10^p = 10^floor(p) * 10^frac(p) */
  long long ip = (long long)p;
  if (p < 0 && (double)ip != p) ip -= 1;
  double frac = p - (double)ip;
  /* 10^frac using small Taylor: e^(frac*ln(10)) */
  double t = frac * 2.30258509299404568402;
  double es = 1.0 + t * (1.0 + t * (0.5 + t * (1.0/6.0 + t * (1.0/24.0 + t * (1.0/120.0)))));
  return ipow10((int)ip) * es;
}

double sqrt(double x) {
  if (x < 0.0) return 0.0 / 0.0;
  if (x == 0.0) return 0.0;
  double r = x;
  for (int i = 0; i < 32; i++) r = 0.5 * (r + x / r);
  return r;
}

double sin(double x) {
  /* reduce to [-pi, pi] */
  const double pi = 3.14159265358979323846;
  double y = x;
  /* rough reduction */
  double k = (long long)(y / (2.0 * pi));
  y -= k * 2.0 * pi;
  /* Taylor */
  double s = 0.0;
  double term = y;
  for (int n = 1; n < 25; n += 2) {
    s += term;
    term *= -y * y / ((double)(n + 1) * (double)(n + 2));
  }
  return s;
}

double cos(double x) { return sin(x + 1.57079632679489661923); }
double tan(double x)  { double c = cos(x); return c == 0.0 ? 0.0/0.0 : sin(x)/c; }
double asin(double x) { return 0.0; /* placeholder; not exercised by lcode.c */ }
double acos(double x) { return 0.0; }
double atan(double x) { return 0.0; }
double atan2(double y, double x) { return 0.0; }
double log(double x)  {
  if (x <= 0.0) return 0.0 / 0.0;
  int e;
  double m = frexp(x, &e);
  double ln2 = 0.693147180559945309417;
  double u = 1.0 - m;
  double s = 0.0;
  double term = -u;
  for (int k = 1; k < 16; k++) {
    s += term / (double)k;
    term *= -u;
  }
  return s + (double)e * ln2;
}
double log10(double x){ return log(x) * 0.43429448190325182765; }
double log2(double x) { return log(x) * 1.44269504088896340736; }
double exp(double x)  {
  double s = 1.0;
  double t = 1.0;
  for (int n = 1; n < 32; n++) { t *= x / (double)n; s += t; }
  return s;
}
double cosh(double x) { double e = exp(x); return (e + 1.0/e) * 0.5; }
double sinh(double x) { double e = exp(x); return (e - 1.0/e) * 0.5; }
double tanh(double x) { double e = exp(2.0*x); return (e - 1.0)/(e + 1.0); }

char *tmpnam(char *s) {
  static char static_buf[128];
  static int temp_counter = 0;
  char *buf = s ? s : static_buf;
  sprintf(buf, "/tmpfile_%d", temp_counter++);
  return buf;
}

clock_t clock(void) {
  return (clock_t)os1_cpu_ns();
}

double difftime(time_t time1, time_t time0) {
  return (double)(time1 - time0);
}

int strcoll(const char *s1, const char *s2) {
  return strcmp(s1, s2);
}

/*
 * os1_lua_readline - REPL line reader for nxlua (lua_portability.h's
 * lua_readline override; see that header for why fgets(stdin) is replaced).
 *
 * NOTE(LUA-TTY-01): fgets()/OS1 console read() hand back keyboard.c's BASE
 * ascii_map byte (data1's low byte), not the character after the active
 * keyboard layout's overrides (payload) - the same .key vs .utf8 split
 * input_event_t already exposes for windowed apps. nxlua has no window, so
 * it decodes its own mailbox here instead. Echo is NOT done here: nxexec.h's
 * host-side relay (USR-TTY-01 #123) already echoes every byte it forwards,
 * in the same style as nxshell.c's read(0,...) loop; echoing again here
 * would double every character on screen.
 *
 * Returns 1 with buf NUL-terminated and ending in '\n' (matches fgets(),
 * which pushline() in lua.c strips itself), or 0 on Ctrl-C (EOF-like).
 */
int os1_lua_readline(char *buf, int bufsize) {
  int len = 0;
  while (1) {
    struct ipc_message m;
    if (try_recv(-1, &m) != 0 || m.type != IPC_TYPE_INPUT || m.data2 == 0) {
      OS1_sleep(15);
      continue;
    }
    char c = m.payload[0];
    if (c == 0x03) /* Ctrl-C: EOF-like */
      return 0;
    if (c == '\r' || c == '\n') {
      if (len + 1 >= bufsize)
        len = bufsize - 2;
      buf[len++] = '\n';
      buf[len] = '\0';
      return 1;
    }
    if (c == '\b' || c == 127) {
      if (len > 0)
        len--;
      continue;
    }
    if (c != 0 && len + 1 < bufsize) {
      buf[len++] = c;
      buf[len] = '\0';
    }
  }
}

