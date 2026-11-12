/*
 * user/sys/lib/portability/gnulib/os1_time.c
 * NexsOS1 time function implementations for Gnulib portability layer.
 * These are the canonical implementations for localtime(), gmtime(), and mktime()
 * backed directly by OS1 syscalls, and Gnulib's time module replacements are not used.
 */

#include <time.h>
#include <os1.h>
#include <string.h>

time_t time(time_t *t) {
  time_t sec = (time_t)OS1_time_now();
  if (t) {
    *t = sec;
  }
  return sec;
}

struct tm *localtime(const time_t *timep) {
  static struct tm result;
  if (!timep)
    return NULL;

  time_t t = *timep;
  result.tm_sec = t % 60;
  t /= 60;
  result.tm_min = t % 60;
  t /= 60;
  result.tm_hour = t % 24;
  t /= 24;

  /* Epoch started Jan 1 1970, which was Thursday (wday = 4) */
  result.tm_wday = (t + 4) % 7;

  int year = 1970;
  while (1) {
    int days_in_year = 365;
    if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
      days_in_year = 366;
    }
    if (t < days_in_year)
      break;
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

static const char *const os1_weekday_names[] = {
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *const os1_month_names[] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
  if (!s || !format || !tm || max == 0) {
    return 0;
  }

  size_t out = 0;
  const char *p = format;

  while (*p && out + 1 < max) {
    if (*p != '%') {
      s[out++] = *p++;
      continue;
    }

    p++;
    if (!*p) {
      break;
    }

    char temp[64];
    int n = 0;

    switch (*p) {
      case '%':
        s[out++] = '%';
        break;
      case 'a':
        n = snprintf(temp, sizeof(temp), "%s", os1_weekday_names[(tm->tm_wday + 7) % 7]);
        break;
      case 'A':
        n = snprintf(temp, sizeof(temp), "%s", os1_weekday_names[(tm->tm_wday + 7) % 7]);
        break;
      case 'b':
        n = snprintf(temp, sizeof(temp), "%s", os1_month_names[(tm->tm_mon + 12) % 12]);
        break;
      case 'B':
        n = snprintf(temp, sizeof(temp), "%s", os1_month_names[(tm->tm_mon + 12) % 12]);
        break;
      case 'd':
        n = snprintf(temp, sizeof(temp), "%02d", tm->tm_mday);
        break;
      case 'e':
        n = snprintf(temp, sizeof(temp), "%2d", tm->tm_mday);
        break;
      case 'H':
        n = snprintf(temp, sizeof(temp), "%02d", tm->tm_hour);
        break;
      case 'I':
        n = snprintf(temp, sizeof(temp), "%02d", (tm->tm_hour % 12) ? (tm->tm_hour % 12) : 12);
        break;
      case 'j':
        n = snprintf(temp, sizeof(temp), "%03d", tm->tm_yday + 1);
        break;
      case 'm':
        n = snprintf(temp, sizeof(temp), "%02d", tm->tm_mon + 1);
        break;
      case 'M':
        n = snprintf(temp, sizeof(temp), "%02d", tm->tm_min);
        break;
      case 'n':
        s[out++] = '\n';
        break;
      case 'p':
        n = snprintf(temp, sizeof(temp), "%s", (tm->tm_hour >= 12) ? "PM" : "AM");
        break;
      case 'r':
        n = snprintf(temp, sizeof(temp), "%02d:%02d:%02d %s",
                     (tm->tm_hour % 12) ? (tm->tm_hour % 12) : 12,
                     tm->tm_min, tm->tm_sec,
                     (tm->tm_hour >= 12) ? "PM" : "AM");
        break;
      case 'R':
        n = snprintf(temp, sizeof(temp), "%02d:%02d",
                     tm->tm_hour, tm->tm_min);
        break;
      case 'S':
        n = snprintf(temp, sizeof(temp), "%02d", tm->tm_sec);
        break;
      case 'T':
        n = snprintf(temp, sizeof(temp), "%02d:%02d:%02d",
                     tm->tm_hour, tm->tm_min, tm->tm_sec);
        break;
      case 'Y':
        n = snprintf(temp, sizeof(temp), "%d", tm->tm_year + 1900);
        break;
      case 'y':
        n = snprintf(temp, sizeof(temp), "%02d", (tm->tm_year + 1900) % 100);
        break;
      case 'z':
      case 'Z':
        temp[0] = '\0';
        n = 0;
        break;
      case 't':
        s[out++] = '\t';
        break;
      default:
        s[out++] = *p;
        break;
    }

    if (n > 0) {
      size_t copy = (size_t)n;
      if (copy >= max - out) {
        copy = max - out - 1;
      }
      memcpy(s + out, temp, copy);
      out += copy;
    }

    p++;
    if (*p == '\0' && out + 1 < max) {
      break;
    }
  }

  s[out] = '\0';
  return out;
}

bool hard_locale(int category) {
  (void)category;
  return false;
}
