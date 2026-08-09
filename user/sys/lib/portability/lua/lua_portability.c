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
  static struct lconv c_locale = {.decimal_point = ".",
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
                                  .n_sign_posn = 127};
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

struct tm *gmtime(const time_t *timep) { return localtime(timep); }

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
  size_t written = 0;
  const char *p = format;
  while (*p && written < max - 1) {
    if (*p == '%') {
      p++;
      if (!*p)
        break;
      int len = 0;
      switch (*p) {
      case 'Y':
        len = snprintf(s + written, max - written, "%d", tm->tm_year + 1900);
        break;
      case 'm':
        len = snprintf(s + written, max - written, "%02d", tm->tm_mon + 1);
        break;
      case 'd':
        len = snprintf(s + written, max - written, "%02d", tm->tm_mday);
        break;
      case 'H':
        len = snprintf(s + written, max - written, "%02d", tm->tm_hour);
        break;
      case 'M':
        len = snprintf(s + written, max - written, "%02d", tm->tm_min);
        break;
      case 'S':
        len = snprintf(s + written, max - written, "%02d", tm->tm_sec);
        break;
      default:
        s[written++] = '%';
        if (written < max - 1)
          s[written++] = *p;
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


char *tmpnam(char *s) {
  static char static_buf[128];
  static int temp_counter = 0;
  char *buf = s ? s : static_buf;
  /* /home is the ONLY user-writable tree (kernel/fs/vfs.c vfs_write_allowed);
   * the old "/tmpfile_%d" sat at the non-writable root, so os.tmpname()'s path
   * could never be created even once file creation via SYS_FILE_WRITE landed.
   * (fopen("w") still needs open(2) to honour O_CREAT for these to fully work
   * — tracked separately; this at least puts the name in the right tree.) */
  sprintf(buf, "/home/.luatmp_%d", temp_counter++);
  return buf;
}

clock_t clock(void) { return (clock_t)os1_cpu_ns(); }

double difftime(time_t time1, time_t time0) { return (double)(time1 - time0); }

int strcoll(const char *s1, const char *s2) { return strcmp(s1, s2); }

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
  /*
   * NOTE(LUA-TTY-03): the keyboard-mailbox path below is ONLY correct for a
   * real console.  `-i` FORCES interactive mode regardless of
   * lua_stdin_is_tty(), so `lua -i < script` and `echo ... | lua -i` reach the
   * REPL with stdin redirected to a FILE or a PIPE — there the line must come
   * from fd 0, not from the keyboard (otherwise the REPL ignores the supplied
   * program and blocks on a keypress that never comes).  isatty() is the same
   * capability-type test the tty check uses, so the two agree by construction.
   */
  if (!isatty(0))
    return fgets(buf, bufsize, stdin) ? 1 : 0; /* NULL => EOF, like Ctrl-C */

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